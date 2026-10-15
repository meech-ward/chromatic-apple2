/* SPDX-License-Identifier: MIT. Test-only ESP-IDF I/O declarations. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 257
#define ESP_ERR_INVALID_ARG 258
#define ESP_ERR_INVALID_STATE 259
#define ESP_ERR_NOT_FOUND 261
#define ESP_ERR_NOT_SUPPORTED 262
#define ESP_ERR_TIMEOUT 263
#define ESP_ERR_NVS_NOT_FOUND 4354
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
typedef const char *esp_event_base_t;
extern const char WIFI_EVENT[];
extern const char IP_EVENT[];
#define ESP_EVENT_DEFINE_BASE(name) const char name[] = #name
#define ESP_EVENT_DECLARE_BASE(name) extern const char name[];
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define IP_EVENT_STA_GOT_IP 3
#define WIFI_EVENT_AP_START 4
#define WIFI_EVENT_AP_STOP 5
#define WIFI_EVENT_AP_STACONNECTED 6
#define WIFI_STORAGE_RAM 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
#define WIFI_MODE_APSTA 3
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_AUTH_OPEN 0
typedef struct { uint8_t bytes[4]; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { int mode; } esp_netif_t;
typedef int wifi_mode_t;
typedef int wifi_interface_t;
typedef struct { esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (p)->bytes[0], (p)->bytes[1], (p)->bytes[2], (p)->bytes[3]
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef union {
    struct { uint8_t ssid[32], password[64]; } sta;
    struct { uint8_t ssid[32], password[64]; uint8_t ssid_len, channel;
             int authmode; uint8_t max_connection; uint16_t beacon_interval; } ap;
} wifi_config_t;
typedef void (*event_handler_t)(void *, esp_event_base_t, int32_t, void *);
esp_err_t nvs_flash_init(void);
esp_err_t esp_netif_init(void);
esp_err_t esp_event_loop_create_default(void);
esp_netif_t *esp_netif_create_default_wifi_ap(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *);
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_event_handler_register(esp_event_base_t, int32_t, event_handler_t, void *);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_get_mode(wifi_mode_t *);
esp_err_t esp_wifi_get_mac(wifi_interface_t, uint8_t[6]);
esp_err_t esp_event_post(esp_event_base_t, int32_t, const void *, size_t, uint32_t);
int64_t esp_timer_get_time(void);
uint32_t esp_random(void);
void esp_fill_random(void *, size_t);
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; const char *name; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t, uint64_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_SOCK_ERR_TIMEOUT -2
typedef struct {
    int method;
    const char *uri;
    size_t content_len;
    const char *token, *host, *origin, *content_type, *accept;
    const char *body;
    size_t received;
    bool ap_socket;
} httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { unsigned stack_size, max_uri_handlers, max_open_sockets;
                 int recv_wait_timeout, send_wait_timeout; } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
int httpd_req_recv(httpd_req_t *, char *, size_t);
int httpd_req_to_sockfd(httpd_req_t *);
size_t httpd_req_get_url_query_len(httpd_req_t *);
esp_err_t httpd_req_get_url_query_str(httpd_req_t *, char *, size_t);
esp_err_t httpd_start(httpd_handle_t *, const httpd_config_t *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
