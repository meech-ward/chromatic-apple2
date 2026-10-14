/* SPDX-License-Identifier: MIT. Test-only identity/password and loopback client. */
#include "ssh_transport_internal.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
extern "C" void esp_fill_random(void *p,size_t n) { arc4random_buf(p,n); }
extern "C" int64_t esp_timer_get_time(void) {
    using namespace std::chrono;
    static auto epoch=steady_clock::now();
    return duration_cast<microseconds>(steady_clock::now()-epoch).count();
}
int main() {
    uint8_t seed[32];for(unsigned i=0;i<32;++i)seed[i]=i;
    if(!ssh_transport_init(seed,"test-pass-123") || !ssh_transport_set_enabled(true)) {
        perror("Test SSH host initialization");return 1;
    }
    printf("READY %s\n",ssh_transport_fingerprint());fflush(stdout);
    uint32_t generation=0;uint8_t pending[1024];size_t length=0,sent=0;
    for(;;){
        ssh_transport_poll();
        if(ssh_transport_connected()){
            if(generation!=ssh_transport_session_id()){
                generation=ssh_transport_session_id();
                const char *hello="\r\nAPPLE II ESP32 SSH TEST\r\n]";
                length=strlen(hello);memcpy(pending,hello,length);sent=0;
            }
            if(length==sent){length=ssh_transport_read(pending,sizeof(pending));sent=0;}
            sent+=ssh_transport_write(pending+sent,length-sent);
            if(length==sent && ssh_transport_input_ended())ssh_transport_disconnect();
        }
        usleep(1000);
    }
}
