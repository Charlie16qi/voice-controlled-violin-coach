#pragma once

#include "driver/gpio.h"
#include "note_utils.h"

/* ===================== 已经验证过的原硬件 ===================== */
#define APP_LCD_SCLK_GPIO       GPIO_NUM_4
#define APP_LCD_MOSI_GPIO       GPIO_NUM_5
#define APP_LCD_CS_GPIO         GPIO_NUM_7
#define APP_LCD_DC_GPIO         GPIO_NUM_9
#define APP_LCD_RST_GPIO        GPIO_NUM_10
#define APP_LCD_H_RES           240
#define APP_LCD_V_RES           240
/* 1.54英寸240x240 ST7789常见为80；画面整体错位时改成0再试。 */
#define APP_LCD_X_GAP           0
#define APP_LCD_Y_GAP           0
#define APP_LCD_INVERT_COLOR    1
#define APP_LCD_MIRROR_X        0
#define APP_LCD_MIRROR_Y        0
#define APP_LCD_SWAP_XY         0

/* 琴头 <- L3 L2 L1 OK R1 R2 R3 -> 琴桥 */
#define APP_LED_L3_GPIO         GPIO_NUM_11
#define APP_LED_L2_GPIO         GPIO_NUM_12
#define APP_LED_L1_GPIO         GPIO_NUM_13
#define APP_LED_OK_GPIO         GPIO_NUM_14
#define APP_LED_R1_GPIO         GPIO_NUM_15
#define APP_LED_R2_GPIO         GPIO_NUM_16
#define APP_LED_R3_GPIO         GPIO_NUM_21

/* ===================== 新增INMP441 =====================
 * 摄像头排线必须保持拔下：GPIO6/17/18在本板上同时连接摄像头信号。
 */
#define APP_I2S_BCLK_GPIO       GPIO_NUM_6
#define APP_I2S_WS_GPIO         GPIO_NUM_17
#define APP_I2S_DIN_GPIO        GPIO_NUM_18
/* INMP441 L/R接GND，所以读取LEFT声道。 */
#define APP_MIC_LEFT_CHANNEL    1
#define APP_MIC_SAMPLE_RATE     16000
#define APP_MIC_BLOCK_SAMPLES   320
/* 24位数据位于32位容器高位。14相当于在16位输出上约4倍数字增益。 */
#define APP_MIC_RIGHT_SHIFT     14
#define APP_MIC_DIGITAL_GAIN    1.0f
#define APP_AUDIO_HPF_HZ         75.0f
#define APP_AUDIO_LPF_HZ         7200.0f

/* ===================== 免按键语音唤醒 =====================
 * WakeNet模型不再在代码中写死。程序会读取menuconfig中勾选并烧录到
 * model分区的实际WakeNet模型。ESP32-S3请选择WakeNet9的“你好小智”。
 */
#define APP_WAKE_STREAM_SAMPLES 4096
/* 云端命令不再固定录4秒：最多3秒，检测到说完后提前结束。 */
#define APP_VOICE_MAX_SECONDS       3
#define APP_VOICE_START_TIMEOUT_MS  3000
#define APP_VOICE_END_SILENCE_MS    380
#define APP_VOICE_MIN_SPEECH_MS     140
#define APP_VOICE_PRE_ROLL_MS       220
#define APP_VOICE_POST_ROLL_MS      140
#define APP_VOICE_VAD_MIN_RMS       0.0040f
#define APP_VOICE_VAD_NOISE_MULT    1.85f
#define APP_VOICE_VAD_RELEASE_RATIO 0.72f
#define APP_VOICE_VAD_START_BLOCKS  2
#define APP_VOICE_TARGET_RMS        0.105f
#define APP_VOICE_MAX_GAIN          5.0f

/* ===================== HPS ===================== */
#define APP_FFT_SIZE            4096
#define APP_HOP_SIZE            1024
#define APP_HPS_MIN_RMS         0.0025f
#define APP_HPS_NOISE_MULT      2.5f
#define APP_HPS_SEARCH_LOW      0.70f
#define APP_HPS_SEARCH_HIGH     1.35f
#define APP_HPS_MIN_CONFIDENCE  0.12f
/*
 * V3.2 八度保护：
 * 当前候选必须明显优于 f/2 与 2f 两个八度假设，
 * 否则宁可判无效，也不能把错八度亮成“准”。
 */
#define APP_HPS_OCTAVE_GUARD_DB       1.8f
#define APP_HPS_OCTAVE_MIN_TEST_HZ    80.0f
#define APP_HPS_OCTAVE_MAX_TEST_HZ    7600.0f
#define APP_HPS_LOG_OCTAVE_REJECT     1

/*
 * V3.3 混合基频确认器（HPS + 轻量YIN）
 *
 * HPS负责利用小提琴谐波结构找候选；
 * YIN负责确认“真正的最短周期”，专门解决E4/E5、A4/A5等八度混淆。
 */
#define APP_HYBRID_YIN_ENABLED                 1
/*
 * V3.4不再只围绕HPS候选搜索YIN。
 * YIN独立扫描70~1800Hz，负责发现“真正拉出来的音”，
 * 这样目标A5时实际拉A4也能直接得到约440Hz，而不是只知道“拒绝”。
 */
#define APP_HYBRID_YIN_GLOBAL_MIN_HZ            70.0f
#define APP_HYBRID_YIN_GLOBAL_MAX_HZ          1800.0f
#define APP_HYBRID_YIN_FRAME_SAMPLES           2048
#define APP_HYBRID_YIN_MAX_LAG                  320
#define APP_HYBRID_YIN_THRESHOLD               0.22f
#define APP_HYBRID_YIN_MIN_CONFIDENCE          0.52f
#define APP_HYBRID_YIN_AGREEMENT_CENTS        180.0f
/*
 * 当HPS被目标音的谐波“吸住”，但YIN以高置信度发现了明显不同的真实音高，
 * 用YIN结果作为实际检测频率输出，而不是让屏幕什么都不显示。
 */
#define APP_HYBRID_YIN_OVERRIDE_CONFIDENCE      0.68f
#define APP_HYBRID_YIN_OVERRIDE_MIN_CENTS      350.0f
#define APP_HYBRID_LOG_REJECT                     1

/*
 * 临时坏帧不马上清空显示。
 * 4帧 × 64ms ≈ 256ms，可明显减少“一闪就没”的感觉。
 */
#define APP_PITCH_INVALID_GRACE_FRAMES             4
#define APP_HPS_STABLE_FRAMES   3
#define APP_NOTE_MIN_MIDI        NOTE_MIN_MIDI
#define APP_NOTE_MAX_MIDI        NOTE_MAX_MIDI
#define APP_MAX_SONG_NOTES       256
#define APP_CORRECT_CENTS       5.0f
#define APP_NEAR_CENTS          15.0f
#define APP_MELODY_ACCEPT_CENTS 30.0f
/* Beginner pitch practice: allow small intonation errors without accepting
 * an adjacent semitone. Rhythm/full and single-note tuning retain their rules. */
#define APP_MELODY_FLUID_MIN_CENTS 40.0f
#define APP_MELODY_HOLD_MS      350
#define APP_MELODY_RELEASE_MS   100
/*
 * V3.5 乐曲逐音推进参数。
 * 逐音训练以“音高命中”为主，不要求保持完整节拍长度。
 */
#define APP_MELODY_MAX_HOLD_MS          650
/* Melody-state UX only; HPS/YIN pitch detection is unchanged. */
/*
 * V10.3 FLUID:
 * HPS/YIN一旦给出有效结果，就从第一帧开始判定，不等待显示端3帧中值。
 * ±5c：1个64ms分析步即可推进；
 * ±25c：1步（约64ms）；
 * 曲目容差内（音准模式至少±40c）：最多2步（约128ms）。
 * 只改“命中后何时推进”，不修改HPS/YIN算法。
 */
#define APP_MELODY_IN_TUNE_FAST_MS       64
#define APP_MELODY_NEAR_FAST_CENTS      25.0f
#define APP_MELODY_NEAR_FAST_MS          64
#define APP_MELODY_ACCEPT_MAX_MS        128
#define APP_MELODY_PROGRESS_DECAY_MS     16
/* Repeated notes may release by energy drop, raw invalid frame, or clear pitch departure. */
#define APP_MELODY_RELEASE_GATE_MULT    1.90f
/* V10.3: 连续同音的重新起弓，单个明确release分析步即可重新武装。 */
#define APP_MELODY_RELEASE_CONFIRM_MS    64
/* Strong frame-to-frame RMS drop can count as a re-bow/re-articulation for repeated notes. */
#define APP_MELODY_REBOW_DROP_RATIO    0.45f
#define APP_MELODY_REBOW_MIN_GATE_MULT 2.80f

/* ===================== V10.4 节奏引擎 =====================
 * MusicXML/曲库中的 beats 以四分音符=1拍表示。
 * HPS/YIN分析步仍为1024/16000=64ms；节奏时钟不修改音高算法。
 */
#define APP_RHYTHM_FRAME_MS               64
#define APP_RHYTHM_SOUND_GATE_MULT       1.35f

/*
 * V10.4.3 REAL-ONSET FILTER
 *
 * A single HPS/YIN-valid frame is not enough to start a rhythm note.
 * This specifically rejects:
 * - short metronome clicks leaking into INMP441
 * - environmental transients
 * - one-frame pitch false positives
 *
 * Two consecutive 64 ms analysis frames are required.
 * The candidate must also be clearly above the adaptive noise gate and have
 * non-trivial pitch confidence.
 */
#define APP_RHYTHM_ONSET_CONFIRM_MS       128
#define APP_RHYTHM_ONSET_GATE_MULT        1.80f
#define APP_RHYTHM_ONSET_MIN_CONFIDENCE   0.20f

/*
 * V10.5 performer-led note boundary.
 * A note ends when the player actually releases / re-articulates / changes pitch,
 * not when the target duration merely expires.
 */
#define APP_RHYTHM_BOUNDARY_CONFIRM_MS     96
#define APP_RHYTHM_PITCH_EXIT_CENTS        90.0f
/* Safety only: a wildly over-held note eventually ends as WRONG-LONG, never as correct. */
#define APP_RHYTHM_HARD_TIMEOUT_EXTRA_MS   900
#define APP_RHYTHM_HARD_TIMEOUT_RATIO      1.00f

#define APP_RHYTHM_DEFAULT_TOLERANCE_MS 140
#define APP_RHYTHM_MIN_TOLERANCE_MS      60
#define APP_RHYTHM_MAX_TOLERANCE_MS     300

/* 1=上电直接进入A4，方便先验证麦克风/HPS；0=上电停在主页等语音。 */
#define APP_BOOT_STARTS_A4      1
/* 1=串口每秒打印麦克风RMS和峰值，硬件调试完成后可改0。 */
#define APP_MIC_DIAGNOSTIC      1
