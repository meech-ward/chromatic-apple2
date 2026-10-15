/* SPDX-License-Identifier: MIT */
/* Execute the generated 6502 helper, including vertical lores nibble writes. */
#include "apple2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CODE=0x9000, EVENT=0x3c0, PADDLE, KEY, PREVIOUS_B, OLD_PADDLE,
       PAUSE_EDGE, MOVED };
static apple2_t machine;
static uint8_t rom[256];
static unsigned address(unsigned x,unsigned y)
{
    const unsigned row=y/2;
    return 0x400+((row&7)<<7)+(row>>3)*40+x;
}
static unsigned pixel(unsigned x,unsigned y)
{
    return (machine.ram[address(x,y)]>>((y&1)*4))&15;
}
static void color(unsigned x,unsigned y,unsigned value)
{
    uint8_t *p=machine.ram+address(x,y);
    const unsigned shift=(y&1)*4;
    *p=(*p&~(15<<shift))|(value<<shift);
}
static void load_helper(const char *path)
{
    FILE *file=fopen(path,"r");assert(file);
    char line[256];unsigned used=0;
    while (fgets(line,sizeof(line),file)) {
        unsigned number;int offset=0;
        if (sscanf(line,"%u DATA %n",&number,&offset)!=1 || !offset || number<2000 || number>=2300) continue;
        const char *p=line+offset;
        while (*p && *p!='\n') {
            char *end;unsigned long byte=strtoul(p,&end,10);
            assert(end!=p && byte<=255 && CODE+used<0x9600);
            machine.ram[CODE+used++]=(uint8_t)byte;p=end;
            if (*p!=',') break;
            ++p;
        }
    }
    assert(!fclose(file));assert(used && machine.ram[CODE]==0x4c);
}
static void seed_paddle(unsigned position)
{
    memset(machine.ram+EVENT,0,9);machine.ram[PADDLE]=position;
    for (unsigned y=0;y<40;++y)
        for (unsigned x=0;x<40;++x)
            color(x,y,x ? (x+y)%15 : y>=position && y<position+6 ? 6:1);
}
static void invoke(unsigned held,int key)
{
    apple2_reset(&machine,false);machine.buttons=held;
    if (key>=0) assert(apple2_key(&machine,key));
    apple2_run(&machine,1200);
    assert(machine.cpu.PC>=0xff03 && machine.cpu.PC<=0xff05);
    assert(machine.key_ready==(key==3));
}
static unsigned expected_paddle(unsigned position,unsigned held,int key)
{
    const unsigned direction=held&0x90;
    if (direction==0x90) return position;
    if (direction==0x10 || (!direction && (key==11 || key==87)))
        return position>1 ? position-2:0;
    if (direction==0x80 || (!direction && (key==10 || key==83)))
        return position<33 ? position+2:34;
    return position;
}
static void check_pixels(unsigned position)
{
    for (unsigned y=0;y<40;++y)
        for (unsigned x=0;x<40;++x)
            assert(pixel(x,y)==(x ? (x+y)%15 : y>=position && y<position+6 ? 6:1));
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    const uint8_t start[]={0x20,0,3,0x4c,3,0xff};
    memcpy(rom,start,sizeof(start));rom[0xfc]=0;rom[0xfd]=0xff;
    assert(apple2_init(&machine,rom,sizeof(rom)));load_helper(argv[1]);
    const uint8_t stubs[]={0x4c,0,0x90,0x4c,3,0x90};
    memcpy(machine.ram+0x300,stubs,sizeof(stubs));
    const unsigned positions[]={0,1,17,33,34};
    const unsigned held[]={0,16,128,144,32,64,255,80};
    const int keys[]={-1,11,10,87,83,32,13,3};
    for (unsigned p=0;p<sizeof(positions)/sizeof(*positions);++p)
        for (unsigned b=0;b<sizeof(held)/sizeof(*held);++b)
            for (unsigned k=0;k<sizeof(keys)/sizeof(*keys);++k) {
                seed_paddle(positions[p]);invoke(held[b],keys[k]);
                const unsigned expected=expected_paddle(positions[p],held[b],keys[k]);
                assert(machine.ram[PADDLE]==expected);
                assert(machine.ram[MOVED]==(expected!=positions[p]));
                assert(machine.ram[KEY]==(keys[k]<0 || keys[k]==3 ? 0:keys[k]));check_pixels(expected);
            }
    seed_paddle(17);
    invoke(4,-1);assert(machine.ram[PAUSE_EDGE]==128);
    invoke(4,-1);assert(!machine.ram[PAUSE_EDGE] && !machine.ram[EVENT]);
    invoke(0,-1);assert(!machine.ram[PAUSE_EDGE] && !machine.ram[EVENT]);
    invoke(4,-1);assert(machine.ram[PAUSE_EDGE]==128);
    for (unsigned y=0;y<40;++y) {
        seed_paddle(17);color(0,y,15);invoke(128,-1);assert(pixel(0,y)==15);
        invoke(16,-1);assert(pixel(0,y)==15);
    }
    seed_paddle(17);color(0,20,15);invoke(128,-1);color(0,20,1);
    invoke(0,-1);assert(pixel(0,20)==6);
    /* The helper is forbidden from using BASIC's zero-page workspace. */
    seed_paddle(17);uint8_t zero_page[256],vectors[48];
    memcpy(zero_page,machine.ram,256);memset(machine.ram+0x3d0,0xa5,sizeof(vectors));
    memcpy(vectors,machine.ram+0x3d0,sizeof(vectors));
    invoke(16,-1);assert(!memcmp(zero_page,machine.ram,256));
    assert(!memcmp(vectors,machine.ram+0x3d0,sizeof(vectors)));
    puts("PASS vertical BREAKOUT input, clamping, held priority, pause edges, all lores rows and visible balls");
    return 0;
}
