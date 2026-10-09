#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t display_st7789_init(void);
void display_show_home(bool wifi_connected);
void display_show_practice(
    const char *target_note,
    float target_hz,
    bool valid,
    float measured_hz,
    float cents,
    float confidence,
    const char *progress
);
void display_show_message(const char *title, const char *line1, const char *line2);
