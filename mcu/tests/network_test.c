/* SPDX-License-Identifier: MIT. Compile the actual network implementation. */
#include "network.c"
#include "network_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

const char WIFI_EVENT[]="wifi";
const char IP_EVENT[]="ip";
static esp_netif_t ap_interface={.mode=WIFI_MODE_AP}, sta_interface={.mode=WIFI_MODE_STA};
static unsigned initializations, connects, disconnects, starts, servers, handlers, status_calls;
static unsigned saves, mode_changes, stops, power_saves;
static bool stored_enabled=true;
static esp_err_t power_save_error=ESP_OK, power_load_error=ESP_OK;
static int selected_mode;
static int socket_family=AF_INET;
static const char *native_ipv6;
static wifi_config_t ap_config, sta_config;
static httpd_uri_t routes[8];
static char response_status[32], response_body[8192];
static bool no_store=true;
static esp_err_t load_error=ESP_ERR_NVS_NOT_FOUND, save_error=ESP_OK, post_error=ESP_OK;
static wifi_profile_t stored_profile;
static int64_t mock_time;
static esp_timer_create_args_t timer_args;
struct registration { esp_event_base_t base; int32_t id; event_handler_t handler; void *arg; };
static struct registration registrations[8];
static unsigned registration_count;
struct queued_event { esp_event_base_t base; int32_t id; uint8_t data[512]; size_t size; };
static struct queued_event events[32];
static unsigned event_count;

esp_err_t wifi_store_load_enabled(bool *value) {
    if(power_load_error!=ESP_OK)return power_load_error;
    *value=stored_enabled;return ESP_OK;
}
esp_err_t wifi_store_save_enabled(bool value) {
    power_saves++;
    if(power_save_error!=ESP_OK)return power_save_error;
    stored_enabled=value;return ESP_OK;
}
esp_err_t nvs_flash_init(void) { initializations++;return ESP_OK; }
esp_err_t wifi_store_load(wifi_profile_t *profile) {
    if(load_error!=ESP_OK)return load_error;
    if(no_store)return ESP_ERR_NVS_NOT_FOUND;
    *profile=stored_profile;return ESP_OK;
}
esp_err_t wifi_store_save(const wifi_profile_t *profile) {
    saves++;assert(wifi_profile_valid(profile));
    if(save_error!=ESP_OK)return save_error;
    stored_profile=*profile;no_store=false;return ESP_OK;
}
esp_err_t wifi_store_setup_key(char output[WIFI_SETUP_PASSWORD_BYTES],const char candidate[WIFI_SETUP_PASSWORD_BYTES]) {
    memcpy(output,candidate,WIFI_SETUP_PASSWORD_BYTES);return ESP_OK;
}
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { return &ap_interface; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { return &sta_interface; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *p,esp_netif_ip_info_t *info) {
    assert(p==&ap_interface || p==&sta_interface);
    *info=p==&ap_interface ? (esp_netif_ip_info_t){.ip={{192,168,4,1}}} :
                               (esp_netif_ip_info_t){.ip={{192,0,2,1}}};return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *p) { assert(p);return ESP_OK; }
esp_err_t esp_wifi_set_storage(int mode) { assert(mode==WIFI_STORAGE_RAM);return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base,int32_t id,event_handler_t fn,void *arg) {
    assert(fn && registration_count<8);
    registrations[registration_count++]=(struct registration){base,id,fn,arg};return ESP_OK;
}
esp_err_t esp_event_post(esp_event_base_t base,int32_t id,const void *data,size_t size,uint32_t wait) {
    (void)wait;if(post_error!=ESP_OK)return post_error;
    assert(event_count<32 && size<=sizeof(events[0].data));
    events[event_count]=(struct queued_event){.base=base,.id=id,.size=size};
    if(size)memcpy(events[event_count].data,data,size);
    event_count++;return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int mode) { mode_changes++;selected_mode=mode;return ESP_OK; }
esp_err_t esp_wifi_set_config(int mode,const wifi_config_t *config) {
    if(mode==WIFI_IF_AP)ap_config=*config;
    else { assert(mode==WIFI_IF_STA);sta_config=*config; }
    return ESP_OK;
}
esp_err_t esp_wifi_start(void) { starts++;return ESP_OK; }
esp_err_t esp_wifi_stop(void) { stops++;return ESP_OK; }
esp_err_t esp_wifi_connect(void) { connects++;return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) { disconnects++;return ESP_OK; }
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode) { *mode=selected_mode;return ESP_OK; }
esp_err_t esp_wifi_get_mac(wifi_interface_t mode,uint8_t mac[6]) {
    (void)mode;const uint8_t fixed[6]={0x01,0x02,0x03,0xab,0xcd,0xef};memcpy(mac,fixed,6);return ESP_OK;
}
int64_t esp_timer_get_time(void) { return mock_time; }
uint32_t esp_random(void) { static uint32_t value=0x1234abcd;value=value*1664525u+1013904223u;return value; }
void esp_fill_random(void *output,size_t size) {
    uint8_t *bytes=output;for(size_t i=0;i<size;i++)bytes[i]=(uint8_t)esp_random();
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *out) {
    timer_args=*args;*out=&timer_args;return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle,uint64_t period) {
    assert(handle==&timer_args && period);return ESP_OK;
}
static const char *header(httpd_req_t *request,const char *name) {
    const char *value=!strcmp(name,"X-Device-Token")?request->token:
        !strcmp(name,"Host")?request->host:!strcmp(name,"Origin")?request->origin:
        !strcmp(name,"Content-Type")?request->content_type:!strcmp(name,"Accept")?request->accept:NULL;
    return value?value:"";
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *p,const char *name) { return strlen(header(p,name)); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *p,const char *name,char *out,size_t n) {
    const char *value=header(p,name);if(strlen(value)>=n)return ESP_FAIL;strcpy(out,value);return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *p,const char *status) { (void)p;snprintf(response_status,sizeof(response_status),"%s",status);return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *p,const char *s) { (void)p;snprintf(response_body,sizeof(response_body),"%s",s);return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *p,const char *a,const char *b) { (void)p;(void)a;(void)b;return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *p,const char *s) { (void)p;(void)s;return ESP_OK; }
int httpd_req_recv(httpd_req_t *p,char *out,size_t n) {
    if(!p->body || p->received>=p->content_len)return ESP_FAIL;
    const size_t remaining=p->content_len-p->received;
    if(n>remaining)n=remaining;
    if(n>17)n=17; /* Exercise split HTTP receives. */
    memcpy(out,p->body+p->received,n);p->received+=n;return (int)n;
}
int httpd_req_to_sockfd(httpd_req_t *request) { return request->ap_socket?41:42; }
int getsockname(int socket,struct sockaddr *address,socklen_t *length) {
    if(socket_family==AF_INET6) {
        assert(*length>=sizeof(struct sockaddr_in6));
        struct sockaddr_in6 local={.sin6_family=AF_INET6,.sin6_port=htons(80)};
        const char *ip=native_ipv6?native_ipv6:(socket==41?"::ffff:192.168.4.1":"::ffff:192.0.2.1");
        assert(inet_pton(AF_INET6,ip,&local.sin6_addr)==1);
        memcpy(address,&local,sizeof(local));*length=sizeof(local);return 0;
    }
    assert(*length>=sizeof(struct sockaddr_in));
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(80)};
    local.sin_addr.s_addr=inet_addr(socket==41?"192.168.4.1":"192.0.2.1");
    memcpy(address,&local,sizeof(local));*length=sizeof(local);return 0;
}
size_t httpd_req_get_url_query_len(httpd_req_t *p) { (void)p;return 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *p,char *out,size_t n) { (void)p;(void)out;(void)n;return ESP_FAIL; }
esp_err_t httpd_start(httpd_handle_t *out,const httpd_config_t *config) {
    assert(starts==1);
    assert(config->max_uri_handlers==8 && config->max_open_sockets==3 && config->stack_size==8192);
    *out=&ap_interface;servers++;return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route) {
    assert(server==&ap_interface && handlers<8);routes[handlers++]=*route;return ESP_OK;
}
void control_status(char *out,size_t n) { status_calls++;snprintf(out,n,"{}"); }
void control_text(char *out,size_t n) { snprintf(out,n,"APPLE II"); }
bool control_command(const char *in,char *out,size_t n) { (void)in;(void)out;(void)n;return false; }
bool control_import_begin(const char *name) { (void)name;return false; }
bool control_import_write(const void *p,size_t n) { (void)p;(void)n;return false; }
bool control_import_finish(void) { return false; }
void control_import_abort(void) {}
int control_storage_error(void) { return 0; }
#if DEVICE_WIFI_ENABLED
static void deliver(esp_event_base_t base,int32_t id,void *data) {
    for(unsigned i=0;i<registration_count;i++) {
        const struct registration *r=&registrations[i];
        if(r->base==base && (r->id==id || r->id==ESP_EVENT_ANY_ID))r->handler(r->arg,base,id,data);
    }
}
static void drain(void) {
    unsigned iterations=0;
    while(event_count) {
        assert(iterations++<64);
        struct queued_event event=events[0];
        memmove(events,events+1,--event_count*sizeof(events[0]));
        deliver(event.base,event.id,event.size?event.data:NULL);
    }
}
#endif

#if DEVICE_WIFI_ENABLED
static esp_err_t request_route(httpd_req_t *request)
{
    strcpy(response_status,"200 OK");response_body[0]=0;request->received=0;
    for(unsigned i=0;i<handlers;i++)
        if(routes[i].method==request->method && !strcmp(routes[i].uri,request->uri))
            return routes[i].handler(request);
    assert(!"missing route");return ESP_FAIL;
}
static httpd_req_t setup_request(int method,const char *uri,const char *body)
{
    return (httpd_req_t){.method=method,.uri=uri,.content_len=body?strlen(body):0,
        .host="192.168.4.1",.origin="http://192.168.4.1",.accept="application/json",
        .content_type="application/x-www-form-urlencoded",.body=body,.ap_socket=true};
}
static void read_nonce(char nonce[33])
{
    httpd_req_t request=setup_request(HTTP_GET,"/wifi",NULL);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"200 OK"));
    assert(strstr(response_body,"Chromatic Wi-Fi setup"));
    assert(!strstr(response_body,DEVICE_WIFI_PASSWORD));
    const char *value=strstr(response_body,"name=csrf value='");assert(value);
    memcpy(nonce,value+strlen("name=csrf value='"),32);nonce[32]=0;
}
static void check_protected_setup(void)
{
    assert(network_setup_start());
    drain();
    const bool was_station=profile.mode==WIFI_PROFILE_STATION;
    assert(selected_mode==(was_station?WIFI_MODE_APSTA:WIFI_MODE_AP));
    assert(ap_config.ap.authmode==WIFI_AUTH_WPA2_PSK && ap_config.ap.max_connection==1);
    assert(ap_config.ap.channel==1 && ap_config.ap.beacon_interval==100);
    assert(!strcmp((const char *)ap_config.ap.ssid,"CHROMATIC-ABCDEF"));
    assert(ap_config.ap.ssid_len==16);
    assert(wifi_setup_password_valid((const char *)ap_config.ap.password));
    deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    assert(network_ready());
    network_setup_info_t info;network_setup_info(&info);
    assert(!strcmp(info.ssid,"CHROMATIC-ABCDEF") && wifi_setup_password_valid(info.password));
    assert(!strcmp(info.url,"HTTP://192.168.4.1"));
    char nonce[33];read_nonce(nonce);
    httpd_req_t request=setup_request(HTTP_GET,"/",NULL);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"200 OK"));
    request.ap_socket=false;
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    socket_family=AF_INET6;request.ap_socket=true;
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"200 OK"));
    request.ap_socket=false;
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    request.ap_socket=true;
    const char *native_addresses[]={"::192.168.4.1","2001:db8::c0a8:401","::1"};
    for(unsigned i=0;i<sizeof(native_addresses)/sizeof(native_addresses[0]);i++) {
        native_ipv6=native_addresses[i];
        assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    }
    native_ipv6=NULL;
    /* Keep mapped IPv6 for all following save/status requests: ESP-IDF's
     * dual-stack HTTP listener reports IPv4 clients this way on hardware. */
    request.host="attacker.example";
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    request.host="192.168.4.1";request.origin="https://attacker.example";
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    request.origin=NULL;request.host="192.168.4.1:80";
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"200 OK"));
    char form[600];
    const char *bad[] = {
        "mode=station&ssid=&password=valid-password&csrf=%s",
        "mode=station&ssid=123456789012345678901234567890123&password=valid-password&csrf=%s",
        "mode=station&ssid=network&password=short&csrf=%s",
        "mode=hotspot&ssid=network&password=&csrf=%s",
        "mode=station&ssid=network&password=valid-password&csrf=invalid%s",
        "mode=station&ssid=network&password=valid-password&csrf=%s&ssid=duplicate",
    };
    const unsigned old_saves=saves,old_changes=mode_changes;
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        snprintf(form,sizeof(form),bad[i],nonce);
        request=setup_request(HTTP_POST,"/wifi/save",form);
        assert(request_route(&request)==ESP_OK && !strcmp(response_status,"400 Bad Request"));
        assert(saves==old_saves && mode_changes==old_changes && event_count==0);
    }
    snprintf(form,sizeof(form),"mode=station&ssid=New+Network&password=new-password&csrf=%s",nonce);
    request=setup_request(HTTP_POST,"/wifi/save",form);request.ap_socket=false;
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    assert(saves==old_saves);
    request.ap_socket=true;request.origin="http://elsewhere.example";
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    assert(saves==old_saves);
    request.origin="http://192.168.4.1";request.content_type="application/json";
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"415 Unsupported Media Type"));
    assert(saves==old_saves);
    request.content_type="application/x-www-form-urlencoded";save_error=ESP_FAIL;
    const wifi_profile_t old_profile=current_profile();
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"500 Internal Server Error"));
    assert(saves==old_saves+1 && mode_changes==old_changes && event_count==0);
    const wifi_profile_t after_failure=current_profile();
    assert(!memcmp(&old_profile,&after_failure,sizeof(old_profile)));
    assert(!strstr(response_body,"new-password"));
    save_error=ESP_OK;post_error=ESP_FAIL;
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"503 Service Unavailable"));
    assert(event_count==0 && mode_changes==old_changes);
    const wifi_profile_t after_post_failure=current_profile();
    assert(!memcmp(&old_profile,&after_post_failure,sizeof(old_profile)));
    assert(!strcmp(stored_profile.ssid,"New Network"));
    post_error=ESP_OK;
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"200 OK"));
    assert(strstr(response_body,"\"ok\":true") && event_count==1);
    assert(stored_profile.mode==WIFI_PROFILE_STATION && !strcmp(stored_profile.ssid,"New Network"));
    assert(mode_changes==old_changes); /* HTTP has saved but has not touched Wi-Fi yet. */
    drain();
    assert(selected_mode==WIFI_MODE_APSTA && !strcmp((const char *)sta_config.sta.ssid,"New Network"));
    assert(!strcmp((const char *)sta_config.sta.password,"new-password"));
    assert(!strcmp((const char *)ap_config.ap.ssid,"CHROMATIC-ABCDEF"));
    ip_event_got_ip_t acquired={.ip_info={.ip={{192,0,2,7}}}};
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(network_ready() && !strcmp(network_ip(),"192.0.2.7"));
    request=setup_request(HTTP_GET,"/wifi/status",NULL);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"connected") && strstr(response_body,"192.0.2.7"));
    network_setup_info(&info);assert(strstr(info.status,"192.0.2.7"));
    const unsigned before_reconnect=connects;
    deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    assert(connects==before_reconnect+1 && network_ready() && !strcmp(network_ip(),"192.168.4.1"));
    network_setup_stop();drain();
    assert(selected_mode==WIFI_MODE_STA);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    deliver(WIFI_EVENT,WIFI_EVENT_AP_STOP,NULL);
    assert(!network_ready() && !strcmp(network_ip(),"0.0.0.0"));
    /* Failed station connections recover without requiring the old network. */
    mock_time+=24999999;timer_args.callback(timer_args.arg);drain();
    assert(selected_mode==WIFI_MODE_STA);
    mock_time+=1;timer_args.callback(timer_args.arg);drain();
    assert(selected_mode==WIFI_MODE_AP);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    assert(network_ready() && !strcmp(network_ip(),"192.168.4.1"));
    const unsigned failed_connects=connects;
    deliver(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(connects==failed_connects && selected_mode==WIFI_MODE_AP);
    assert(!atomic_load(&station_connected) && !strcmp(network_ip(),"192.168.4.1"));
    request=setup_request(HTTP_GET,"/wifi/status",NULL);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"failed"));
    /* Closing failed setup explicitly retries the unchanged saved station. */
    network_setup_stop();drain();
    assert(selected_mode==WIFI_MODE_STA && connects>failed_connects);
    assert(atomic_load(&connection_state)==CONNECTING);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_STOP,NULL);
    assert(!network_ready());
    mock_time+=25000000;timer_args.callback(timer_args.arg);drain();
    assert(selected_mode==WIFI_MODE_AP);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    char renewed[33];read_nonce(renewed);assert(strcmp(renewed,nonce));
    /* Saving new details re-enables station mode while retaining setup. */
    const unsigned before_new_station=connects;
    snprintf(form,sizeof(form),"mode=station&ssid=Recovered+Network&password=recovered-password&csrf=%s",renewed);
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"\"ok\":true"));
    drain();
    assert(selected_mode==WIFI_MODE_APSTA && connects>before_new_station);
    assert(atomic_load(&connection_state)==CONNECTING);
    assert(!strcmp((const char *)sta_config.sta.ssid,"Recovered Network"));
    /* A second failure must stop scanning even though setup is already open. */
    mock_time+=25000000;timer_args.callback(timer_args.arg);drain();
    assert(selected_mode==WIFI_MODE_AP && atomic_load(&connection_state)==FAILED);
    const unsigned second_failure_connects=connects;
    deliver(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(connects==second_failure_connects && !atomic_load(&station_connected));
    assert(!strcmp(network_ip(),"192.168.4.1"));
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"\"ok\":true"));
    drain();
    assert(selected_mode==WIFI_MODE_APSTA && connects>second_failure_connects);
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(atomic_load(&connection_state)==CONNECTED && !strcmp(network_ip(),"192.0.2.7"));
    snprintf(form,sizeof(form),"mode=hotspot&ssid=Travel+Chromatic&password=travel-password&csrf=%s",nonce);
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"400 Bad Request"));
    snprintf(form,sizeof(form),"mode=hotspot&ssid=Travel+Chromatic&password=travel-password&csrf=%s",renewed);
    request=setup_request(HTTP_POST,"/wifi/save",form);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"\"ok\":true"));
    drain();
    assert(selected_mode==WIFI_MODE_AP && stored_profile.mode==WIFI_PROFILE_HOTSPOT);
    assert(!strcmp((const char *)ap_config.ap.ssid,"CHROMATIC-ABCDEF"));
    /* A DHCP event queued before changing modes cannot resurrect station IP. */
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(!strcmp(network_ip(),"192.168.4.1"));
    request=setup_request(HTTP_GET,"/wifi/status",NULL);
    assert(request_route(&request)==ESP_OK && strstr(response_body,"hotspot"));
    network_setup_stop();drain();
    assert(selected_mode==WIFI_MODE_AP && !strcmp((const char *)ap_config.ap.ssid,"Travel Chromatic"));
    assert(!strcmp((const char *)ap_config.ap.password,"travel-password"));
    assert(request_route(&request)==ESP_OK && !strcmp(response_status,"403 Forbidden"));
    assert(servers==1 && handlers==8);
}
#endif

int main(int argc,char **argv) {
#if DEVICE_WIFI_ENABLED
    const char *scenario=argc>1?argv[1]:"defaults";
    if(!strcmp(scenario,"stored-station") || !strcmp(scenario,"stored-hotspot")) {
        no_store=false;load_error=ESP_OK;
        stored_profile=(wifi_profile_t){.mode=!strcmp(scenario,"stored-hotspot")?
            WIFI_PROFILE_HOTSPOT:WIFI_PROFILE_STATION,.ssid="Saved Network",.password="saved-password"};
    } else if(!strcmp(scenario,"load-failed"))load_error=ESP_FAIL;
    if(!strcmp(scenario,"power-off"))stored_enabled=false;
    const wifi_profile_t expected=no_store ? (wifi_profile_t){
        .mode=DEVICE_WIFI_AP?WIFI_PROFILE_HOTSPOT:WIFI_PROFILE_STATION,
        .ssid=DEVICE_WIFI_SSID,.password=DEVICE_WIFI_PASSWORD} : stored_profile;
#else
    (void)argc;(void)argv;
#endif
    assert(!network_ready() && !strcmp(network_ip(),"0.0.0.0"));
    assert(!network_setup_start());
    network_start();
#if !DEVICE_WIFI_ENABLED
    assert(!initializations && !starts && !servers && !network_ready());
    network_setup_info_t info;network_setup_info(&info);
    assert(!info.ssid[0] && !info.password[0] && strstr(info.status,"NOT AVAILABLE"));
    network_setup_stop();
#else
    if(!strcmp(scenario,"power-off")) {
        assert(initializations==1 && !starts && !servers && !network_ready());
        network_setup_info_t info;network_setup_info(&info);
        assert(!info.enabled && !strcmp(info.network_ssid,expected.ssid));
        mock_time=120000000;timer_args.callback(NULL);drain();
        assert(!starts && !setup_active);
        ip_event_got_ip_t stale={.ip_info={.ip={{192,0,2,77}}}};
        deliver(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
        deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
        deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&stale);
        assert(!connects && !network_ready());
        power_save_error=ESP_FAIL;
        assert(network_set_enabled(true));drain();
        network_setup_info(&info);assert(!info.enabled && strstr(info.state,"COULD NOT SAVE"));
        assert(!starts);
        power_save_error=ESP_OK;
        assert(network_set_enabled(true));drain();
        network_setup_info(&info);assert(info.enabled && starts==1);
        assert(profile.mode==expected.mode && !strcmp(profile.ssid,expected.ssid));
        assert(!strcmp(profile.password,expected.password));
        if(expected.mode==WIFI_PROFILE_HOTSPOT)deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
        else deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&stale);
        assert(network_ready());
        assert(network_set_enabled(false));drain();
        assert(stops==1 && !network_ready() && !strcmp(network_ip(),"0.0.0.0"));
        assert(!stored_enabled);
        deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
        deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
        deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&stale);
        assert(!network_ready());
        mock_time+=120000000;timer_args.callback(NULL);drain();
        assert(!setup_active && starts==1);
        assert(network_setup_start());drain();
        network_setup_info(&info);
        assert(info.enabled && stored_enabled && setup_active && starts==2);
        assert(!strcmp(info.network_ssid,expected.ssid));
        if(expected.mode==WIFI_PROFILE_STATION)assert(connecting_since==mock_time);
        network_setup_stop();drain();
        assert(!setup_active && stored_enabled && profile.mode==expected.mode);
        puts("PASS actual networking power preference, off boot, failure isolation, stale events and explicit setup recovery");
        return 0;
    }
    assert(initializations==1 && starts==1 && !servers && !network_ready());
    assert(timer_args.callback);
    if(expected.mode==WIFI_PROFILE_HOTSPOT) {
        assert(selected_mode==WIFI_MODE_AP);
        assert(ap_config.ap.authmode==WIFI_AUTH_WPA2_PSK && ap_config.ap.max_connection==1);
        assert(ap_config.ap.ssid_len==strlen(expected.ssid));
        assert(!memcmp(ap_config.ap.ssid,expected.ssid,strlen(expected.ssid)));
        assert(!memcmp(ap_config.ap.password,expected.password,strlen(expected.password)));
        deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
        assert(network_ready() && !strcmp(network_ip(),"192.168.4.1") && !connects);
        deliver(WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL);
        assert(network_ready() && servers==1);
        deliver(WIFI_EVENT,WIFI_EVENT_AP_STOP,NULL);
        assert(!network_ready() && !strcmp(network_ip(),"0.0.0.0"));
        deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
        assert(network_ready() && servers==1);
    } else {
        assert(selected_mode==WIFI_MODE_STA);
        assert(!memcmp(sta_config.sta.ssid,expected.ssid,strlen(expected.ssid)));
        assert(!memcmp(sta_config.sta.password,expected.password,strlen(expected.password)));
        deliver(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
        assert(connects==1 && !servers && !network_ready());
        ip_event_got_ip_t acquired={.ip_info={.ip={{192,0,2,1}}}};
        deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
        assert(network_ready() && !strcmp(network_ip(),"192.0.2.1"));
        deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
        assert(connects==2 && !network_ready() && !strcmp(network_ip(),"0.0.0.0"));
        deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
        assert(network_ready() && servers==1);
    }
    assert(handlers==8);
    httpd_req_t request={.method=HTTP_GET,.uri="/status",.token="bad-token"};
    assert(request_route(&request)==ESP_OK && !status_calls && !strcmp(response_status,"401 Unauthorized"));
    request.token=DEVICE_API_TOKEN;
    assert(request_route(&request)==ESP_OK && status_calls==1);
    check_protected_setup();
#endif
    puts("PASS actual networking: disabled/default/saved station and hotspot, AP-only protected setup over IPv4 and mapped IPv6, native IPv6 rejected, CSRF/origin/host rejection, form validation, failed commit keeps active connection, asynchronous save/apply, reconnect and AP-only failed-station recovery/retry");
    return 0;
}
