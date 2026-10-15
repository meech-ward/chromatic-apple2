/* SPDX-License-Identifier: MIT */
#include "control.h"
#include "network.h"
#include "private_config.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_settings.h"
#include "wifi_store.h"
#include "wifi_portal.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"

static char device_ip[16] = "0.0.0.0";
const char *network_ip(void) { return device_ip; }
static atomic_bool ready;
bool network_ready(void) { return atomic_load(&ready); }

#ifndef DEVICE_WIFI_AP
#define DEVICE_WIFI_AP 0
#endif

#if DEVICE_WIFI_ENABLED
#if defined(CONFIG_HTTPD_MAX_REQ_HDR_LEN) && CONFIG_HTTPD_MAX_REQ_HDR_LEN < 1536
#error "Wi-Fi setup needs CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048 for normal browsers"
#endif
static char response[6144];
static bool server_started;

static bool authorized(httpd_req_t *request)
{
    char token[129];
    const size_t expected = strlen(DEVICE_API_TOKEN);
    if (httpd_req_get_hdr_value_len(request, "X-Device-Token") != expected ||
        httpd_req_get_hdr_value_str(request, "X-Device-Token", token, sizeof(token)) != ESP_OK)
        return false;
    unsigned difference = 0;
    for (size_t i = 0; i < expected; ++i) difference |= token[i] ^ DEVICE_API_TOKEN[i];
    return difference == 0;
}

static esp_err_t handle(httpd_req_t *request)
{
    if (!authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        return httpd_resp_sendstr(request, "Unauthorized\n");
    }
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (request->method == HTTP_GET && !strcmp(request->uri, "/status")) {
        control_status(response, sizeof(response));
        httpd_resp_set_type(request, "application/json");
    } else if (request->method == HTTP_GET && !strcmp(request->uri, "/text")) {
        control_text(response, sizeof(response));
        httpd_resp_set_type(request, "text/plain");
    } else {
        if (request->content_len < 1 || request->content_len > 255) {
            httpd_resp_set_status(request, "400 Bad Request");
            return httpd_resp_sendstr(request, "Command body must contain 1..255 bytes\n");
        }
        char command[256];
        unsigned received = 0;
        while (received < request->content_len) {
            const int count = httpd_req_recv(request, command + received, request->content_len - received);
            if (count <= 0) return ESP_FAIL;
            received += count;
        }
        if (memchr(command, 0, received)) {
            httpd_resp_set_status(request, "400 Bad Request");
            return httpd_resp_sendstr(request, "Command cannot contain NUL\n");
        }
        command[received] = 0;
        const bool ok = control_command(command, response, sizeof(response));
        httpd_resp_set_type(request, "text/plain");
        if (!ok) httpd_resp_set_status(request, "400 Bad Request");
    }
    return httpd_resp_sendstr(request, response);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool import_name(httpd_req_t *request, char name[64])
{
    const size_t query_size = httpd_req_get_url_query_len(request);
    if (!query_size) {
        strcpy(name, "Workshop.dsk");
        return true;
    }
    /* A 63-byte basename may occupy 189 bytes when percent-encoded. Only one
     * name parameter is accepted, so duplicates cannot select a different file.
     */
    char query[3 * 63 + 6];
    if (query_size >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        strncmp(query, "name=", 5) || strchr(query + 5, '&')) return false;
    size_t used = 0;
    for (const char *source = query + 5; *source; ++source) {
        unsigned char value = (unsigned char)*source;
        if (value == '%') {
            if (!source[1] || !source[2]) return false;
            const int high = hex_digit(source[1]), low = hex_digit(source[2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            source += 2;
        } else if (value == '+') value = ' ';
        if (!value || used >= 63) return false;
        name[used++] = (char)value;
    }
    name[used] = 0;
    return used != 0;
}

static esp_err_t import_failed(httpd_req_t *request)
{
    const int error = control_storage_error();
    control_import_abort();
    char message[160];
    snprintf(message, sizeof(message),
             "Disk import failed (storage error %d). Upload outcome is unconfirmed; no retry was made.\n",
             error);
    httpd_resp_set_status(request, "500 Internal Server Error");
    (void)httpd_resp_sendstr(request, message);
    /* A failure may leave unread body bytes. Close instead of reusing them as
     * another HTTP request; the client must inspect state before any retry.
     */
    return ESP_FAIL;
}

static esp_err_t handle_disk(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (!authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        (void)httpd_resp_sendstr(request, "Unauthorized\n");
        return ESP_FAIL;
    }
    if (request->content_len != 143360) {
        httpd_resp_set_status(request, "400 Bad Request");
        (void)httpd_resp_sendstr(request, "Disk body must contain exactly 143360 bytes\n");
        return ESP_FAIL;
    }
    char name[64];
    if (!import_name(request, name)) {
        httpd_resp_set_status(request, "400 Bad Request");
        (void)httpd_resp_sendstr(request, "Use one name query parameter of at most 63 decoded bytes\n");
        return ESP_FAIL;
    }
    if (!control_import_begin(name)) return import_failed(request);
    char buffer[1024];
    size_t remaining = request->content_len;
    while (remaining) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const int count = httpd_req_recv(request, buffer, wanted);
        if (count <= 0) {
            control_import_abort();
            httpd_resp_set_status(request, count == HTTPD_SOCK_ERR_TIMEOUT ?
                                 "408 Request Timeout" : "400 Bad Request");
            (void)httpd_resp_sendstr(request, "Disk transfer interrupted; import aborted, no retry was made\n");
            return ESP_FAIL;
        }
        if (!control_import_write(buffer, (size_t)count)) return import_failed(request);
        remaining -= (size_t)count;
    }
    if (!control_import_finish()) return import_failed(request);
    char message[112];
    snprintf(message, sizeof(message), "Imported %s: 143360 bytes\n", name);
    httpd_resp_set_status(request, "201 Created");
    return httpd_resp_sendstr(request, message);
}

static void start_server(void);
static void refresh_address(void);

enum { SETUP_OPEN, SETUP_CLOSE, PROFILE_APPLY, NETWORK_TICK, POWER_SET };
enum { CONNECTING, CONNECTED, FAILED, HOTSPOT, SAVE_FAILED, OFF };
ESP_EVENT_DEFINE_BASE(A2_WIFI_EVENT);
static esp_netif_t *station_netif, *ap_netif;
static wifi_profile_t profile;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool initialized, setup_active, station_connected, ap_started, enabled;
static atomic_bool radio_started;
static bool setup_key_retained;
static atomic_uint connection_state;
static char station_ip[16], setup_ssid[33], setup_password[WIFI_SETUP_PASSWORD_BYTES], setup_csrf[33];
static int64_t connecting_since;

static wifi_profile_t current_profile(void)
{
    wifi_profile_t copy;
    portENTER_CRITICAL(&state_lock);
    copy=profile;
    portEXIT_CRITICAL(&state_lock);
    return copy;
}

static void random_hex(char *out, size_t digits)
{
    static const char alphabet[]="0123456789ABCDEF";
    uint8_t bytes[16];
    esp_fill_random(bytes,sizeof(bytes));
    for (size_t i=0;i<digits;++i) out[i]=alphabet[(bytes[i/2] >> ((i&1) ? 0 : 4))&15];
    out[digits]=0;
}

static void random_setup_password(char output[WIFI_SETUP_PASSWORD_BYTES])
{
    uint8_t entropy[WIFI_SETUP_PASSWORD_ENTROPY_BYTES];
    do {
        esp_fill_random(entropy,sizeof(entropy));
    } while (!wifi_setup_password_generate(output,entropy));
}

bool network_set_enabled(bool value)
{
    return atomic_load(&initialized) &&
        esp_event_post(A2_WIFI_EVENT,POWER_SET,&value,sizeof(value),0)==ESP_OK;
}

bool network_setup_start(void)
{
    return atomic_load(&initialized) &&
        esp_event_post(A2_WIFI_EVENT,SETUP_OPEN,NULL,0,0)==ESP_OK;
}

void network_setup_stop(void)
{
    if (atomic_load(&initialized))
        (void)esp_event_post(A2_WIFI_EVENT,SETUP_CLOSE,NULL,0,0);
}

void network_setup_info(network_setup_info_t *out)
{
    memset(out,0,sizeof(*out));
    portENTER_CRITICAL(&state_lock);
    snprintf(out->ssid,sizeof(out->ssid),"%s",setup_ssid);
    snprintf(out->password,sizeof(out->password),"%s",setup_password);
    char ip[16]; snprintf(ip,sizeof(ip),"%s",device_ip);
    const bool hotspot=profile.mode==WIFI_PROFILE_HOTSPOT;
    out->hotspot=hotspot;
    out->enabled=atomic_load(&enabled);
    snprintf(out->network_ssid,sizeof(out->network_ssid),"%s",profile.ssid);
    portEXIT_CRITICAL(&state_lock);
    const unsigned state=atomic_load(&connection_state);
    if (state==SAVE_FAILED) strcpy(out->state,"COULD NOT SAVE - PLEASE TRY AGAIN");
    else if (!atomic_load(&initialized)) strcpy(out->state,"WI-FI IS NOT AVAILABLE");
    else if (!out->enabled) strcpy(out->state,"OFF - YOUR SETTINGS ARE KEPT");
    else if (atomic_load(&setup_active)) strcpy(out->state,"SETUP NETWORK IS OPEN");
    else if (hotspot) strcpy(out->state,"HOTSPOT STARTS AFTER EVERY RESTART");
    else if (atomic_load(&station_connected)) strcpy(out->state,"CONNECTED TO YOUR NETWORK");
    else if (state==FAILED) strcpy(out->state,"NOT CONNECTED - OPEN SETUP");
    else strcpy(out->state,"CONNECTING TO YOUR NETWORK...");
    strcpy(out->url,"HTTP://192.168.4.1");
    if (!atomic_load(&initialized)) strcpy(out->status,"WI-FI IS NOT AVAILABLE");
    else if (atomic_load(&connection_state)==SAVE_FAILED) strcpy(out->status,"SAVE FAILED - PLEASE TRY AGAIN");
    else if (!atomic_load(&setup_active)) strcpy(out->status,"STARTING SETUP NETWORK...");
    else if (hotspot) strcpy(out->status,"HOTSPOT MODE - B TO APPLY AND CLOSE");
    else if (atomic_load(&station_connected)) snprintf(out->status,sizeof(out->status),"CONNECTED: %s",ip);
    else if (atomic_load(&connection_state)==FAILED) strcpy(out->status,"NOT CONNECTED - CHECK YOUR DETAILS");
    else strcpy(out->status,"ENTER YOUR DETAILS IN THE BROWSER");
}

/* The setup form is available only through the protected setup AP, never the
 * router-facing interface. CSRF additionally prevents blind browser requests. */
static bool setup_client(httpd_req_t *request)
{
    if (!atomic_load(&setup_active)) return false;
    struct sockaddr_storage local={0}; socklen_t length=sizeof(local);
    if (getsockname(httpd_req_to_sockfd(request),(struct sockaddr *)&local,&length)) return false;
    bool setup_address=false;
    if (local.ss_family==AF_INET && length>=sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4=(const struct sockaddr_in *)&local;
        setup_address=ntohl(ipv4->sin_addr.s_addr)==0xc0a80401u;
    } else if (local.ss_family==AF_INET6 && length>=sizeof(struct sockaddr_in6)) {
        /* ESP-IDF's dual-stack HTTP listener reports IPv4 clients as mapped
         * IPv6 addresses. Accept only the exact setup destination in either form. */
        const struct sockaddr_in6 *ipv6=(const struct sockaddr_in6 *)&local;
        const uint8_t mapped[16]={0,0,0,0,0,0,0,0,0,0,255,255,192,168,4,1};
        setup_address=!memcmp(&ipv6->sin6_addr,mapped,sizeof(mapped));
    }
    if (!setup_address) return false;
    char host[40];
    if (httpd_req_get_hdr_value_str(request,"Host",host,sizeof(host))!=ESP_OK ||
        (strcmp(host,"192.168.4.1") && strcmp(host,"192.168.4.1:80"))) return false;
    if (httpd_req_get_hdr_value_len(request,"Origin")) {
        char origin[48];
        if (httpd_req_get_hdr_value_str(request,"Origin",origin,sizeof(origin))!=ESP_OK ||
            (strcmp(origin,"http://192.168.4.1") && strcmp(origin,"http://192.168.4.1:80"))) return false;
    }
    return true;
}

static esp_err_t setup_error(httpd_req_t *request,const char *status,const char *message)
{
    httpd_resp_set_status(request,status);
    httpd_resp_set_type(request,"text/plain");
    return httpd_resp_sendstr(request,message);
}

static esp_err_t handle_setup(httpd_req_t *request)
{
    httpd_resp_set_hdr(request,"Cache-Control","no-store");
    httpd_resp_set_hdr(request,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(request,"Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
    if (!setup_client(request)) return setup_error(request,"403 Forbidden","Open WI-FI SETUP on the Chromatic and join its displayed network.\n");
    if (request->method==HTTP_GET && !strcmp(request->uri,"/wifi/status")) {
        const unsigned state=atomic_load(&connection_state);
        const char *name=state==CONNECTED ? "connected" : state==HOTSPOT ? "hotspot" :
                         state==FAILED || state==SAVE_FAILED ? "failed" : "connecting";
        char ip[16];
        portENTER_CRITICAL(&state_lock);
        snprintf(ip,sizeof(ip),"%s",device_ip);
        portEXIT_CRITICAL(&state_lock);
        snprintf(response,sizeof(response),"{\"state\":\"%s\",\"ip\":\"%s\"}",name,ip);
        httpd_resp_set_type(request,"application/json");
        return httpd_resp_sendstr(request,response);
    }
    char nonce[33];
    portENTER_CRITICAL(&state_lock);
    memcpy(nonce,setup_csrf,sizeof(nonce));
    portEXIT_CRITICAL(&state_lock);
    if (request->method==HTTP_GET) {
        const wifi_profile_t current=current_profile();
        if (!wifi_portal_render(response,sizeof(response),current.ssid,nonce,
                                current.mode==WIFI_PROFILE_HOTSPOT))
            return setup_error(request,"500 Internal Server Error","Setup page unavailable.\n");
        httpd_resp_set_type(request,"text/html; charset=utf-8");
        return httpd_resp_sendstr(request,response);
    }
    if (!request->content_len || request->content_len>WIFI_FORM_MAX_BYTES)
        return setup_error(request,"400 Bad Request","Invalid form size.\n");
    char type[96];
    if (httpd_req_get_hdr_value_str(request,"Content-Type",type,sizeof(type))!=ESP_OK ||
        (strcmp(type,"application/x-www-form-urlencoded") &&
         strcmp(type,"application/x-www-form-urlencoded;charset=UTF-8")))
        return setup_error(request,"415 Unsupported Media Type","Use the Wi-Fi setup form.\n");
    char body[WIFI_FORM_MAX_BYTES]; size_t received=0;
    while (received<request->content_len) {
        const int count=httpd_req_recv(request,body+received,request->content_len-received);
        if (count<=0) {
            httpd_resp_set_status(request,"408 Request Timeout");
            (void)httpd_resp_sendstr(request,"Incomplete form. Nothing saved.\n");
            return ESP_FAIL;
        }
        received+=(size_t)count;
    }
    wifi_profile_t next;
    if (!wifi_form_parse(body,received,nonce,&next))
        return setup_error(request,"400 Bad Request","Check the network name and password, or reload this page.\n");
    if (wifi_store_save(&next)!=ESP_OK) {
        atomic_store(&connection_state,SAVE_FAILED);
        return setup_error(request,"500 Internal Server Error","Saving could not be confirmed. Current connection unchanged; please try again.\n");
    }
    if (esp_event_post(A2_WIFI_EVENT,PROFILE_APPLY,&next,sizeof(next),0)!=ESP_OK)
        return setup_error(request,"503 Service Unavailable","Settings saved. Restart the Chromatic to apply.\n");
    char accept[80];
    if (httpd_req_get_hdr_value_str(request,"Accept",accept,sizeof(accept))==ESP_OK &&
        strstr(accept,"application/json")) {
        httpd_resp_set_type(request,"application/json");
        return httpd_resp_sendstr(request,next.mode==WIFI_PROFILE_HOTSPOT ?
            "{\"ok\":true,\"mode\":\"hotspot\"}" : "{\"ok\":true,\"mode\":\"station\"}");
    }
    httpd_resp_set_type(request,"text/html; charset=utf-8");
    return httpd_resp_sendstr(request,wifi_portal_saved());
}

static void start_server(void)
{
    if (server_started) return;
    httpd_config_t http_config=HTTPD_DEFAULT_CONFIG();
    http_config.stack_size=8192;
    http_config.max_uri_handlers=8;
    http_config.max_open_sockets=3;
    http_config.recv_wait_timeout=30;
    http_config.send_wait_timeout=30;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server,&http_config));
    const httpd_uri_t routes[]={
        {.uri="/status",.method=HTTP_GET,.handler=handle},
        {.uri="/text",.method=HTTP_GET,.handler=handle},
        {.uri="/command",.method=HTTP_POST,.handler=handle},
        {.uri="/disk",.method=HTTP_POST,.handler=handle_disk},
        {.uri="/",.method=HTTP_GET,.handler=handle_setup},
        {.uri="/wifi",.method=HTTP_GET,.handler=handle_setup},
        {.uri="/wifi/status",.method=HTTP_GET,.handler=handle_setup},
        {.uri="/wifi/save",.method=HTTP_POST,.handler=handle_setup},
    };
    for (size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
    server_started=true;
}

static void refresh_address(void)
{
    const bool active=radio_started && (atomic_load(&enabled) || atomic_load(&setup_active));
    const bool station=active && atomic_load(&station_connected),ap=active && atomic_load(&ap_started);
    char address[16]="0.0.0.0";
    if (station) snprintf(address,sizeof(address),"%s",station_ip);
    else if (ap) strcpy(address,"192.168.4.1");
    portENTER_CRITICAL(&state_lock);
    strcpy(device_ip,address);
    portEXIT_CRITICAL(&state_lock);
    if (station || ap) start_server();
    atomic_store(&ready,station || ap);
}

static esp_err_t configure_ap(bool setup)
{
    wifi_config_t config={0};
    const wifi_profile_t current=current_profile();
    const char *ssid=setup ? setup_ssid : current.ssid;
    const char *password=setup ? setup_password : current.password;
    memcpy(config.ap.ssid,ssid,strlen(ssid)); config.ap.ssid_len=strlen(ssid);
    memcpy(config.ap.password,password,strlen(password));
    config.ap.channel=1; config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection=1; config.ap.beacon_interval=100;
    return esp_wifi_set_config(WIFI_IF_AP,&config);
}

static esp_err_t configure_station(void)
{
    wifi_config_t config={0};
    const wifi_profile_t current=current_profile();
    memcpy(config.sta.ssid,current.ssid,strlen(current.ssid));
    memcpy(config.sta.password,current.password,strlen(current.password));
    return esp_wifi_set_config(WIFI_IF_STA,&config);
}

static void start_radio(void)
{
    if (radio_started) return;
    radio_started=true;
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!setup_key_retained) {
        /* Generate retained credentials only while RF supplies hardware entropy.
         * A device booted with Wi-Fi off defers this until it is enabled. */
        char candidate[WIFI_SETUP_PASSWORD_BYTES], saved[WIFI_SETUP_PASSWORD_BYTES];
        random_setup_password(candidate);
        if (wifi_store_setup_key(saved,candidate)==ESP_OK) {
            memcpy(candidate,saved,sizeof(candidate));
            setup_key_retained=true;
        } else puts("CHROMATIC_WIFI setup key could not be retained; this boot uses a temporary key");
        portENTER_CRITICAL(&state_lock);
        memcpy(setup_password,candidate,sizeof(setup_password));
        portEXIT_CRITICAL(&state_lock);
    }
}

static void stop_radio(void)
{
    if (radio_started) {
        radio_started=false;
        ESP_ERROR_CHECK(esp_wifi_stop());
    }
    atomic_store(&station_connected,false);
    atomic_store(&ap_started,false);
    atomic_store(&connection_state,OFF);
    refresh_address();
}

static bool save_enabled(bool value)
{
    if (wifi_store_save_enabled(value)!=ESP_OK) {
        atomic_store(&connection_state,SAVE_FAILED);
        return false;
    }
    atomic_store(&enabled,value);
    return true;
}

static void open_setup(void)
{
    if (atomic_load(&setup_active)) return;
    /* Choosing setup explicitly enables Wi-Fi; closing it restores the saved
     * network, while the separate power control can turn the radio off again. */
    if (!atomic_load(&enabled) && !save_enabled(true)) return;
    const wifi_profile_t current=current_profile();
    if (!radio_started && current.mode==WIFI_PROFILE_STATION) {
        connecting_since=esp_timer_get_time();
        atomic_store(&connection_state,CONNECTING);
    }
    const bool try_station=current.mode==WIFI_PROFILE_STATION &&
                           atomic_load(&connection_state)!=FAILED;
    ESP_ERROR_CHECK(esp_wifi_set_mode(try_station ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(configure_ap(true));
    start_radio();
    ESP_ERROR_CHECK(configure_ap(true));
    char nonce[33]; random_hex(nonce,32);
    portENTER_CRITICAL(&state_lock);
    memcpy(setup_csrf,nonce,sizeof(nonce));
    portEXIT_CRITICAL(&state_lock);
    atomic_store(&setup_active,true);
    refresh_address();
}

static void close_setup(void)
{
    atomic_store(&setup_active,false);
    portENTER_CRITICAL(&state_lock);
    memset(setup_csrf,0,sizeof(setup_csrf));
    portEXIT_CRITICAL(&state_lock);
    if (!atomic_load(&enabled)) { stop_radio(); return; }
    const wifi_profile_t current=current_profile();
    if (current.mode==WIFI_PROFILE_HOTSPOT) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(configure_ap(false));
        atomic_store(&connection_state,HOTSPOT);
    } else {
        connecting_since=esp_timer_get_time();
        if (!atomic_load(&station_connected)) atomic_store(&connection_state,CONNECTING);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(configure_station());
        if (radio_started && !atomic_load(&station_connected)) (void)esp_wifi_connect();
    }
    start_radio();
    refresh_address();
}

static void wifi_event(void *unused,esp_event_base_t base,int32_t id,void *data)
{
    (void)unused;
    if (base==A2_WIFI_EVENT) {
        if (id==POWER_SET) {
            const bool value=*(const bool *)data;
            if (save_enabled(value)) close_setup();
        }
        else if (id==SETUP_OPEN) open_setup();
        else if (id==SETUP_CLOSE) close_setup();
        else if (id==PROFILE_APPLY) {
            const wifi_profile_t next=*(const wifi_profile_t *)data;
            (void)esp_wifi_disconnect();
            atomic_store(&station_connected,false);
            portENTER_CRITICAL(&state_lock);
            profile=next;
            portEXIT_CRITICAL(&state_lock);
            connecting_since=esp_timer_get_time();
            atomic_store(&connection_state,next.mode==WIFI_PROFILE_HOTSPOT ? HOTSPOT : CONNECTING);
            if (next.mode==WIFI_PROFILE_STATION) {
                ESP_ERROR_CHECK(esp_wifi_set_mode(atomic_load(&setup_active) ? WIFI_MODE_APSTA : WIFI_MODE_STA));
                ESP_ERROR_CHECK(configure_station());
                (void)esp_wifi_connect();
            } else if (atomic_load(&setup_active)) {
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
            } else close_setup();
            refresh_address();
        } else if (id==NETWORK_TICK && atomic_load(&enabled) && radio_started && profile.mode==WIFI_PROFILE_STATION &&
                   !atomic_load(&station_connected) && atomic_load(&connection_state)!=FAILED &&
                   esp_timer_get_time()-connecting_since>=25000000) {
            /* Repeated station scans share the AP radio and can prevent a new
             * client from joining recovery. Keep setup on a stable AP until the
             * user saves station details or explicitly closes setup to retry. */
            atomic_store(&connection_state,FAILED);
            (void)esp_wifi_disconnect();
            open_setup();
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        }
        return;
    }
    if (!radio_started || !atomic_load(&enabled)) return;
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_START) {
        if (profile.mode==WIFI_PROFILE_STATION && atomic_load(&connection_state)!=FAILED)
            (void)esp_wifi_connect();
    } else if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        if (atomic_exchange(&station_connected,false)) connecting_since=esp_timer_get_time();
        if (profile.mode==WIFI_PROFILE_STATION && atomic_load(&connection_state)!=FAILED) {
            atomic_store(&connection_state,CONNECTING);
            (void)esp_wifi_connect();
        }
        refresh_address();
    } else if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        if (profile.mode!=WIFI_PROFILE_STATION || atomic_load(&connection_state)==FAILED) return;
        const ip_event_got_ip_t *event=data;
        snprintf(station_ip,sizeof(station_ip),IPSTR,IP2STR(&event->ip_info.ip));
        atomic_store(&station_connected,true);
        atomic_store(&connection_state,CONNECTED);
        refresh_address();
    } else if (base==WIFI_EVENT && (id==WIFI_EVENT_AP_START || id==WIFI_EVENT_AP_STOP)) {
        atomic_store(&ap_started,id==WIFI_EVENT_AP_START);
        refresh_address();
    }
}

static void network_tick(void *unused)
{
    (void)unused;
    (void)esp_event_post(A2_WIFI_EVENT,NETWORK_TICK,NULL,0,0);
}

void network_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station_netif=esp_netif_create_default_wifi_sta();
    ap_netif=esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(station_netif && ap_netif ? ESP_OK : ESP_ERR_NO_MEM);
    profile=(wifi_profile_t){.mode=DEVICE_WIFI_AP ? WIFI_PROFILE_HOTSPOT : WIFI_PROFILE_STATION};
    snprintf(profile.ssid,sizeof(profile.ssid),"%s",DEVICE_WIFI_SSID);
    snprintf(profile.password,sizeof(profile.password),"%s",DEVICE_WIFI_PASSWORD);
    wifi_profile_t saved;
    const esp_err_t loaded=wifi_store_load(&saved);
    if (loaded==ESP_OK) profile=saved;
    else if (loaded!=ESP_ERR_NVS_NOT_FOUND) puts("CHROMATIC_WIFI stored profile unavailable; build defaults retained");
    bool saved_enabled=true;
    const esp_err_t power_loaded=wifi_store_load_enabled(&saved_enabled);
    if (power_loaded!=ESP_OK && power_loaded!=ESP_ERR_NVS_NOT_FOUND)
        puts("CHROMATIC_WIFI stored power preference unavailable; Wi-Fi remains on");
    atomic_store(&enabled,saved_enabled);
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(A2_WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    atomic_store(&connection_state,profile.mode==WIFI_PROFILE_HOTSPOT ? HOTSPOT : CONNECTING);
    connecting_since=esp_timer_get_time();
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,mac));
    snprintf(setup_ssid,sizeof(setup_ssid),"CHROMATIC-%02X%02X%02X",mac[3],mac[4],mac[5]);
    random_setup_password(setup_password);
    if (profile.mode==WIFI_PROFILE_HOTSPOT) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(configure_ap(false));
    } else {
        /* Preconfigure a protected AP while radio is stopped, so enabling setup
         * later never briefly advertises the SDK's default open network. */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(configure_ap(true));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(configure_station());
    }
    if (saved_enabled) start_radio();
    else atomic_store(&connection_state,OFF);
    const esp_timer_create_args_t timer_args={.callback=network_tick,.name="wifi_recovery"};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args,&timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer,1000000));
    atomic_store(&initialized,true);
}

#else
void network_start(void) { puts("CHROMATIC_APPLE2_WIFI disabled (no private build config)"); }
bool network_set_enabled(bool value) { (void)value; return false; }
bool network_setup_start(void) { return false; }
void network_setup_stop(void) {}
void network_setup_info(network_setup_info_t *out)
{
    memset(out,0,sizeof(*out));
    strcpy(out->status,"WI-FI IS NOT AVAILABLE");
    strcpy(out->state,"WI-FI IS NOT AVAILABLE");
}
#endif
