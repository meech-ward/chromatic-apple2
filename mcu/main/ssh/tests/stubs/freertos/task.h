#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
void vTaskDelay(unsigned);
void vTaskDelete(TaskHandle_t);
int xTaskCreatePinnedToCore(void (*)(void *),const char *,unsigned,void *,unsigned,TaskHandle_t *,int);
