/* SPDX-License-Identifier: MIT. Exercise actual parser internals with ASan/UBSan.
 * This translation unit includes the engine; no test entry point ships in firmware. */
#include "../ssh_transport.cpp"
#include <cassert>
#include <cstdlib>
#include <cstdio>
extern "C" bool ssh_platform_listen(void) { return true; }
extern "C" void ssh_platform_stop(void) {}
extern "C" void ssh_platform_poll(void) {}
static uint32_t test_clock=1;
static bool advance_clock=false;
extern "C" uint32_t ssh_platform_milliseconds(void) { return advance_clock?test_clock++:test_clock; }
extern "C" void ssh_platform_random(uint8_t *p,size_t n) { arc4random_buf(p,n); }
static uint32_t rng=0x12345678;
static uint32_t next_random() { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void reset(Phase next) {
    phase=next; closing=false; encrypted_in=encrypted_out=false; tx_size=rx_size=0;
    input_size=input_head=window_credit=0; channel_open=shell=true;
    pending_interrupt=input_eof=skip_guess=false;
    keepalive_pending=false; accepted_at=last_receive=test_clock;
    client_channel=42; client_window=4096; client_max=1024; receive_window=INPUT_CAP;
    auth_failures=auth_packets=0;
}
static void consume(Writer &w) { on_packet(w.p,w.used); }
int main() {
    uint8_t seed[32]={1}; assert(ssh_transport_init(seed,"test-pass-123"));
    uint8_t sample[2048];
    const uint8_t types[]={5,20,30,50,80,90,93,94,96,97,98};
    for(unsigned i=0;i<60000;++i) {
        size_t n=next_random()%sizeof(sample);
        for(size_t j=0;j<n;++j) sample[j]=next_random();
        if(n)sample[0]=types[next_random()%(sizeof(types)/sizeof(types[0]))];
        reset(i%3==0?Phase::AUTH:(i%3==1?Phase::KEXINIT:Phase::SESSION));
        on_packet(sample,n);
    }
    /* Framing: zero/short/overflow lengths, random padding, and split headers. */
    for(unsigned i=0;i<30000;++i) {
        reset(Phase::KEXINIT); rx_size=next_random()%RX_CAP;
        for(size_t j=0;j<rx_size;++j)rx[j]=next_random();
        if(rx_size>=4 && i%2)store32(rx,(next_random()%1024)*8-4);
        process_receive();
    }
    /* Structured, malformed auth lengths reach every checked string read. */
    for(unsigned offset=0;offset<64;++offset) {
        for(uint32_t bad:{0u,1u,0xffffu,0xffffffffu}) {
            reset(Phase::AUTH); Writer w{sample,sizeof(sample)};
            w.byte(50);w.string("apple");w.string("ssh-connection");w.string("password");w.byte(0);w.string("test-pass-123");
            if(offset+4<=w.used)store32(sample+offset,bad);
            consume(w);
        }
    }
    /* Long typing traverses the ring repeatedly and only returned bytes earn credit. */
    reset(Phase::SESSION); uint8_t expected[31],out[7];
    for(size_t i=0;i<sizeof(expected);++i)expected[i]=i+32;
    for(unsigned round=0;round<1000;++round) {
        Writer w{sample,sizeof(sample)};w.byte(94);w.u32(0);w.string(expected,sizeof(expected));consume(w);
        assert(!closing); size_t consumed=0;
        while(consumed<sizeof(expected)) {
            size_t n=ssh_transport_read(out,sizeof(out));assert(n);
            assert(!memcmp(out,expected+consumed,n));consumed+=n;
        }
        assert(window_credit==sizeof(expected));assert(receive_window==INPUT_CAP-sizeof(expected));
        receive_window+=window_credit;window_credit=0;
    }
    /* Sender honors the receiver's window, including overflow-safe adjustments. */
    reset(Phase::SESSION);client_window=3;
    assert(ssh_transport_write(expected,31)==3);assert(ssh_transport_write(expected,31)==0);
    Writer adjust{sample,sizeof(sample)};adjust.byte(93);adjust.u32(0);adjust.u32(2);consume(adjust);
    assert(ssh_transport_write(expected,31)==2);
    reset(Phase::SESSION);client_window=100;
    adjust=Writer{sample,sizeof(sample)};adjust.byte(93);adjust.u32(0);adjust.u32(UINT32_MAX);consume(adjust);assert(closing);
    reset(Phase::SESSION);receive_window=1;
    Writer over{sample,sizeof(sample)};over.byte(94);over.u32(0);over.string(expected,2);consume(over);assert(closing);
    /* OOB SIGINT doesn't falsely increase the advertised SSH byte window. */
    reset(Phase::SESSION);Writer sig{sample,sizeof(sample)};
    sig.byte(98);sig.u32(0);sig.string("signal");sig.byte(0);sig.string("INT");consume(sig);
    assert(ssh_transport_read(out,1)==1 && out[0]==3 && window_credit==0);
    /* Ctrl-C discards preceding paste, preserves following keys, and returns all credit. */
    reset(Phase::SESSION);Writer prefix{sample,sizeof(sample)};
    prefix.byte(94);prefix.u32(0);prefix.string("old");consume(prefix);
    Writer cancel{sample,sizeof(sample)};cancel.byte(94);cancel.u32(0);cancel.string("discard\x03" "after");consume(cancel);
    assert(ssh_transport_take_interrupt() && !ssh_transport_take_interrupt());
    assert(ssh_transport_read(out,sizeof(out))==5 && !memcmp(out,"after",5));
    assert(window_credit==16 && receive_window==INPUT_CAP-16);
    /* EOF must wait for application queues, not disconnect as soon as transport empties. */
    reset(Phase::SESSION);Writer eof{sample,sizeof(sample)};eof.byte(96);eof.u32(0);consume(eof);
    assert(ssh_transport_input_ended() && ssh_transport_connected());
    Writer after_eof{sample,sizeof(sample)};after_eof.byte(94);after_eof.u32(0);after_eof.string("x");consume(after_eof);assert(closing);
    /* A silent broken client releases the single slot; a valid reply keeps it. */
    reset(Phase::SESSION);test_clock+=KEEPALIVE_INTERVAL+1;ssh_transport_poll();assert(keepalive_pending && !closing);
    Writer alive{sample,sizeof(sample)};alive.byte(82);consume(alive);assert(!keepalive_pending);
    reset(Phase::SESSION);test_clock+=KEEPALIVE_INTERVAL+1;ssh_transport_poll();assert(keepalive_pending);
    test_clock+=KEEPALIVE_TIMEOUT+1;ssh_transport_poll();assert(closing);
    tx_size=0;assert(ssh_transport_wire_should_close());ssh_transport_wire_closed();assert(phase==Phase::CLOSED);
    /* A decrypt/parser crossing a clock tick must not underflow the idle age. */
    reset(Phase::SESSION);Writer tick=message(2);tick.string("");assert(send_packet(tick));
    memcpy(rx,tx,tx_size);rx_size=tx_size;tx_size=0;
    advance_clock=true;ssh_transport_poll();advance_clock=false;
    assert(!closing && !keepalive_pending && !tx_size);
    /* No data, window or shell request bypasses authentication. */
    reset(Phase::AUTH);Writer bad{sample,sizeof(sample)};bad.byte(94);bad.u32(0);bad.string("x");consume(bad);assert(closing);
    puts("Parser fuzz: 90,000 cases plus auth bounds, ring wrap, windows, and SIGINT passed");
}
