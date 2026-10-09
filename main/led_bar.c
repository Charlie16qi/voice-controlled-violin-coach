#include "led_bar.h"
#include <math.h>
#include "app_config.h"
#include "driver/gpio.h"

static const gpio_num_t LEDS[] = {
    APP_LED_L3_GPIO, APP_LED_L2_GPIO, APP_LED_L1_GPIO, APP_LED_OK_GPIO,
    APP_LED_R1_GPIO, APP_LED_R2_GPIO, APP_LED_R3_GPIO,
};

esp_err_t led_bar_init(void)
{
    uint64_t mask = 0;
    for (size_t index = 0; index < sizeof(LEDS) / sizeof(LEDS[0]); ++index) {
        mask |= 1ULL << LEDS[index];
    }
    gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t error = gpio_config(&config);
    led_bar_off();
    return error;
}

void led_bar_off(void)
{
    for (size_t index = 0; index < sizeof(LEDS) / sizeof(LEDS[0]); ++index) {
        gpio_set_level(LEDS[index], 0);
    }
}

void led_bar_show_cents(float cents, bool valid)
{
    led_bar_off();
    if (!valid || !isfinite(cents)) return;
    int selected;
    /* 偏高向琴头左侧；偏低向琴桥右侧。 */
    if (cents > 30.0f) selected = 0;
    else if (cents > 15.0f) selected = 1;
    else if (cents > APP_CORRECT_CENTS) selected = 2;
    else if (cents >= -APP_CORRECT_CENTS) selected = 3;
    else if (cents >= -15.0f) selected = 4;
    else if (cents >= -30.0f) selected = 5;
    else selected = 6;
    gpio_set_level(LEDS[selected], 1);
}
