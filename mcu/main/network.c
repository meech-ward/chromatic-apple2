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

static char device_ip[16] = "0.0.0.0";
const char *network_ip(void) { return device_ip; }
static atomic_bool ready;
bool network_ready(void) { return atomic_load(&ready); }

#ifndef DEVICE_WIFI_AP
#define DEVICE_WIFI_AP 0
#endif

#if DEVICE_WIFI_ENABLED
static char response[4096];
static esp_netif_t *wifi_netif;
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

static void start_server(void)
{
    if (server_started) return;
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size = 8192;
    http_config.max_uri_handlers = 4;
    http_config.max_open_sockets = 3;
    /* These bound socket inactivity, not the duration of storage calls. Initial
     * flash allocation/programming can legitimately keep an import busy longer.
     */
    http_config.recv_wait_timeout = 30;
    http_config.send_wait_timeout = 30;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &http_config));
    const httpd_uri_t routes[] = {
        {.uri = "/status", .method = HTTP_GET, .handler = handle},
        {.uri = "/text", .method = HTTP_GET, .handler = handle},
        {.uri = "/command", .method = HTTP_POST, .handler = handle},
        {.uri = "/disk", .method = HTTP_POST, .handler = handle_disk},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    server_started = true;
}

static void publish_address(const esp_netif_ip_info_t *info)
{
    snprintf(device_ip, sizeof(device_ip), IPSTR, IP2STR(&info->ip));
    start_server();
    /* SSH may generate its host key only after Wi-Fi has started. AP_START is
     * the equivalent of station address acquisition for hotspot mode.
     */
    atomic_store(&ready, true);
    printf("CHROMATIC_APPLE2_IP %s\n", device_ip);
}

static void wifi_event(void *unused, esp_event_base_t base, int32_t id, void *data)
{
    (void)unused;
#if DEVICE_WIFI_AP
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        esp_netif_ip_info_t info;
        ESP_ERROR_CHECK(esp_netif_get_ip_info(wifi_netif, &info));
        publish_address(&info);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        atomic_store(&ready, false);
        strcpy(device_ip, "0.0.0.0");
    }
#else
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        atomic_store(&ready, false);
        strcpy(device_ip, "0.0.0.0");
        (void)esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        publish_address(&event->ip_info);
    }
#endif
}

void network_start(void)
{
    // Keep Wi-Fi config in RAM; do not erase or replace the console's existing NVS.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if DEVICE_WIFI_AP
    wifi_netif = esp_netif_create_default_wifi_ap();
#else
    wifi_netif = esp_netif_create_default_wifi_sta();
#endif
    ESP_ERROR_CHECK(wifi_netif ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
#if !DEVICE_WIFI_AP
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
#endif
    wifi_config_t config = {0};
#if DEVICE_WIFI_AP
    memcpy(config.ap.ssid, DEVICE_WIFI_SSID, strlen(DEVICE_WIFI_SSID));
    config.ap.ssid_len = strlen(DEVICE_WIFI_SSID);
    memcpy(config.ap.password, DEVICE_WIFI_PASSWORD, strlen(DEVICE_WIFI_PASSWORD));
    config.ap.channel = 1;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    config.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
#else
    memcpy(config.sta.ssid, DEVICE_WIFI_SSID, strlen(DEVICE_WIFI_SSID));
    memcpy(config.sta.password, DEVICE_WIFI_PASSWORD, strlen(DEVICE_WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
}

#else
void network_start(void) { puts("CHROMATIC_APPLE2_WIFI disabled (no private build config)"); }
#endif
