#include "wifi_time.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_secrets.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI";
static EventGroupHandle_t s_events;
static const EventBits_t CONNECTED_BIT = BIT0;
static volatile int32_t s_last_disconnect_reason;
static bool s_sntp_started;

static void event_handler(
    void *argument,
    esp_event_base_t base,
    int32_t id,
    void *data
)
{
    (void)argument;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "station started; connecting...");
        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(error));
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated with access point; waiting for DHCP address");
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)data;

        s_last_disconnect_reason = event ? event->reason : -1;
        xEventGroupClearBits(s_events, CONNECTED_BIT);

        ESP_LOGW(
            TAG,
            "disconnected; reason=%ld, reconnecting",
            (long)s_last_disconnect_reason
        );

        esp_err_t error = esp_wifi_connect();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "reconnect failed: %s", esp_err_to_name(error));
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_last_disconnect_reason = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);

        if (event) {
            ESP_LOGI(
                TAG,
                "GOT IP: " IPSTR ", gateway: " IPSTR,
                IP2STR(&event->ip_info.ip),
                IP2STR(&event->ip_info.gw)
            );
        } else {
            ESP_LOGI(TAG, "GOT IP");
        }
    }
}

esp_err_t wifi_time_init(void)
{
    esp_err_t error = nvs_flash_init();
    if (
        error == ESP_ERR_NVS_NO_FREE_PAGES
        || error == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    s_events = xEventGroupCreate();
    if (!s_events) {
        ESP_LOGE(TAG, "event group allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(error);
    }

    esp_netif_t *station = esp_netif_create_default_wifi_sta();
    if (!station) {
        ESP_LOGE(TAG, "failed to create station network interface");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &event_handler,
            NULL
        )
    );
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {0};
    strlcpy(
        (char *)wifi_config.sta.ssid,
        APP_WIFI_SSID,
        sizeof(wifi_config.sta.ssid)
    );
    strlcpy(
        (char *)wifi_config.sta.password,
        APP_WIFI_PASSWORD,
        sizeof(wifi_config.sta.password)
    );

    /*
     * 使用OPEN作为“最低安全级别”，允许连接OPEN/WPA2/WPA3等热点。
     * 密码仍会按APP_WIFI_PASSWORD参与加密网络认证。
     */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    /*
     * 关闭省电可减少某些手机热点下的连接/UDP时间同步不稳定。
     * 功耗会略有增加，但更适合当前面包板联调。
     */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(
        TAG,
        "connecting to SSID: %s (password length=%u)",
        APP_WIFI_SSID,
        (unsigned)strlen(APP_WIFI_PASSWORD)
    );
    return ESP_OK;
}

bool wifi_time_is_connected(void)
{
    return s_events
        && (xEventGroupGetBits(s_events) & CONNECTED_BIT);
}

esp_err_t wifi_time_wait_connected(TickType_t timeout)
{
    if (!s_events) {
        ESP_LOGE(TAG, "Wi-Fi event group is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_events,
        CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        timeout
    );

    if (bits & CONNECTED_BIT) {
        return ESP_OK;
    }

    ESP_LOGE(
        TAG,
        "Wi-Fi connection timeout; last disconnect reason=%ld",
        (long)s_last_disconnect_reason
    );
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_time_sync_clock(TickType_t timeout)
{
    time_t now;
    struct tm timeinfo = {0};

    time(&now);
    gmtime_r(&now, &timeinfo);
    if (timeinfo.tm_year + 1900 >= 2024) {
        ESP_LOGI(
            TAG,
            "clock already valid: %04d-%02d-%02d %02d:%02d:%02d UTC",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        );
        return ESP_OK;
    }

    if (!wifi_time_is_connected()) {
        ESP_LOGE(TAG, "cannot synchronize time: Wi-Fi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_sntp_started) {
        ESP_LOGI(TAG, "starting SNTP with ntp.aliyun.com");
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
        s_sntp_started = true;
    } else {
        ESP_LOGI(TAG, "SNTP already running; waiting for valid UTC time");
    }

    TickType_t start = xTaskGetTickCount();

    while (xTaskGetTickCount() - start < timeout) {
        time(&now);
        gmtime_r(&now, &timeinfo);

        if (timeinfo.tm_year + 1900 >= 2024) {
            setenv("TZ", "UTC0", 1);
            tzset();

            ESP_LOGI(
                TAG,
                "TIME SYNC OK: %04d-%02d-%02d %02d:%02d:%02d UTC",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec
            );
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGE(
        TAG,
        "time synchronization timeout after %lu ms",
        (unsigned long)(timeout * portTICK_PERIOD_MS)
    );
    return ESP_ERR_TIMEOUT;
}

int32_t wifi_time_last_disconnect_reason(void)
{
    return s_last_disconnect_reason;
}
