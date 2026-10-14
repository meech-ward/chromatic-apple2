#pragma once
#include "esp_err.h"
typedef struct { int unused; } wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
