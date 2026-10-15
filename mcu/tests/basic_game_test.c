/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include "disk2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static apple2_t machine;
static disk2_t disk;
static uint8_t rom[16384], slot_rom[256];
static FILE *image;

static bool read_track(void *context, unsigned track, uint8_t output[4096])
{
    (void)context;
    return !fseek(image, track * 4096, SEEK_SET) && fread(output, 1, 4096, image) == 4096;
}
static bool write_track(void *context, unsigned track, const uint8_t input[4096])
{
    (void)context;
    return !fseek(image, track * 4096, SEEK_SET) && fwrite(input, 1, 4096, image) == 4096 &&
           !fflush(image) && !fsync(fileno(image));
}
static void screen(void)
{
    char line[41];
    printf("PC=%04x cycles=%llu disk_error=%u line=%u\n", machine.cpu.PC,
           (unsigned long long)machine.cpu.total_cycle, disk.error,
           machine.ram[0x75] | machine.ram[0x76] << 8);
    for (unsigned row = 0; row < 24; ++row) {
        apple2_text_line(&machine, row, line); puts(line);
    }
}
static void check(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); screen(); exit(1); }
}
static bool contains(const char *text)
{
    char line[41];
    for (unsigned row = 0; row < 24; ++row) {
        apple2_text_line(&machine, row, line);
        if (strstr(line, text)) return true;
    }
    return false;
}
static void run(unsigned instructions) { apple2_run(&machine, instructions); }
static void await_text(const char *text)
{
    for (unsigned i = 0; i < 3000 && !contains(text); ++i) run(10000);
    check(contains(text), text);
}
static void ready(void)
{
    unsigned matches = 0;
    for (unsigned i = 0; i < 6000 && matches < 3; ++i) {
        run(5000);
        if (!machine.key_ready && machine.cpu.PC >= 0xc27d && machine.cpu.PC <= 0xc28e) ++matches;
        else matches = 0;
    }
    check(matches == 3, "DOS keyboard prompt ready");
}
static void type(const char *text)
{
    for (; *text; ++text) {
        unsigned waits = 0;
        while (machine.key_ready && ++waits < 3000) run(1000);
        check(!machine.key_ready && apple2_key(&machine, *text), "key accepted");
        run(2000);
    }
    for (unsigned waits = 0; machine.key_ready && waits < 3000; ++waits) run(1000);
    check(!machine.key_ready, "last key consumed");
}
static uint8_t *variable(const char *name)
{
    const unsigned start = machine.ram[0x69] | machine.ram[0x6a] << 8;
    const unsigned end = machine.ram[0x6b] | machine.ram[0x6c] << 8;
    check(start <= end && end < sizeof(machine.ram), "Applesoft variable bounds");
    for (unsigned offset = start; offset + 7 <= end; offset += 7)
        if (machine.ram[offset] == (uint8_t)name[0] && machine.ram[offset + 1] == (uint8_t)name[1])
            return machine.ram + offset + 2;
    return NULL;
}
static double value(const char *name)
{
    const uint8_t *p = variable(name);
    if (!p) return NAN;
    if (!p[0]) return 0;
    const double mantissa = 1 + (p[1] & 127) / 128.0 + p[2] / 32768.0 +
                            p[3] / 8388608.0 + p[4] / 2147483648.0;
    return ldexp(p[1] & 128 ? -mantissa : mantissa, p[0] - 129);
}
static void set_value(const char *name, int number)
{
    uint8_t *p = variable(name);
    check(p != NULL, "game numeric variable exists");
    memset(p, 0, 5);
    if (!number) return;
    unsigned magnitude = number < 0 ? -number : number, exponent = 0;
    while ((1u << (exponent + 1)) <= magnitude) ++exponent;
    const uint32_t fraction = (uint32_t)((uint64_t)(magnitude - (1u << exponent)) << (31 - exponent));
    p[0] = 129 + exponent;
    p[1] = (number < 0 ? 128 : 0) | (fraction >> 24);
    p[2] = fraction >> 16; p[3] = fraction >> 8; p[4] = fraction;
}
static void await_value(const char *name, int expected)
{
    for (unsigned i = 0; i < 10000 && value(name) != expected; ++i) run(128);
    check(value(name) == expected, name);
}
static void at_game_loop(void)
{
    for (unsigned i = 0; i < 10000; ++i) {
        const unsigned line = machine.ram[0x75] | machine.ram[0x76] << 8;
        if (line == 100) return;
        run(32);
    }
    check(false, "game input loop reached");
}
static void next_game_loop(void)
{
    for (unsigned i = 0; i < 10000; ++i) {
        run(32);
        if ((machine.ram[0x75] | machine.ram[0x76] << 8) != 100) {
            at_game_loop();
            return;
        }
    }
    check(false, "game loop advanced");
}
static unsigned ball_pixels(void)
{
    const unsigned base=machine.page_two ? 0x800 : 0x400;
    unsigned count=0;
    for (unsigned y=0;y<40;++y) {
        const unsigned row=y/2;
        const unsigned address=base+((row&7)<<7)+(row>>3)*40;
        for (unsigned x=0;x<40;++x)
            if (((machine.ram[address+x]>>((y&1)*4))&15)==15) ++count;
    }
    return count;
}
static void seed_ball_pixel(unsigned x,unsigned y)
{
    /* Keep the framebuffer consistent when constructing the deterministic
     * missed-paddle state below; prior tests also reposition BASIC variables. */
    const unsigned base=machine.page_two ? 0x800 : 0x400;
    for (unsigned row=0;row<20;++row) {
        const unsigned address=base+((row&7)<<7)+(row>>3)*40;
        for (unsigned col=0;col<40;++col) {
            uint8_t *pixel=machine.ram+address+col;
            if ((*pixel&15)==15) *pixel=(*pixel&0xf0)|1;
            if ((*pixel>>4)==15) *pixel=(*pixel&0x0f)|0x10;
        }
    }
    const unsigned row=y/2;
    const unsigned address=base+((row&7)<<7)+(row>>3)*40+x;
    machine.ram[address]|=(uint8_t)(15<<((y&1)*4));
    check(ball_pixels()==1,"deterministic missed-paddle ball seeded");
}
static unsigned pixel(unsigned x,unsigned y)
{
    const unsigned row=y/2;
    return (machine.ram[0x400+((row&7)<<7)+(row>>3)*40+x]>>((y&1)*4))&15;
}
static void frame(const char *name)
{
    const char *directory=getenv("A2_GAME_FRAMES_DIR");
    if (!directory) return;
    char path[1024];
    check(snprintf(path,sizeof(path),"%s/%s.ram",directory,name)<(int)sizeof(path),"frame path fits");
    FILE *output=fopen(path,"wb");
    check(output && fwrite(machine.ram,1,sizeof(machine.ram),output)==sizeof(machine.ram),"frame captured");
    check(!fclose(output),"frame closed");
}
static void position_paddle(unsigned expected)
{
    at_game_loop();double current=value("P");
    if (expected!=0 && expected!=34 && (((unsigned)current^expected)&1)) {
        machine.buttons=16;await_value("P",0);current=0;
    }
    if (current!=expected) {
        machine.buttons=current>expected?16:128;await_value("P",expected);
    }
    machine.buttons=0;at_game_loop();
}
static void steering_step(uint8_t buttons,uint8_t key,unsigned expected,const char *message)
{
    at_game_loop();machine.buttons=buttons;
    if (key) check(apple2_key(&machine,key),"steering fixture key accepted");
    next_game_loop();check(value("P")==expected,message);
}
static void ball(unsigned x,unsigned y,int dx,int dy)
{
    at_game_loop();set_value("X",x);set_value("Y",y);
    set_value("DX",dx);set_value("DY",dy);seed_ball_pixel(x,y);
}
static void pace(const char *label,uint8_t buttons,bool active)
{
    position_paddle(34);ball(18,10,-1,1);set_value("SP",1);
    machine.buttons=buttons;const uint64_t start=machine.cpu.total_cycle;
    for (unsigned step=0;step<16;++step) {
        if (active) machine.buttons=step&1?128:16;
        next_game_loop();
        check(value("X")==17-step && value("Y")==11+step,"pace follows the same clear-field path");
        if (active) check(value("P")==((step&1)?34:32),"held reversal takes effect next loop");
    }
    const uint64_t elapsed=machine.cpu.total_cycle-start;
    check(16.0*1023000/elapsed >= (active ? 14.7:19.0),"retains the previous game speed improvements");
    check(ball_pixels()==1 && value("P")==34,"pace ends with visible ball and bounded paddle");
    printf("BREAKOUT %s: %.1f cycles/step, %.2f steps/s at 1.023 MHz\n",label,elapsed/16.0,16.0*1023000/elapsed);
    machine.buttons=0;
}

static void visible_game_loop(void)
{
    at_game_loop();
    check(ball_pixels()==1,"one ball before monitored loop");
    bool advanced=false;
    for (unsigned i=0;i<200000;++i) {
        run(1);
        /* A framebuffer can be sampled at any instruction, including inside
         * the ROM's PLOT routine; endpoint-only checks miss erase/draw gaps. */
        check(ball_pixels()>=1,"ball remains visible between CPU instructions");
        const unsigned line=machine.ram[0x75] | machine.ram[0x76]<<8;
        if (line!=100) advanced=true;
        if (advanced && line==100) {
            check(ball_pixels()==1,"one ball after monitored loop");
            return;
        }
    }
    check(false,"monitored game loop completed");
}
static void load_source(const char *path)
{
    FILE *source = fopen(path, "r");
    check(source != NULL, "BASIC source opened");
    char line[256];
    type("NEW\r"); ready();
    while (fgets(line, sizeof(line), source)) {
        const size_t n = strcspn(line, "\r\n");
        check(n + 2 < sizeof(line), "BASIC source line fits editor");
        line[n] = '\r'; line[n + 1] = 0;
        type(line); ready();
        check(!contains("?SYNTAX ERROR"), "BASIC source tokenized");
    }
    fclose(source);
}
static void language_test(void)
{
    type("NEW\r"); ready();
    const char *lines[] = {
        "10 DIM A(4):S=0\r",
        "20 FOR I=1 TO 4:A(I)=I*I:S=S+A(I):NEXT\r",
        "30 DEF FN F(X)=X*2+1\r",
        "40 A$=\"APPLE\":B$=LEFT$(A$,3)+RIGHT$(A$,2)\r",
        "50 IF S<>30 OR FN F(3)<>7 OR B$<>\"APPLE\" THEN PRINT \"LANGUAGE FAIL\":END\r",
        "60 INPUT \"NUMBER\";N\r",
        "70 IF N=7 THEN PRINT \"LANGUAGE PASS\":END\r",
        "80 PRINT \"LANGUAGE FAIL\":END\r"
    };
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i) { type(lines[i]); ready(); }
    type("HOME:RUN\r"); run(100000); await_text("NUMBER"); type("7\r");
    await_text("LANGUAGE PASS"); ready();
    check(value("N") == 7 && value("S") == 30, "runtime INPUT/array arithmetic values");
    check(!contains("?SYNTAX ERROR") && !contains("?ILLEGAL QUANTITY"), "language program without ROM errors");
    puts("PASS Applesoft arithmetic, FOR/NEXT, arrays, DEF FN, strings, comparisons and INPUT");
}
static void game_test(void)
{
    type("HOME:RUN\r");run(100000);await_text("A / SPACE TO SERVE");
    check(!machine.text_mode && machine.mixed_mode && !machine.hires_mode,"game lores mixed mode");
    await_value("BR",160);await_value("L",5);await_value("W",1);at_game_loop();
    const unsigned px=(unsigned)value("P"),bx=(unsigned)value("X"),by=(unsigned)value("Y");
    for (unsigned y=0;y<40;++y) for (unsigned x=0;x<40;++x) {
        const unsigned expected=x==bx && y==by ? 15 : x==0 && y>=px && y<px+6 ? 6 :
            x>=20 && x<=34 && !(x&1) ? (((x-20)/2+y/2)&1 ? 13:9):1;
        check(pixel(x,y)==expected,"historical magenta field, left blue paddle and 160 alternating bricks");
    }
    check(contains("1 2 3 4 5 6 7 8") && contains("5 BALLS LEFT"),"historic point scale and five balls");
    check((machine.ram[0x73]|machine.ram[0x74]<<8)==0x9000,"native helper memory reserved below DOS");
    frame("ready");visible_game_loop();visible_game_loop();
    machine.buttons=16;await_value("P",0);machine.buttons=128;await_value("P",34);
    steering_step(16,10,32,"held UP overrides stale DOWN cursor");
    steering_step(128,11,34,"held DOWN reversal overrides stale UP cursor");
    steering_step(144,11,34,"opposed UP/DOWN stays neutral");
    steering_step(176,10,34,"opposed directions with RIGHT stays neutral");
    steering_step(48,10,32,"UP/RIGHT overrides stale DOWN cursor");
    steering_step(192,11,34,"DOWN/LEFT overrides stale UP cursor");
    steering_step(0,11,32,"cursor UP works after physical directions release");
    machine.buttons=0;at_game_loop();
    check(value("X")==bx && (unsigned)value("Y")==by,"waiting ball stays at its original center serve");
    puts("PASS historical layout, held UP/DOWN, opposed directions and keyboard fallback");
    machine.buttons=8;await_value("W",0);machine.buttons=0;at_game_loop();
    visible_game_loop();check(value("X")!=bx,"served ball travels left toward paddle");
    at_game_loop();set_value("H",1);ball(0,20,1,1);position_paddle(34);
    machine.buttons=16;
    for (unsigned step=0;step<8;++step) visible_game_loop();
    check(value("P")==18 && value("X")==0 && value("Y")==20,"paused ball remains visible under moving paddle");
    machine.buttons=0;ball(10,15,1,1);visible_game_loop();
    check(pixel(0,20)==6,"stationary paddle heals its vacated ball pixel");
    set_value("H",0);
    puts("PASS ball visibility at every instruction, including native paddle repaint");
    pace("clear field",0,false);pace("held DOWN at clamp",128,false);pace("active steering/redraw",0,true);
    at_game_loop();machine.buttons=4;await_value("H",1);run(100000);
    check(value("H")==1,"held B pauses only once");machine.buttons=0;run(10000);
    const double paused_x=value("X"),paused_y=value("Y");run(100000);
    check(value("X")==paused_x && value("Y")==paused_y,"B pause holds ball");
    machine.buttons=4;await_value("H",0);machine.buttons=0;
    at_game_loop();set_value("W",1);position_paddle(18);set_value("W",0);set_value("L",5);
    for (unsigned hit=0;hit<6;++hit) {
        ball(1,18+hit,-1,0);const unsigned edges=machine.speaker_edges;
        next_game_loop();
        check(value("DX")==1 && fabs(value("DY")-(2*(int)hit-5)/3.0)<1e-8,"paddle contact sets the original six rebound angles");
        check(value("L")==5 && machine.speaker_edges>edges,"paddle bounce preserves ball and clicks speaker");
    }
    at_game_loop();set_value("N",9);set_value("SP",1);ball(1,21,-1,0);next_game_loop();
    check(value("N")==10 && value("SP")==2 && value("DX")==2,"ten returns double horizontal ball speed");
    next_game_loop();check(value("X")==2,"faster ball advances two columns");
    ball(39,15,1,0);next_game_loop();check(value("DX")==-1 && value("X")==38,"right wall rebound");
    ball(10,0,1,-1);next_game_loop();check(value("DY")==1 && value("Y")==0,"upper boundary rebound");
    ball(10,39,1,1);next_game_loop();check(value("DY")==-1 && value("Y")==39,"lower boundary rebound");
    ball(1,0,-1,0);visible_game_loop();await_value("L",4);await_value("W",1);
    visible_game_loop();await_value("Q",0);
    check(value("X")==19 && value("N")==0 && value("SP")==1,"new ball starts centrally at initial speed");
    machine.buttons=8;await_value("W",0);machine.buttons=0;at_game_loop();
    set_value("L",5);set_value("SP",1);
    unsigned score=(unsigned)value("S"),bricks=(unsigned)value("BR");
    for (unsigned c=0;c<8;++c) for (unsigned r=0;r<20;++r) {
        if (pixel(20+2*c,2*r)==1) continue;
        ball(19+2*c,2*r,1,0);score+=c+1;--bricks;
        await_value("S",score);await_value("BR",bricks);
        if (bricks) {
            at_game_loop();check(pixel(20+2*c,2*r)==1 && pixel(20+2*c,2*r+1)==1,"each struck two-cell brick disappears");
            check(value("DX")==-1,"brick sends ball back left");
            if (c==0 && r==9) frame("playing");
        }
    }
    await_text("YOU WIN!");check(score==720 && value("S")==720,"all 160 bricks total a perfect 720 points");
    machine.buttons=0;run(10000);machine.buttons=8;await_value("S",0);machine.buttons=0;
    await_text("A / SPACE TO SERVE");await_value("W",1);
    machine.buttons=8;await_value("W",0);machine.buttons=0;position_paddle(18);
    for (unsigned remaining=4;;--remaining) {
        ball(1,0,-1,0);await_value("L",remaining);
        if (!remaining) break;
        await_value("W",1);at_game_loop();
        check(apple2_key(&machine,32),"keyboard SPACE accepted for next ball");
        await_value("W",0);at_game_loop();
    }
    await_text("GAME OVER");await_text("TERRIBLE!");
    check(contains("TERRIBLE!"),"original score rating shown after final missed ball");
    run(100000);check(value("L")==0 && contains("GAME OVER"),"game over waits for a fresh press rather than stale SPACE");
    check(apple2_key(&machine,13),"fresh keyboard RETURN accepted for restart");
    await_value("L",5);await_text("A / SPACE TO SERVE");await_value("W",1);
    puts("PASS original scoring, angles, acceleration, walls, all 160 bricks, five balls, win/restart and score rating");
}

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    FILE *input = fopen(argv[1], "rb");
    check(input && fread(rom, 1, sizeof(rom), input) == sizeof(rom), "private ROM size"); fclose(input);
    input = fopen(argv[2], "rb");
    check(input && fread(slot_rom, 1, sizeof(slot_rom), input) == sizeof(slot_rom), "private slot ROM size"); fclose(input);
    image = fopen(argv[3], "r+b");
    check(image && apple2_init(&machine, rom, sizeof(rom)) &&
          disk2_init(&disk, slot_rom, read_track, write_track, NULL), "machine/disk initialized");
    apple2_attach_disk(&machine, &disk); ready(); language_test(); load_source(argv[4]);
    type("SAVE BREAKOUT\r"); ready(); check(disk2_flush(&disk) && !disk.error, "game SAVE persisted");
    fclose(image); image = fopen(argv[3], "r+b");
    check(image && disk2_init(&disk, slot_rom, read_track, write_track, NULL), "disk reopened");
    apple2_reset(&machine, true); ready(); type("LOAD BREAKOUT\r"); ready(); game_test();
    for (unsigned i=0;machine.cpu.PC!=0x9006 && i<1000000;++i) run(1);
    check(machine.cpu.PC==0x9006,"native input helper reached for break test");
    check(!machine.key_ready && apple2_key(&machine,3),"Ctrl-C injected at helper entry");
    ready();check(contains("BREAK IN"),"native input helper leaves Ctrl-C for Applesoft break");
    type("TEXT:HOME:SAVE BREAKOUT\r");ready();
    check(disk2_flush(&disk) && !disk.error,"SAVE remains usable after native game code ran");
    fclose(image);image=fopen(argv[3],"r+b");
    check(image && disk2_init(&disk,slot_rom,read_track,write_track,NULL),"post-game disk reopened");
    apple2_reset(&machine,true);ready();type("LOAD BREAKOUT\r");ready();type("RUN\r");
    await_text("A / SPACE TO SERVE");await_value("BR",160);await_value("L",5);
    fclose(image);
    puts("PASS BREAKOUT SAVE before/after play, image reopen, reboot, LOAD and RUN with actual ROM and Disk II");
    return 0;
}
