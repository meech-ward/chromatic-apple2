// SPDX-License-Identifier: MIT
#include "a2_dos_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1); } } while(0)
static uint8_t disk[35*4096], scratch[4096];
static a2_dos_catalog catalog;
typedef struct { unsigned calls; int fail_track; } reader_state;
static uint8_t *sector(unsigned t,unsigned s) { return disk+t*4096+s*256; }
static bool read_track(void *context,unsigned t,uint8_t out[4096]) {
    reader_state *state=context;CHECK(t<35);state->calls++;
    if((int)t==state->fail_track)return false;
    memcpy(out,disk+t*4096,4096);return true;
}
static void fresh(reader_state *state) {
    memset(disk,0,sizeof(disk));state->calls=0;state->fail_track=-1;
    uint8_t *v=sector(17,0);v[1]=17;v[2]=15;v[3]=3;v[0x27]=122;
    v[0x34]=35;v[0x35]=16;v[0x37]=1;
}
static uint8_t *entry(unsigned t,unsigned s,unsigned i,const char *name,uint8_t type) {
    CHECK(i<7 && strlen(name)<=30);
    uint8_t *e=sector(t,s)+11+i*35;e[0]=18;e[1]=1;e[2]=type;
    memset(e+3,0xa0,30);
    for(unsigned n=0;name[n];++n)e[3+n]=(uint8_t)name[n]|0x80;
    return e;
}
static int parse(reader_state *state) { return a2_dos_catalog_read(read_track,state,&catalog,scratch); }
int main(int argc,char **argv) {
    reader_state state;fresh(&state);
    CHECK(!parse(&state) && !catalog.count && !catalog.truncated);
    CHECK(state.calls==1); // VTOC and same-track catalog share the scratch read.
    entry(17,15,0,"HELLO WORLD",0x82);entry(17,15,1,"INTEGER-1",1);
    entry(17,15,2,"DELETED",2)[0]=0xff;
    entry(17,15,3,"UNUSED",2)[0]=0;
    entry(17,15,4,"BINARY",4);entry(17,15,5,"TEXT",0);
    entry(17,15,6,"BAD,SLICE",2);
    sector(17,15)[1]=17;sector(17,15)[2]=14;
    entry(17,14,0,"ABCDEFGHIJKLMNOPQRSTUVWXYZ1234",2);
    entry(17,14,1,"BAD:RUN",2);
    entry(17,14,2,"BAD\"QUOTE",2);
    entry(17,14,3,"BAD CTRL",2)[7]=0x8d;
    entry(17,14,4,"BAD DEL",2)[7]=0xff;
    entry(17,14,5,"",2);entry(17,14,6," LEADING",2);
    state.calls=0;CHECK(!parse(&state));
    CHECK(catalog.count==3 && !catalog.truncated && state.calls==1);
    CHECK(!strcmp(catalog.programs[0].name,"HELLO WORLD") && catalog.programs[0].type==2 && catalog.programs[0].locked);
    CHECK(!strcmp(catalog.programs[1].name,"INTEGER-1") && catalog.programs[1].type==1 && !catalog.programs[1].locked);
    CHECK(strlen(catalog.programs[2].name)==30 && catalog.programs[2].name[30]==0);

    // Cross-track chain forces a new callback; failed reads clear stale entries.
    fresh(&state);entry(17,15,0,"FIRST",2);
    sector(17,15)[1]=34;sector(17,15)[2]=15;entry(34,15,0,"LAST",1);
    CHECK(!parse(&state) && catalog.count==2 && state.calls==2);
    state.fail_track=34;CHECK(parse(&state)==A2_DOS_ERR_IO && !catalog.count && !catalog.truncated);
    state.fail_track=17;CHECK(parse(&state)==A2_DOS_ERR_IO && !catalog.count);

    // Both catalog head and chain pointers are range-checked before callbacks.
    const uint8_t invalid[][2]={{35,0},{17,16},{0,1},{17,0},{255,15}};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        fresh(&state);sector(17,0)[1]=invalid[i][0];sector(17,0)[2]=invalid[i][1];
        CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count && state.calls==1);
        fresh(&state);entry(17,15,0,"FIRST",2);
        sector(17,15)[1]=invalid[i][0];sector(17,15)[2]=invalid[i][1];
        CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count && !catalog.truncated);
    }
    fresh(&state);sector(17,15)[1]=17;sector(17,15)[2]=15;
    CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count);
    fresh(&state);sector(17,15)[1]=18;sector(17,15)[2]=1;
    sector(18,1)[1]=17;sector(18,1)[2]=15;
    CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count && state.calls==2);
    fresh(&state);entry(17,15,0,"BAD POINTER",2)[0]=35;
    CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count);
    fresh(&state);entry(17,15,0,"BAD POINTER",2)[1]=16;
    CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count);
    const unsigned geometry[]={3,0x27,0x34,0x35,0x36,0x37};
    for(unsigned i=0;i<sizeof(geometry)/sizeof(geometry[0]);++i){
        fresh(&state);sector(17,0)[geometry[i]]^=1;
        CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count);
    }

    fresh(&state);
    for(unsigned s=15;s>=6;--s){
        if(s>6){sector(17,s)[1]=17;sector(17,s)[2]=(uint8_t)(s-1);}
        for(unsigned i=0;i<7;++i){char name[20];snprintf(name,sizeof(name),"PROGRAM %u",(15-s)*7+i);entry(17,s,i,name,2);}
    }
    CHECK(!parse(&state) && catalog.count==64 && catalog.truncated && state.calls==1);
    CHECK(!strcmp(catalog.programs[63].name,"PROGRAM 63"));
    sector(17,6)[1]=17;sector(17,6)[2]=15;
    CHECK(parse(&state)==A2_DOS_ERR_CORRUPT && !catalog.count && !catalog.truncated);
    CHECK(a2_dos_catalog_read(NULL,&state,&catalog,scratch)==A2_DOS_ERR_INVAL && !catalog.count);
    CHECK(a2_dos_catalog_read(read_track,&state,&catalog,NULL)==A2_DOS_ERR_INVAL && !catalog.count);
    CHECK(a2_dos_catalog_read(read_track,&state,NULL,scratch)==A2_DOS_ERR_INVAL);

    if(argc==2){
        FILE *source=fopen(argv[1],"rb");CHECK(source);
        CHECK(fread(disk,1,sizeof(disk),source)==sizeof(disk));CHECK(fgetc(source)==EOF);fclose(source);
        state.calls=0;state.fail_track=-1;CHECK(!parse(&state));
        CHECK(catalog.count>0);
        printf("Local DOS disk: %u safe BASIC programs, %u track reads:",catalog.count,state.calls);
        for(unsigned i=0;i<catalog.count;++i)printf(" %s%s",catalog.programs[i].locked?"*":"",catalog.programs[i].name);
        putchar('\n');
    }
    puts("PASS: BASIC types/lock/highbits/names, deleted/unused filtering, geometry/pointers/cycles, truncation, callback errors");
    return 0;
}
