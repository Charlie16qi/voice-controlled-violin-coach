#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* 初始化WakeNet模型和检测任务。需要partitions.csv中的model分区，
 * 并在menuconfig中选择WakeNet9“你好小智”模型。 */
esp_err_t wake_word_init(void);

/* 音频采集任务持续喂入16 kHz、16 bit、单声道PCM。 */
void wake_word_feed(const int16_t *samples, size_t sample_count);

/* 等待本地唤醒词被检测到。 */
esp_err_t wake_word_wait(TickType_t timeout);

/*
 * V10.3调试入口：由网页/串口手动触发一次语音采集，
 * 用来区分“WakeNet没唤醒”与“云端ASR没识别”。
 * 正常脱机使用仍然说“你好小智”唤醒。
 */
esp_err_t wake_word_trigger(void);

/* 云端录音/识别期间关闭WakeNet，完成后重新打开。 */
void wake_word_set_enabled(bool enabled);

const char *wake_word_model_name(void);
