/* SPDX-License-Identifier: MIT. Actual terminal wrapper with test-only SDK IO. */
#include "../ssh_terminal.c"
#include <assert.h>
#include <stdlib.h>
static uint8_t source[2048], keys[128], stored_seed[32];
static size_t source_size, source_at, key_count, captured_size;
static char text[1024], captured[8192];
static bool blocked, text_busy, interrupt_pending, stored;
static size_t stored_size=32, write_limit=7;
static unsigned random_calls, commits, creates;
static int commit_error;
static bool gate_test;
static unsigned ready_polls, readiness_delays, seed_accesses;
void esp_fill_random(void *p,size_t n) { random_calls++;memset(p,0x42,n); }
int64_t esp_timer_get_time(void) { return 1000; }
bool network_ready(void) { return !gate_test || ++ready_polls >= 4; }
esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *h) { assert(!strcmp(s,"a2_ssh") && mode==NVS_READWRITE);*h=1;return 0; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *p,size_t *n) {
    (void)h;if(gate_test){assert(ready_polls==4);seed_accesses++;}
    assert(!strcmp(key,"host_seed"));if(!stored)return ESP_ERR_NVS_NOT_FOUND;
    assert(*n>=stored_size);memcpy(p,stored_seed,stored_size);*n=stored_size;return 0;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *p,size_t n) {
    (void)h;assert(!strcmp(key,"host_seed") && n==32);memcpy(stored_seed,p,n);stored=true;stored_size=n;return 0;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;commits++;return commit_error; }
void nvs_close(nvs_handle_t h) { (void)h; }
void vTaskDelay(unsigned ticks) {
    if(gate_test){assert(ticks==250 && !seed_accesses);readiness_delays++;}
}
void vTaskDelete(TaskHandle_t h) { (void)h; }
int xTaskCreatePinnedToCore(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *h,int core) {
    assert(fn && !strcmp(name,"apple2_ssh") && stack==12288 && !arg && priority==3 && core==0);
    creates++;*h=(void *)1;return pdPASS;
}
void mbedtls_platform_zeroize(void *p,size_t n) { memset(p,0,n); }
bool ssh_transport_init(const uint8_t seed[32],const char *password) { (void)seed;(void)password;return !gate_test; }
bool ssh_transport_set_enabled(bool enabled) { return enabled; }
void ssh_transport_poll(void) {}
bool ssh_transport_connected(void) { return true; }
uint32_t ssh_transport_session_id(void) { return 1; }
bool ssh_transport_take_interrupt(void) { bool p=interrupt_pending;interrupt_pending=false;return p; }
bool ssh_transport_input_ended(void) { return false; }
size_t ssh_transport_read(uint8_t *p,size_t n) {
    if(n>source_size-source_at)n=source_size-source_at;
    memcpy(p,source+source_at,n);source_at+=n;return n;
}
size_t ssh_transport_write(const uint8_t *p,size_t n) {
    if(n>write_limit)n=write_limit;assert(captured_size+n<sizeof(captured));
    memcpy(captured+captured_size,p,n);captured_size+=n;captured[captured_size]=0;return n;
}
void ssh_transport_terminal_size(unsigned *c,unsigned *r) { *c=80;*r=26; }
void ssh_transport_disconnect(void) {}
bool control_terminal_key(uint8_t key) {
    if(blocked)return false;assert(key_count<sizeof(keys));keys[key_count++]=key;return true;
}
bool control_terminal_text(char *p,size_t n) {
    if(text_busy)return false;assert(n>strlen(text));strcpy(p,text);return true;
}
static void feed(const char *p) {
    assert(strlen(p)<=sizeof(source));memcpy(source,p,strlen(p));source_size=strlen(p);source_at=0;
}
static void reset_input(void) {
    key_count=source_size=source_at=0;blocked=false;interrupt_pending=false;
    key_pending=replay_pending=skip_lf=false;escape_size=0;
}
int main(void) {
    uint8_t seed[32],again[32];
    assert(!host_seed(seed) && random_calls==1 && commits==1);
    assert(!host_seed(again) && !memcmp(seed,again,32) && random_calls==1 && commits==1);
    stored_size=31;assert(host_seed(again)==ESP_ERR_INVALID_SIZE && random_calls==1);
    stored=false;commit_error=-1;assert(host_seed(again)==-1);
    ssh_terminal_start();ssh_terminal_start();assert(creates==1);

    reset_input();feed("\033[A\033[B\033[C\033[D\r\n\177\035");
    for(unsigned i=0;i<30;++i)input(i*10);
    const uint8_t expected[]={11,10,21,8,13,8,29};
    assert(key_count==sizeof(expected) && !memcmp(keys,expected,sizeof(expected)));
    reset_input();feed("AB");blocked=true;input(0);input(10);
    assert(key_pending && pending_key=='A' && !key_count && source_at==1);
    blocked=false;input(20);input(30);assert(key_count==2 && keys[0]=='A' && keys[1]=='B');
    reset_input();feed("\033");input(0);input(90);assert(!key_count);
    input(100);input(110);assert(key_count==1 && keys[0]==27);
    reset_input();feed("\033[200~PASTE\033[201~");
    for(unsigned i=0;i<20;++i)input(i*10);
    assert(key_count==5 && !memcmp(keys,"PASTE",5));
    reset_input();blocked=true;queue_key('A');escape_size=1;interrupt_pending=true;
    input(0);assert(key_pending && pending_key==3 && !escape_size);
    blocked=false;input(10);assert(key_count==1 && keys[0]==3);
    reset_input();feed("\033x");for(unsigned i=0;i<4;++i)input(i*10);
    assert(key_count==2 && keys[0]==27 && keys[1]=='x');

    for(unsigned row=0;row<24;++row){memset(text+row*41,'A',40);text[row*41+40]='\n';}
    text[24*41]=0;force_frame=true;text_busy=true;assert(!screen() && force_frame);
    text_busy=false;assert(screen() && output_length<sizeof(pending_output));
    while(output_length)send_output();
    assert(strstr(captured,"Ctrl] opens menu") && strstr(captured,"\033[24;1H"));
    assert(screen() && !output_length); // Unchanged snapshots send no bytes.
    text[5*41+4]='X';assert(screen() && output_length<80);
    while(output_length)send_output();
    assert(strstr(captured,"\033[6;1HAAAAX"));
    gate_test=true;stored=false;commit_error=0;ssh_task(NULL);
    assert(ready_polls==4 && readiness_delays==3 && seed_accesses==1);
    puts("PASS actual SSH terminal: station/hotspot network readiness, durable/test-failed seed,12KiB task, arrows/Return/Backspace/menu/ESC/paste/interrupt, key backpressure, changed-row snapshots and partial output");
    return 0;
}
