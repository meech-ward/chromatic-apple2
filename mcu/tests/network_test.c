/* SPDX-License-Identifier: MIT. Compile the actual network implementation. */
#include "network.c"
#include "network_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const char WIFI_EVENT[]="wifi";
const char IP_EVENT[]="ip";
static esp_netif_t interface;
static unsigned initializations, connects, starts, servers, handlers, status_calls;
static int selected_mode, selected_interface;
static bool delivered_event;
static wifi_config_t selected_config;
static event_handler_t event_handler;
static httpd_uri_t routes[4];
static char response_status[32];
esp_err_t nvs_flash_init(void) { initializations++;return ESP_OK; }
esp_err_t esp_netif_init(void) { return ESP_OK; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_ap(void) { interface.mode=WIFI_MODE_AP;return &interface; }
esp_netif_t *esp_netif_create_default_wifi_sta(void) { interface.mode=WIFI_MODE_STA;return &interface; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *p,esp_netif_ip_info_t *info) {
    assert(p==&interface && interface.mode==WIFI_MODE_AP);
    *info=(esp_netif_ip_info_t){.ip={{192,168,4,1}}};return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *p) { assert(p);return ESP_OK; }
esp_err_t esp_wifi_set_storage(int mode) { assert(mode==WIFI_STORAGE_RAM);return ESP_OK; }
esp_err_t esp_event_handler_register(esp_event_base_t base,int32_t id,event_handler_t fn,void *arg) {
    (void)base;(void)id;assert(!arg && fn);event_handler=fn;return ESP_OK;
}
esp_err_t esp_wifi_set_mode(int mode) { selected_mode=mode;return ESP_OK; }
esp_err_t esp_wifi_set_config(int mode,const wifi_config_t *config) {
    selected_interface=mode;selected_config=*config;return ESP_OK;
}
esp_err_t esp_wifi_start(void) { starts++;return ESP_OK; }
esp_err_t esp_wifi_connect(void) { connects++;return ESP_OK; }
size_t httpd_req_get_hdr_value_len(httpd_req_t *p,const char *name) { (void)name;return strlen(p->token); }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *p,const char *name,char *out,size_t n) {
    (void)name;if(strlen(p->token)>=n)return ESP_FAIL;strcpy(out,p->token);return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *p,const char *status) { (void)p;strcpy(response_status,status);return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *p,const char *s) { (void)p;(void)s;return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *p,const char *a,const char *b) { (void)p;(void)a;(void)b;return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *p,const char *s) { (void)p;(void)s;return ESP_OK; }
int httpd_req_recv(httpd_req_t *p,char *out,size_t n) { (void)p;(void)out;(void)n;return ESP_FAIL; }
size_t httpd_req_get_url_query_len(httpd_req_t *p) { (void)p;return 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *p,char *out,size_t n) { (void)p;(void)out;(void)n;return ESP_FAIL; }
esp_err_t httpd_start(httpd_handle_t *out,const httpd_config_t *config) {
    assert(starts==1 && delivered_event && !network_ready());
    assert(config->max_uri_handlers==4 && config->max_open_sockets==3 && config->stack_size==8192);
    *out=&interface;servers++;return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t server,const httpd_uri_t *route) {
    assert(server==&interface && handlers<4);routes[handlers++]=*route;return ESP_OK;
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
    delivered_event=true;event_handler(NULL,base,id,data);delivered_event=false;
}
#endif
int main(void) {
    assert(!network_ready() && !strcmp(network_ip(),"0.0.0.0"));
    network_start();
#if !DEVICE_WIFI_ENABLED
    assert(!initializations && !starts && !servers && !network_ready());
#else
    assert(initializations==1 && starts==1 && !servers && !network_ready());
#if DEVICE_WIFI_AP
    assert(selected_mode==WIFI_MODE_AP && interface.mode==WIFI_MODE_AP && selected_interface==WIFI_IF_AP);
    assert(selected_config.ap.authmode==WIFI_AUTH_WPA2_PSK && selected_config.ap.max_connection==1);
    assert(selected_config.ap.ssid_len==strlen(DEVICE_WIFI_SSID));
    assert(!memcmp(selected_config.ap.ssid,DEVICE_WIFI_SSID,strlen(DEVICE_WIFI_SSID)));
    assert(!memcmp(selected_config.ap.password,DEVICE_WIFI_PASSWORD,strlen(DEVICE_WIFI_PASSWORD)));
    assert(selected_config.ap.channel==1 && selected_config.ap.beacon_interval==100);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    assert(network_ready() && !strcmp(network_ip(),"192.168.4.1") && !connects);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,NULL);
    assert(network_ready() && servers==1);
    deliver(WIFI_EVENT,WIFI_EVENT_AP_STOP,NULL);
    assert(!network_ready() && !strcmp(network_ip(),"0.0.0.0"));
    deliver(WIFI_EVENT,WIFI_EVENT_AP_START,NULL);
    assert(network_ready() && servers==1);
#else
    assert(selected_mode==WIFI_MODE_STA && interface.mode==WIFI_MODE_STA && selected_interface==WIFI_IF_STA);
    assert(!memcmp(selected_config.sta.ssid,DEVICE_WIFI_SSID,strlen(DEVICE_WIFI_SSID)));
    assert(!memcmp(selected_config.sta.password,DEVICE_WIFI_PASSWORD,strlen(DEVICE_WIFI_PASSWORD)));
    deliver(WIFI_EVENT,WIFI_EVENT_STA_START,NULL);
    assert(connects==1 && !servers && !network_ready());
    ip_event_got_ip_t acquired={.ip_info={.ip={{192,0,2,1}}}};
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(network_ready() && !strcmp(network_ip(),"192.0.2.1"));
    deliver(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);
    assert(connects==2 && !network_ready() && !strcmp(network_ip(),"0.0.0.0"));
    deliver(IP_EVENT,IP_EVENT_STA_GOT_IP,&acquired);
    assert(network_ready() && servers==1);
#endif
    assert(handlers==4);
    httpd_req_t request={.method=HTTP_GET,.uri="/status",.token="bad-token"};
    assert(routes[0].handler(&request)==ESP_OK && !status_calls && !strcmp(response_status,"401 Unauthorized"));
    request.token=DEVICE_API_TOKEN;
    assert(routes[0].handler(&request)==ESP_OK && status_calls==1);
#endif
    puts("PASS actual networking: disabled/station/hotspot readiness, protected AP, delayed HTTP, reconnect, token auth");
    return 0;
}
