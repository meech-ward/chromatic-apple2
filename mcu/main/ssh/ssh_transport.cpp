/*
 * Apple II single-session SSH transport.
 * Derived from staticnet SSHTransportServer.cpp / CryptoEngine.cpp,
 * Copyright (c) 2021-2025 Andrew D. Zonenberg and contributors.
 * Distributed under BSD-3-Clause: see vendor/staticnet/LICENSE.
 *
 * The staticnet exchange and cipher design is retained; its network stack,
 * packet casts and circular FIFO are replaced with checked byte-stream IO.
 */
#include "ssh_transport_internal.h"
#include "vendor/staticnet/crypt/CryptoEngine.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/platform_util.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

namespace {
constexpr size_t RX_CAP = SSH_TRANSPORT_WIRE_CAPACITY, TX_CAP = 8192, INPUT_CAP = 2048;
constexpr size_t PAYLOAD_CAP = 1400, MAX_CHANNEL_PACKET = 1024;
constexpr size_t ENCRYPTED_CAP = 2048;
constexpr uint32_t AUTH_TIMEOUT = 60000, MAX_SESSION_MS = 12u*60u*60u*1000u;
constexpr uint32_t KEEPALIVE_INTERVAL = 30000, KEEPALIVE_TIMEOUT = 60000;
constexpr uint64_t MAX_SESSION_BYTES = 256u*1024u*1024u;
constexpr char BANNER[] = "SSH-2.0-AppleII_1.0";
constexpr char KEX[] = "curve25519-sha256";
constexpr char HOST[] = "ssh-ed25519";
constexpr char CIPHER[] = "aes128-gcm@openssh.com";

uint32_t load32(const uint8_t *p) {
    return uint32_t(p[0])<<24 | uint32_t(p[1])<<16 | uint32_t(p[2])<<8 | p[3];
}
void store32(uint8_t *p, uint32_t n) {
    p[0]=n>>24; p[1]=n>>16; p[2]=n>>8; p[3]=n;
}
struct Slice {
    const uint8_t *data=nullptr; size_t size=0;
    bool is(const char *s) const { return size==strlen(s) && !memcmp(data,s,size); }
};
struct Reader {
    const uint8_t *p; size_t left; bool good=true;
    Slice bytes(size_t n) {
        if(n>left) { good=false; return {}; }
        Slice result{p,n}; p+=n; left-=n; return result;
    }
    uint8_t byte() { auto x=bytes(1); return good ? *x.data : 0; }
    uint32_t u32() { auto x=bytes(4); return good ? load32(x.data) : 0; }
    Slice string() { uint32_t n=u32(); return bytes(n); }
    bool done() const { return good && !left; }
};
struct Writer {
    uint8_t *p; size_t capacity, used=0; bool good=true;
    void bytes(const void *data, size_t n) {
        if(n>capacity-used) { good=false; return; }
        memcpy(p+used,data,n); used+=n;
    }
    void byte(uint8_t n) { bytes(&n,1); }
    void u32(uint32_t n) { uint8_t b[4]; store32(b,n); bytes(b,4); }
    void string(const void *data,size_t n) { u32(n); bytes(data,n); }
    void string(const char *s) { string(s,strlen(s)); }
};
bool contains(Slice list, const char *name) {
    size_t at=0, want=strlen(name);
    while(at<list.size) {
        size_t end=at; while(end<list.size && list.data[end]!=',') ++end;
        if(end-at==want && !memcmp(list.data+at,name,want)) return true;
        at=end+1;
    }
    return false;
}
bool first_is(Slice list,const char *name) {
    size_t n=0; while(n<list.size && list.data[n]!=',') ++n;
    return Slice{list.data,n}.is(name);
}
void iv_next(uint8_t iv[12]) { for(int i=11;i>=4;--i) if(++iv[i]) break; }

class Crypto final : public CryptoEngine {
public:
    mbedtls_sha256_context sha{};
    mbedtls_gcm_context receive{}, send{};
    uint8_t plaintext[ENCRYPTED_CAP];
    bool good=true;
    Crypto() {
        mbedtls_sha256_init(&sha); mbedtls_gcm_init(&receive); mbedtls_gcm_init(&send);
    }
    void GenerateRandom(uint8_t *out,size_t n) override { ssh_platform_random(out,n); }
    void SHA256_Init() override { if(mbedtls_sha256_starts(&sha,0)) good=false; }
    void SHA256_Update(const uint8_t *p,uint16_t n) override {
        if(mbedtls_sha256_update(&sha,p,n)) good=false;
    }
    void SHA256_Final(uint8_t *out) override { if(mbedtls_sha256_finish(&sha,out)) good=false; }
    void Clear() override {
        mbedtls_gcm_free(&receive); mbedtls_gcm_free(&send);
        mbedtls_gcm_init(&receive); mbedtls_gcm_init(&send);
        mbedtls_platform_zeroize(plaintext,sizeof(plaintext));
        good=true; CryptoEngine::Clear();
    }
    bool install() {
        if(mbedtls_gcm_setkey(&receive,MBEDTLS_CIPHER_ID_AES,m_keyClientToServer,128) ||
           mbedtls_gcm_setkey(&send,MBEDTLS_CIPHER_ID_AES,m_keyServerToClient,128)) good=false;
        return good;
    }
    bool DecryptAndVerify(uint8_t *data,uint16_t len) override {
        if(len<GCM_TAG_SIZE || size_t(len-GCM_TAG_SIZE)>sizeof(plaintext)) return false;
        size_t n=len-GCM_TAG_SIZE;
        int err=mbedtls_gcm_auth_decrypt(&receive,n,m_ivClientToServer,GCM_IV_SIZE,
            data-4,4,data+n,GCM_TAG_SIZE,data,plaintext);
        if(!err) memcpy(data,plaintext,n);
        mbedtls_platform_zeroize(plaintext,n);
        iv_next(m_ivClientToServer); return !err;
    }
    void EncryptAndMAC(uint8_t *data,uint16_t n) override {
        if(mbedtls_gcm_crypt_and_tag(&send,MBEDTLS_GCM_ENCRYPT,n,m_ivServerToClient,GCM_IV_SIZE,
            data-4,4,data,data,GCM_TAG_SIZE,data+n)) good=false;
        iv_next(m_ivServerToClient);
    }
    void hash_string(const void *p,size_t n) {
        uint8_t length[4]; store32(length,n); SHA256_Update(length,4);
        SHA256_Update(static_cast<const uint8_t*>(p),n);
    }
    void hash_mpint(const uint8_t *p,size_t n) {
        while(n && !*p) { ++p; --n; }
        bool prefix=n && (*p&0x80); uint8_t length[4]; store32(length,n+prefix);
        SHA256_Update(length,4); if(prefix) { uint8_t zero=0; SHA256_Update(&zero,1); }
        SHA256_Update(p,n);
    }
};

enum class Phase { CLOSED, BANNER, KEXINIT, ECDH, NEWKEYS, SERVICE, AUTH, SESSION };
Crypto crypto;
alignas(4) uint8_t rx[RX_CAP], tx[TX_CAP], input[INPUT_CAP], payload[PAYLOAD_CAP];
size_t rx_size=0,tx_size=0,input_head=0,input_size=0;
uint8_t password_hash[32], session_id[32];
char fingerprint[64]="";
Phase phase=Phase::CLOSED;
bool initialized=false,enabled=false,listening=false,closing=false,shell=false;
bool encrypted_in=false,encrypted_out=false,skip_guess=false,channel_open=false;
bool pending_interrupt=false,input_eof=false;
bool keepalive_pending=false;
uint32_t client_channel=0,client_window=0,client_max=0,receive_window=0,window_credit=0;
uint32_t accepted_at=0,closed_at=0,in_sequence=0,out_sequence=0;
uint32_t last_receive=0,keepalive_sent=0;
uint64_t total_bytes=0;
uint32_t session_generation=0;
unsigned columns=80,rows=24,auth_failures=0,auth_packets=0;
const char *status="Off";

Writer message(uint8_t type) { Writer w{payload,sizeof(payload)}; w.byte(type); return w; }
bool send_packet(const Writer &w) {
    if(!w.good || !w.used) return false;
    size_t padding=4,block=encrypted_out ? 16:8;
    while((w.used+1+padding+(encrypted_out?0:4))%block) ++padding;
    size_t packet_length=w.used+1+padding,wire=4+packet_length+(encrypted_out?16:0);
    if(wire>sizeof(tx)-tx_size) return false;
    uint8_t *p=tx+tx_size; store32(p,packet_length); p[4]=padding;
    memcpy(p+5,w.p,w.used); crypto.GenerateRandom(p+5+w.used,padding);
    if(encrypted_out) crypto.EncryptAndMAC(p+4,packet_length);
    if(!crypto.good) return false;
    tx_size+=wire; total_bytes+=wire; ++out_sequence; return true;
}
void fail(const char *why,uint32_t reason=2) {
    if(closing || phase==Phase::CLOSED) return;
    auto w=message(1); w.u32(reason); w.string(why); w.string(""); send_packet(w);
    status=why; closing=true; shell=false; closed_at=ssh_platform_milliseconds();
    input_size=0; mbedtls_platform_zeroize(input,sizeof(input));
}
void auth_failed(bool count) {
    if(count && ++auth_failures>=3) { fail("Authentication failed",2); return; }
    auto w=message(51); w.string("password"); w.byte(0);
    if(!send_packet(w)) fail("SSH send buffer full");
}
bool parse_kex(Reader &r) {
    r.bytes(16); Slice lists[10]; for(auto &list:lists) list=r.string();
    bool follows=r.byte()!=0; uint32_t reserved=r.u32();
    if(!r.done() || reserved || !contains(lists[0],KEX) || !contains(lists[1],HOST) ||
       !contains(lists[2],CIPHER) || !contains(lists[3],CIPHER) ||
       !contains(lists[6],"none") || !contains(lists[7],"none")) return false;
    skip_guess=follows && (!first_is(lists[0],KEX) || !first_is(lists[1],HOST));
    return true;
}
void on_kex(const uint8_t *data,size_t length,Reader &r) {
    if(!parse_kex(r)) { fail("No supported SSH algorithms",3); return; }
    crypto.hash_string(data,length);
    auto w=message(20); uint8_t cookie[16]; crypto.GenerateRandom(cookie,sizeof(cookie));
    w.bytes(cookie,sizeof(cookie)); w.string(KEX); w.string(HOST);
    w.string(CIPHER); w.string(CIPHER); w.string("none"); w.string("none");
    w.string("none"); w.string("none"); w.string(""); w.string(""); w.byte(0); w.u32(0);
    crypto.hash_string(w.p,w.used);
    if(!send_packet(w)) { fail("SSH send buffer full"); return; }
    phase=Phase::ECDH;
}
void on_ecdh(Reader &r) {
    Slice client_key=r.string();
    if(!r.done() || client_key.size!=32) { fail("Invalid SSH key exchange",3); return; }
    uint8_t ephemeral[32],secret[32],signature[64],host_blob[51];
    Writer hb{host_blob,sizeof(host_blob)}; hb.string(HOST); hb.string(crypto.GetHostPublicKey(),32);
    crypto.GenerateX25519KeyPair(ephemeral);
    crypto.SharedSecret(secret,const_cast<uint8_t*>(client_key.data));
    uint8_t nonzero=0; for(auto b:secret) nonzero|=b;
    if(!nonzero) { fail("Invalid SSH shared secret",3); return; }
    crypto.hash_string(host_blob,sizeof(host_blob)); crypto.hash_string(client_key.data,32);
    crypto.hash_string(ephemeral,32); crypto.hash_mpint(secret,32);
    crypto.SHA256_Final(session_id); crypto.SignExchangeHash(signature,session_id);
    crypto.DeriveSessionKeys(secret,session_id,session_id);
    mbedtls_platform_zeroize(secret,sizeof(secret));
    if(!crypto.install()) { fail("SSH crypto initialization failed"); return; }
    auto w=message(31); w.string(host_blob,sizeof(host_blob)); w.string(ephemeral,32);
    w.u32(83); w.string(HOST); w.string(signature,64);
    if(!send_packet(w)) { fail("SSH send buffer full"); return; }
    w=message(21);
    if(!send_packet(w)) { fail("SSH send buffer full"); return; }
    encrypted_out=true; phase=Phase::NEWKEYS;
}
void on_auth(Reader &r) {
    if(++auth_packets>32) { fail("Too many authentication requests"); return; }
    Slice username=r.string(),service=r.string(),method=r.string();
    if(!r.good || username.size>64 || !service.is("ssh-connection")) { fail("Invalid authentication request"); return; }
    if(!method.is("password")) { auth_failed(false); return; }
    bool change=r.byte()!=0; Slice password=r.string();
    if(!r.done() || change || password.size>64) { auth_failed(true); return; }
    uint8_t digest[32];
    if(mbedtls_sha256(password.data,password.size,digest,0)) { fail("SSH crypto failure"); return; }
    uint8_t different=0; for(size_t i=0;i<32;++i) different|=digest[i]^password_hash[i];
    mbedtls_platform_zeroize(digest,sizeof(digest));
    if(different || !username.is("apple")) { auth_failed(true); return; }
    auto w=message(52); if(!send_packet(w)) { fail("SSH send buffer full"); return; }
    phase=Phase::SESSION; status="Authenticated";
}
void channel_status(bool success) {
    auto w=message(success?99:100); w.u32(client_channel);
    if(!send_packet(w)) fail("SSH send buffer full");
}
void on_channel_open(Reader &r) {
    Slice kind=r.string(); uint32_t sender=r.u32(),window=r.u32(),maximum=r.u32();
    if(!r.good) { fail("Malformed channel request"); return; }
    if(channel_open || !kind.is("session") || !r.done() || maximum<1) {
        auto w=message(92); w.u32(sender); w.u32(1); w.string("One Apple II session only"); w.string("");
        if(!send_packet(w)) fail("SSH send buffer full");
        return;
    }
    channel_open=true; client_channel=sender; client_window=window; client_max=maximum;
    receive_window=INPUT_CAP;
    auto w=message(91); w.u32(sender); w.u32(0); w.u32(receive_window); w.u32(MAX_CHANNEL_PACKET);
    if(!send_packet(w)) fail("SSH send buffer full");
}
bool pty_modes_valid(Slice modes) {
    Reader m{modes.data,modes.size};
    while(m.left && m.good) {
        uint8_t op=m.byte(); if(!op || op>=160) return true;
        m.u32();
    }
    return false;
}
void on_channel_request(Reader &r) {
    uint32_t recipient=r.u32(); Slice request=r.string(); bool want_reply=r.byte()!=0;
    if(!r.good || !channel_open || recipient!=0) { fail("Invalid SSH channel"); return; }
    bool ok=false;
    if(request.is("pty-req") && !shell) {
        Slice terminal=r.string(); uint32_t c=r.u32(),h=r.u32(); r.u32(); r.u32(); Slice modes=r.string();
        ok=r.done() && terminal.size<=256 && pty_modes_valid(modes);
        if(ok) { columns=std::min<uint32_t>(c?c:80,1000); rows=std::min<uint32_t>(h?h:24,1000); }
    } else if(request.is("window-change")) {
        uint32_t c=r.u32(),h=r.u32(); r.u32(); r.u32(); ok=r.done();
        if(ok) { columns=std::min<uint32_t>(c?c:80,1000); rows=std::min<uint32_t>(h?h:24,1000); }
    } else if(request.is("shell") && !shell) {
        ok=r.done(); if(ok) { shell=true; ++session_generation; status="Connected"; }
    } else if(request.is("env")) {
        r.string(); r.string(); ok=r.done(); /* No host environment imported. */
    } else if(request.is("signal")) {
        Slice signal=r.string(); ok=r.done();
        if(ok && shell && signal.is("INT")) {
            pending_interrupt=true; window_credit+=input_size; input_size=input_head=0;
        }
    }
    if(want_reply) channel_status(ok);
}
void close_channel() {
    auto w=message(98); w.u32(client_channel); w.string("exit-status"); w.byte(0); w.u32(0); send_packet(w);
    w=message(96); w.u32(client_channel); send_packet(w);
    w=message(97); w.u32(client_channel); send_packet(w);
    closing=true; shell=false; status="Disconnected"; closed_at=ssh_platform_milliseconds();
}
void on_session(uint8_t type,Reader &r) {
    if(type==90) { on_channel_open(r); return; }
    if(type==98) { on_channel_request(r); return; }
    if(type==80) {
        r.string(); bool want=r.byte()!=0; if(!r.good) { fail("Malformed global request"); return; }
        if(want) { auto w=message(82); if(!send_packet(w)) fail("SSH send buffer full"); } return;
    }
    if((type==81 || type==82) && keepalive_pending) {
        if(!r.done()) { fail("Malformed keepalive reply"); return; }
        keepalive_pending=false; return;
    }
    if(type==93 || type==94 || type==96 || type==97) {
        uint32_t recipient=r.u32();
        if(!r.good || !channel_open || recipient!=0) { fail("Invalid SSH channel"); return; }
        if(type==93) {
            uint32_t amount=r.u32();
            if(!r.done() || amount>UINT32_MAX-client_window) { fail("Invalid SSH window"); return; }
            client_window+=amount;
        } else if(type==94) {
            Slice data=r.string();
            if(!r.done() || !shell || input_eof || data.size>MAX_CHANNEL_PACKET || data.size>receive_window || data.size>INPUT_CAP-input_size) {
                fail("SSH input window exceeded"); return;
            }
            size_t start=0;
            for(size_t i=0;i<data.size;++i) if(data.data[i]==3) start=i+1;
            if(start) {
                pending_interrupt=true; window_credit+=input_size+start; input_size=input_head=0;
            }
            for(size_t i=start;i<data.size;++i) input[(input_head+input_size+i-start)%INPUT_CAP]=data.data[i];
            input_size+=data.size-start; receive_window-=data.size;
        } else {
            if(!r.done()) { fail("Malformed channel close"); return; }
            if(type==96) input_eof=true;
            else close_channel();
        }
        return;
    }
    auto w=message(3); w.u32(in_sequence); if(!send_packet(w)) fail("SSH send buffer full");
}
void on_packet(const uint8_t *data,size_t length) {
    Reader r{data,length}; uint8_t type=r.byte();
    if(!r.good) { fail("Empty SSH packet"); return; }
    if(type==1) { closing=true; shell=false; status="Disconnected"; closed_at=ssh_platform_milliseconds(); return; }
    if(type==2 || type==4) return; /* IGNORE / DEBUG never becomes terminal input. */
    if(type==20 && encrypted_in) { fail("SSH rekey requested; reconnect",11); return; }
    if(skip_guess) { skip_guess=false; return; }
    switch(phase) {
    case Phase::KEXINIT: if(type==20) { on_kex(data,length,r); return; } break;
    case Phase::ECDH: if(type==30) { on_ecdh(r); return; } break;
    case Phase::NEWKEYS:
        if(type==21 && r.done()) { encrypted_in=true; phase=Phase::SERVICE; return; } break;
    case Phase::SERVICE:
        if(type==5) {
            Slice service=r.string();
            if(r.done() && service.is("ssh-userauth")) {
                auto w=message(6); w.string("ssh-userauth");
                if(!send_packet(w)) fail("SSH send buffer full"); else phase=Phase::AUTH;
                return;
            }
        } break;
    case Phase::AUTH: if(type==50) { on_auth(r); return; } break;
    case Phase::SESSION:
        on_session(type,r); return;
    default: break;
    }
    fail("Unexpected SSH packet");
}
void process_receive() {
    if(phase==Phase::BANNER) {
        auto newline=static_cast<uint8_t*>(memchr(rx,'\n',rx_size));
        if(!newline) { if(rx_size>=255) fail("Invalid SSH identification"); return; }
        size_t wire=newline-rx+1,n=wire-1; if(n && rx[n-1]=='\r') --n;
        if(wire>255 || n<8 || memcmp(rx,"SSH-2.0-",8)) { fail("SSH-2.0 required",8); return; }
        for(size_t i=0;i<n;++i) if(rx[i]<32 || rx[i]>126) { fail("Invalid SSH identification"); return; }
        crypto.SHA256_Init(); crypto.hash_string(rx,n); crypto.hash_string(BANNER,strlen(BANNER));
        memmove(rx,rx+wire,rx_size-wire); rx_size-=wire; phase=Phase::KEXINIT;
    }
    /* Bound work per poll; the emulator and network tasks need regular service. */
    for(unsigned processed=0;processed<4 && !closing && rx_size>=4;++processed) {
        uint32_t n=load32(rx); size_t tag=encrypted_in?16:0;
        if(n<6 || n>RX_CAP-4-tag || (encrypted_in && n>ENCRYPTED_CAP) ||
           (encrypted_in ? n%16 : (n+4)%8)) { fail("Invalid SSH packet length"); return; }
        size_t wire=4+n+tag; if(rx_size<wire) return;
        if(encrypted_in && !crypto.DecryptAndVerify(rx+4,n+16)) { fail("SSH authentication tag mismatch",5); return; }
        uint8_t padding=rx[4];
        if(padding<4 || size_t(padding)+2>n) { fail("Invalid SSH padding"); return; }
        on_packet(rx+5,n-padding-1); ++in_sequence; total_bytes+=wire;
        last_receive=ssh_platform_milliseconds();
        if(closing) return;
        memmove(rx,rx+wire,rx_size-wire); rx_size-=wire;
        mbedtls_platform_zeroize(rx+rx_size,wire);
    }
}
} // namespace

extern "C" bool ssh_transport_init(const uint8_t seed[32],const char *password) {
    if(enabled) ssh_transport_set_enabled(false);
    initialized=false;
    if(!seed || !password || strlen(password)<8 || strlen(password)>64) { status="SSH password needs 8-64 characters"; return false; }
    uint8_t private_key[32],public_key[32]; memcpy(private_key,seed,32);
    crypto_sign_keypair(public_key,private_key); crypto.SetHostKey(public_key,private_key);
    mbedtls_platform_zeroize(private_key,sizeof(private_key));
    if(mbedtls_sha256(reinterpret_cast<const uint8_t*>(password),strlen(password),password_hash,0)) { status="SSH crypto failure"; return false; }
    char base64[52]={0}; crypto.GetHostKeyFingerprint(base64,sizeof(base64));
    base64[strcspn(base64,"\r\n")]=0;
    snprintf(fingerprint,sizeof(fingerprint),"SHA256:%s",base64);
    initialized=true; status="Off"; return true;
}
extern "C" bool ssh_transport_set_enabled(bool value) {
    if(!value) {
        enabled=false; listening=false; ssh_platform_stop(); ssh_transport_wire_closed(); status="Off"; return true;
    }
    if(!initialized) { status="SSH is not configured"; return false; }
    if(listening) return true;
    enabled=true; listening=ssh_platform_listen();
    if(!listening) enabled=false;
    status=listening?"Ready":"Cannot open SSH port"; return listening;
}
extern "C" bool ssh_transport_enabled(void) { return enabled; }
extern "C" bool ssh_transport_listening(void) { return listening; }
extern "C" bool ssh_transport_connected(void) { return shell && !closing; }
extern "C" uint32_t ssh_transport_session_id(void) { return session_generation; }
extern "C" bool ssh_transport_take_interrupt(void) {
    bool result=shell && !closing && pending_interrupt; pending_interrupt=false; return result;
}
extern "C" bool ssh_transport_input_ended(void) {
    return shell && !closing && input_eof && !input_size && !pending_interrupt;
}
extern "C" const char *ssh_transport_status(void) { return status; }
extern "C" const char *ssh_transport_fingerprint(void) { return fingerprint; }
extern "C" void ssh_transport_terminal_size(unsigned *c,unsigned *r) { if(c)*c=columns; if(r)*r=rows; }
extern "C" void ssh_transport_disconnect(void) {
    if(phase==Phase::CLOSED || closing) return;
    if(channel_open) close_channel(); else fail("Disconnected by Chromatic",11);
}
extern "C" size_t ssh_transport_read(uint8_t *out,size_t capacity) {
    if(!out || !shell || closing) return 0;
    size_t prefix=0;
    if(pending_interrupt && capacity) { out[0]=3; ++out; --capacity; prefix=1; pending_interrupt=false; }
    size_t n=std::min(capacity,input_size);
    for(size_t i=0;i<n;++i) out[i]=input[(input_head+i)%INPUT_CAP];
    input_head=(input_head+n)%INPUT_CAP; input_size-=n; window_credit+=n;
    return n+prefix;
}
extern "C" size_t ssh_transport_write(const uint8_t *data,size_t length) {
    if(!data || !shell || closing || tx_size>TX_CAP-256) return 0;
    size_t n=std::min({length,size_t(client_window),size_t(client_max),size_t(1280),TX_CAP-tx_size-256});
    if(!n) return 0;
    auto w=message(94); w.u32(client_channel); w.string(data,n);
    if(!send_packet(w)) return 0;
    client_window-=n; return n;
}
extern "C" void ssh_transport_poll(void) {
    ssh_platform_poll();
    if(phase==Phase::CLOSED) return;
    uint32_t now=ssh_platform_milliseconds();
    if(!closing) {
        if(phase!=Phase::SESSION && uint32_t(now-accepted_at)>AUTH_TIMEOUT) fail("SSH login timed out",11);
        else if(uint32_t(now-accepted_at)>MAX_SESSION_MS || total_bytes>MAX_SESSION_BYTES) fail("SSH session limit; reconnect",11);
        else process_receive();
    }
    if(window_credit && shell && !closing) {
        auto w=message(93); w.u32(client_channel); w.u32(window_credit);
        if(send_packet(w)) { receive_window+=window_credit; window_credit=0; }
    }
    if(phase==Phase::SESSION && !closing) {
        now=ssh_platform_milliseconds(); /* Packet processing may have advanced the clock. */
        if(keepalive_pending && uint32_t(now-keepalive_sent)>KEEPALIVE_TIMEOUT) {
            fail("SSH peer stopped responding",11);
        } else if(!keepalive_pending && uint32_t(now-last_receive)>KEEPALIVE_INTERVAL) {
            auto w=message(80); w.string("keepalive@openssh.com"); w.byte(1);
            if(send_packet(w)) { keepalive_pending=true; keepalive_sent=now; }
        }
    }
    ssh_platform_poll();
}
extern "C" bool ssh_transport_wire_accept(void) {
    if(!enabled || phase!=Phase::CLOSED) return false;
    crypto.Clear(); rx_size=tx_size=input_size=input_head=window_credit=0;
    encrypted_in=encrypted_out=skip_guess=channel_open=shell=closing=false;
    pending_interrupt=input_eof=false;
    keepalive_pending=false;
    auth_failures=auth_packets=0; total_bytes=0; in_sequence=out_sequence=0;
    client_window=receive_window=0; columns=80; rows=24;
    phase=Phase::BANNER; status="Connecting"; accepted_at=last_receive=ssh_platform_milliseconds();
    memcpy(tx,BANNER,strlen(BANNER)); tx_size=strlen(BANNER); tx[tx_size++]='\r'; tx[tx_size++]='\n';
    return true;
}
extern "C" size_t ssh_transport_wire_receive_space(void) { return closing?0:RX_CAP-rx_size; }
extern "C" bool ssh_transport_wire_receive(const uint8_t *data,size_t length) {
    if(phase==Phase::CLOSED || closing || length>RX_CAP-rx_size) return false;
    if(length) memcpy(rx+rx_size,data,length);
    rx_size+=length; return true;
}
extern "C" const uint8_t *ssh_transport_wire_output(size_t *length) { *length=tx_size; return tx; }
extern "C" void ssh_transport_wire_sent(size_t n) {
    n=std::min(n,tx_size); memmove(tx,tx+n,tx_size-n); tx_size-=n;
}
extern "C" bool ssh_transport_wire_should_close(void) {
    return closing && (!tx_size || uint32_t(ssh_platform_milliseconds()-closed_at)>250);
}
extern "C" void ssh_transport_wire_closed(void) {
    phase=Phase::CLOSED; shell=false; closing=false; rx_size=tx_size=input_size=input_head=0;
    mbedtls_platform_zeroize(rx,sizeof(rx)); mbedtls_platform_zeroize(tx,sizeof(tx));
    mbedtls_platform_zeroize(input,sizeof(input)); crypto.Clear();
    if(enabled) status="Ready";
}
