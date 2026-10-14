/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "video_render.h"
void video_link_init(void);
esp_err_t video_link_frame(const uint8_t frame[A2_FRAME_BYTES]);
