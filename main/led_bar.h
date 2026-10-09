#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t led_bar_init(void);
void led_bar_off(void);
void led_bar_show_cents(float cents, bool valid);
