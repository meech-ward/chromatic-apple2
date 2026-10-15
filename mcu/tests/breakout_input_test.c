/* SPDX-License-Identifier: MIT */
/* Exercise the embedded original 6502 helper with a synthetic monitor ROM. */
#include "apple2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CODE=0x300, EVENT=0x3c0, PADDLE, KEY, PREVIOUS_B, OLD_PADDLE,
       PAUSE_EDGE, MOVED, PADDLE_ROW=0x550 };
static apple2_t machine;
static uint8_t rom[256];

static void load_helper(const char *path)
{
    FILE *file=fopen(path,"r");
    assert(file);
    char line[256];
    unsigned used=0;
    while (fgets(line,sizeof(line),file)) {
        unsigned number;
        int offset=0;
        if (sscanf(line,"%u DATA %n",&number,&offset)!=1 ||
            !offset || number<2000 || number>=2100) continue;
        const char *p=line+offset;
        while (*p && *p!='\n') {
            char *end;
            unsigned long byte=strtoul(p,&end,10);
            assert(end!=p && byte<=255 && CODE+used<EVENT);
            machine.ram[CODE+used++]=(uint8_t)byte;
            p=end;
            if (*p!=',') break;
            ++p;
        }
    }
    assert(!fclose(file));
    assert(used && machine.ram[CODE+used-1]==0x60);
}

static void seed_paddle(unsigned position)
{
    memset(machine.ram+EVENT,0,7);
    machine.ram[PADDLE]=position;
    for (unsigned x=0;x<40;++x)
        machine.ram[PADDLE_ROW+x]=(x&15) |
            (x>=position && x<position+6 ? 0xd0:0);
}

static void invoke(unsigned held,int key)
{
    apple2_reset(&machine,false);
    machine.buttons=held;
    if (key>=0) assert(apple2_key(&machine,key));
    apple2_run(&machine,600);
    assert(machine.cpu.PC>=0xff03 && machine.cpu.PC<=0xff05);
    assert(!machine.key_ready);
}

static unsigned expected_paddle(unsigned position,unsigned held,int key)
{
    const unsigned direction=held&0x60;
    if (direction==0x60) return position;
    if (direction==0x40 || (!direction && (key==8 || key==65)))
        return position>2 ? position-2:1;
    if (direction==0x20 || (!direction && (key==21 || key==68)))
        return position<32 ? position+2:33;
    return position;
}

static void check_pixels(unsigned position)
{
    for (unsigned x=0;x<40;++x) {
        assert((machine.ram[PADDLE_ROW+x]&15)==(x&15));
        assert((machine.ram[PADDLE_ROW+x]>>4)==
               (x>=position && x<position+6 ? 13:0));
    }
}

int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    /* Reset: JSR $0300, then loop at $FF03 after the helper returns. */
    const uint8_t start[]={0x20,0,3,0x4c,3,0xff};
    memcpy(rom,start,sizeof(start));
    rom[0xfc]=0;rom[0xfd]=0xff;
    assert(apple2_init(&machine,rom,sizeof(rom)));
    load_helper(argv[1]);
    const unsigned positions[]={1,2,18,32,33};
    const unsigned held[]={0,32,64,96,16,128,255,40};
    const int keys[]={-1,8,21,65,68,32,13};
    for (unsigned p=0;p<sizeof(positions)/sizeof(*positions);++p)
        for (unsigned b=0;b<sizeof(held)/sizeof(*held);++b)
            for (unsigned k=0;k<sizeof(keys)/sizeof(*keys);++k) {
                seed_paddle(positions[p]);
                invoke(held[b],keys[k]);
                const unsigned expected=expected_paddle(positions[p],held[b],keys[k]);
                assert(machine.ram[PADDLE]==expected);
                assert(machine.ram[MOVED]==(expected!=positions[p]));
                assert(machine.ram[KEY]==(keys[k]<0 ? 0:keys[k]));
                check_pixels(expected);
            }
    seed_paddle(18);
    invoke(4,-1); assert(machine.ram[PAUSE_EDGE]==128);
    invoke(4,-1); assert(!machine.ram[PAUSE_EDGE] && !machine.ram[EVENT]);
    invoke(0,-1); assert(!machine.ram[PAUSE_EDGE] && !machine.ram[EVENT]);
    invoke(4,-1); assert(machine.ram[PAUSE_EDGE]==128);
    /* Moving y37 preserves a white ball in the shared y36 lower nibble. */
    seed_paddle(18);machine.ram[PADDLE_ROW+20]=0xdf;
    invoke(32,-1); assert((machine.ram[PADDLE_ROW+20]&15)==15);
    /* A y37 ball stays visible when the paddle moves under or away from it. */
    seed_paddle(18);machine.ram[PADDLE_ROW+24]=0xf8;
    invoke(32,-1); assert(machine.ram[PADDLE_ROW+24]==0xf8);
    invoke(32,-1); assert(machine.ram[PADDLE_ROW+24]==0xf8);
    /* Simulate the ball's normal erase, then restore the stationary paddle. */
    machine.ram[PADDLE_ROW+24]=8;
    invoke(0,-1); assert(machine.ram[PADDLE_ROW+24]==0xd8);
    puts("PASS native BREAKOUT input, clamping, held priority, pause edges and adjacent lores nibbles");
    return 0;
}
