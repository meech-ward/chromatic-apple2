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
            if ((*pixel&15)==15) *pixel&=0xf0;
            if ((*pixel>>4)==15) *pixel&=0x0f;
        }
    }
    const unsigned row=y/2;
    const unsigned address=base+((row&7)<<7)+(row>>3)*40+x;
    machine.ram[address]|=(uint8_t)(15<<((y&1)*4));
    check(ball_pixels()==1,"deterministic missed-paddle ball seeded");
}
static void position_paddle(unsigned expected)
{
    at_game_loop();
    double current=value("P");
    if (expected!=1 && expected!=33 && (((unsigned)current^expected)&1)) {
        machine.buttons=64;await_value("P",1);current=1;
    }
    machine.buttons=current>expected?64:32;
    await_value("P",expected);
    machine.buttons=0;
    at_game_loop();
}
static void steering_step(uint8_t buttons, uint8_t key, unsigned expected, const char *message)
{
    at_game_loop();
    machine.buttons=buttons;
    if (key) check(apple2_key(&machine,key),"steering fixture key accepted");
    next_game_loop();
    check(value("P")==expected,message);
}
static void pace(const char *label, uint8_t buttons, bool active)
{
    position_paddle(33);
    set_value("X",10);set_value("Y",32);set_value("DX",1);set_value("DY",-1);
    seed_ball_pixel(10,32);
    machine.buttons=buttons;
    const uint64_t start=machine.cpu.total_cycle;
    for (unsigned step=0;step<20;++step) {
        if (active) machine.buttons=step&1?32:64;
        next_game_loop();
        check(value("X")==11+step && value("Y")==31-step,
              "pace fixture advances one unobstructed ball step per loop");
        if (active) check(value("P")==((step&1)?33:31),
                          "held direction reverses paddle on the next loop");
    }
    const uint64_t elapsed=machine.cpu.total_cycle-start;
    check(value("X")==30 && value("Y")==12,"pace fixture follows identical unobstructed ball path");
    check(ball_pixels()==1,"pace fixture ends with one visible ball");
    check(value("P")==33,"pace fixture paddle stays within bounds and reverses immediately");
    printf("BREAKOUT %s: %.1f cycles/step, %.2f steps/s at 1.023 MHz\n",label,
           elapsed/20.0,20.0*1023000/elapsed);
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
    type("HOME:RUN\r"); run(100000); await_text("A / SPACE TO LAUNCH");
    check(!machine.text_mode && machine.mixed_mode && !machine.hires_mode, "game lores mixed mode");
    await_value("BR", 36); await_value("L", 3); await_value("W", 1);
    run(100000); at_game_loop();
    const double waiting_x=value("X"),waiting_y=value("Y");
    visible_game_loop(); visible_game_loop();
    check(value("X")==waiting_x && value("Y")==waiting_y,"stationary waiting ball retained");
    machine.buttons = 64;
    await_value("P", 1);
    machine.buttons = 32;
    await_value("P", 33);
    /* Lores y36 and y37 share bytes: moving the paddle must preserve every
     * possible color nibble on the adjacent row, including a white ball. */
    uint8_t row36[40];
    for (unsigned col=0;col<40;++col) {
        row36[col]=machine.ram[0x550+col]&15;
        machine.ram[0x550+col]=(machine.ram[0x550+col]&0xf0)|(col&15);
    }
    steering_step(64,21,31,"fresh held LEFT overrides stale RIGHT cursor key");
    steering_step(32,8,33,"quick held RIGHT reversal overrides stale LEFT cursor key");
    steering_step(96,21,33,"opposed LEFT+RIGHT stays neutral despite queued cursor key");
    steering_step(224,8,33,"opposed directions with UP stay neutral");
    steering_step(192,21,31,"UP+LEFT overrides opposite cursor key");
    steering_step(160,8,33,"UP+RIGHT overrides opposite cursor key");
    steering_step(0,8,31,"cursor steering works after physical directions release");
    for (unsigned col=0;col<40;++col) {
        check((machine.ram[0x550+col]&15)==(col&15),"paddle redraw preserves adjacent y36 pixels");
        machine.ram[0x550+col]=(machine.ram[0x550+col]&0xf0)|row36[col];
    }
    machine.buttons=0;
    puts("PASS BREAKOUT quick held reversals, opposed directions and keyboard fallback");
    machine.buttons = 8; await_value("W", 0); machine.buttons = 0;
    at_game_loop();
    const double previous_x=value("X"),previous_y=value("Y");
    visible_game_loop();
    check(value("X")!=previous_x || value("Y")!=previous_y,"monitored moving ball advanced");
    puts("PASS BREAKOUT ball visible at every instruction through stationary waiting and moving steps");
    /* A paused ball can be under a moving paddle at y37. Neither its native
     * erase nor redraw may hide the ball, and the paddle must heal afterward. */
    at_game_loop();set_value("H",1);set_value("X",20);set_value("Y",37);
    seed_ball_pixel(20,37);machine.ram[0x550+20]|=8;
    position_paddle(33);machine.buttons=64;
    for (unsigned step=0;step<8;++step) visible_game_loop();
    check(value("P")==17 && value("X")==20 && value("Y")==37,
          "paused ball remains stationary while paddle passes underneath");
    machine.buttons=0;visible_game_loop();
    set_value("X",10);set_value("Y",32);set_value("DX",1);set_value("DY",-1);
    seed_ball_pixel(10,32);visible_game_loop();
    check(machine.ram[0x550+20]==0xd8,"stationary paddle heals the vacated ball pixel and preserves y36");
    set_value("H",0);
    puts("PASS BREAKOUT visible paused ball under native paddle and stationary redraw repair");
    pace("clear field",0,false);
    pace("held RIGHT at clamp",32,false);
    pace("active steering/redraw",0,true);
    at_game_loop(); machine.buttons = 4; await_value("H", 1);
    run(100000);check(value("H")==1,"held B toggles pause only once");
    machine.buttons = 0;
    run(10000);
    const double paused_x = value("X"), paused_y = value("Y");
    run(100000);
    check(value("H") == 1 && value("X") == paused_x && value("Y") == paused_y,
          "B pause holds ball while input continues");
    machine.buttons = 4; await_value("H", 0); machine.buttons = 0;
    /* Set ordinary Applesoft numeric variables at its main loop, without game test hooks. */
    at_game_loop(); set_value("X", 3); set_value("Y", 6); set_value("DX", 1); set_value("DY", -1);
    await_value("S", 10); await_value("BR", 35);
    position_paddle(19); set_value("X", 20); set_value("Y", 36);
    set_value("DX", 1); set_value("DY", 1); await_value("DY", -1);
    check(value("L") == 3, "paddle bounce preserves life");
    at_game_loop(); set_value("X", 38); set_value("Y", 20); set_value("DX", 1); set_value("DY", -1);
    await_value("DX", -1);
    at_game_loop(); set_value("X", 20); set_value("Y", 1); set_value("DX", 1); set_value("DY", -1);
    await_value("DY", 1);
    position_paddle(19); set_value("X", 1); set_value("Y", 38);
    set_value("DX", 1); set_value("DY", 1);
    seed_ball_pixel(1,38); visible_game_loop();
    await_value("L", 2); await_value("W", 1); visible_game_loop();
    check(value("X")==value("P")+2 && value("Y")==35,"waiting ball repositioned after lost life");
    puts("PASS BREAKOUT ball stays visible through lost life and waiting-ball reposition");
    machine.buttons = 8; await_value("W", 0); machine.buttons = 0;
    at_game_loop(); set_value("X", 3); set_value("Y", 8); set_value("DX", 1); set_value("DY", -1);
    set_value("BR", 1); await_text("YOU WIN!");
    machine.buttons = 0; run(10000); machine.buttons = 8; await_value("L", 3);
    machine.buttons = 0; await_value("W", 1);
    machine.buttons = 8; await_value("W", 0); machine.buttons = 0;
    position_paddle(19); set_value("L", 1); set_value("X", 1); set_value("Y", 38);
    set_value("DX", 1); set_value("DY", 1); await_text("GAME OVER");
    puts("PASS BREAKOUT real Applesoft: lores, held/cursor steering, A launch, B pause, walls/paddle, brick/score, lost life, win/restart, game over");
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
    fclose(image);
    puts("PASS BREAKOUT SAVE, image close/reopen, reboot, LOAD and RUN with actual ROM and Disk II");
    return 0;
}
