#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t wifi_time_init(void);
bool wifi_time_is_connected(void);
esp_err_t wifi_time_wait_connected(TickType_t timeout);
esp_err_t wifi_time_sync_clock(TickType_t timeout);

/* 最近一次Wi-Fi断线原因码；0表示尚未记录。 */
int32_t wifi_time_last_disconnect_reason(void);
