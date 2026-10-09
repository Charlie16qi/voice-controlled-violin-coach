#include "hps_pitch.h"
#include "note_utils.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "HPS";
static float *s_fft_complex;
static float *s_magnitude;
static float *s_window;
static float s_yin_difference[APP_HYBRID_YIN_MAX_LAG + 1];

static float clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

float pitch_cents(float frequency_hz, float target_hz)
{
    return 1200.0f * log2f(frequency_hz / target_hz);
}


esp_err_t hps_pitch_init(void)
{
    s_fft_complex = heap_caps_aligned_alloc(
        16,
        APP_FFT_SIZE * 2 * sizeof(float),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    s_magnitude = heap_caps_aligned_alloc(
        16,
        (APP_FFT_SIZE / 2 + 1) * sizeof(float),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    s_window = heap_caps_aligned_alloc(
        16,
        APP_FFT_SIZE * sizeof(float),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    ESP_RETURN_ON_FALSE(
        s_fft_complex && s_magnitude && s_window,
        ESP_ERR_NO_MEM,
        TAG,
        "FFT buffer allocation failed"
    );
    dsps_wind_hann_f32(s_window, APP_FFT_SIZE);
    ESP_RETURN_ON_ERROR(
        dsps_fft2r_init_fc32(NULL, APP_FFT_SIZE),
        TAG,
        "FFT table init failed"
    );
    return ESP_OK;
}

static float spectrum_log_at(float frequency_hz)
{
    const float bin_hz = (float)APP_MIC_SAMPLE_RATE / APP_FFT_SIZE;
    float location = frequency_hz / bin_hz;
    int left = (int)floorf(location);
    float fraction = location - left;
    if (left < 0 || left + 1 > APP_FFT_SIZE / 2) return logf(1e-12f);
    float value = s_magnitude[left] * (1.0f - fraction) + s_magnitude[left + 1] * fraction;
    return logf(value + 1e-12f);
}

static float weighted_hps_score(float candidate_hz)
{
    static const float weights[] = {1.0f, 0.8f, 0.6f, 0.4f};
    float score = 0.0f;
    float used = 0.0f;
    for (int harmonic = 1; harmonic <= 4; ++harmonic) {
        float harmonic_hz = candidate_hz * harmonic;
        if (harmonic_hz >= APP_MIC_SAMPLE_RATE * 0.5f) break;
        float weight = weights[harmonic - 1];
        /* 三个相邻频率点取最大值，减轻FFT频率栅格和轻微非谐性影响。 */
        float local = spectrum_log_at(harmonic_hz);
        float side = fmaxf(
            spectrum_log_at(harmonic_hz - 1.0f),
            spectrum_log_at(harmonic_hz + 1.0f)
        );
        local = fmaxf(local, side);
        score += weight * local;
        used += weight;
    }
    return used > 0.0f ? score / used : -FLT_MAX;
}


static float score_difference_db(
    float first_score,
    float second_score
)
{
    return (
        first_score - second_score
    ) * (20.0f / logf(10.0f));
}

static bool octave_guard_passes(
    float candidate_hz,
    float candidate_score,
    float *half_advantage_db,
    float *double_advantage_db
)
{
    if (half_advantage_db) {
        *half_advantage_db = -FLT_MAX;
    }
    if (double_advantage_db) {
        *double_advantage_db = -FLT_MAX;
    }

    bool pass = true;

    const float half_hz = candidate_hz * 0.5f;

    if (
        half_hz >= APP_HPS_OCTAVE_MIN_TEST_HZ
        && half_hz <= APP_HPS_OCTAVE_MAX_TEST_HZ
    ) {
        float half_score =
            weighted_hps_score(half_hz);

        float advantage_db =
            score_difference_db(
                half_score,
                candidate_score
            );

        if (half_advantage_db) {
            *half_advantage_db = advantage_db;
        }

        if (
            advantage_db
            > -APP_HPS_OCTAVE_GUARD_DB
        ) {
            pass = false;
        }
    }

    const float double_hz = candidate_hz * 2.0f;

    if (
        double_hz >= APP_HPS_OCTAVE_MIN_TEST_HZ
        && double_hz <= APP_HPS_OCTAVE_MAX_TEST_HZ
        && double_hz < APP_MIC_SAMPLE_RATE * 0.5f
    ) {
        float double_score =
            weighted_hps_score(double_hz);

        float advantage_db =
            score_difference_db(
                double_score,
                candidate_score
            );

        if (double_advantage_db) {
            *double_advantage_db = advantage_db;
        }

        if (
            advantage_db
            > -APP_HPS_OCTAVE_GUARD_DB
        ) {
            pass = false;
        }
    }

    return pass;
}


typedef struct {
    bool available;
    float frequency_hz;
    float confidence;
    float cmndf;
} yin_verify_result_t;

static float yin_parabolic_tau(
    const float *curve,
    int tau,
    int max_tau
)
{
    if (
        !curve
        || tau <= 1
        || tau >= max_tau
    ) {
        return (float)tau;
    }

    float left = curve[tau - 1];
    float center = curve[tau];
    float right = curve[tau + 1];

    float denominator =
        left - 2.0f * center + right;

    if (fabsf(denominator) < 1e-7f) {
        return (float)tau;
    }

    float offset =
        0.5f * (left - right)
        / denominator;

    if (offset < -1.0f) offset = -1.0f;
    if (offset > 1.0f) offset = 1.0f;

    return (float)tau + offset;
}

/*
 * 轻量YIN验证器。
 *
 * 与完整YIN不同，这里不是重新替代HPS，而只在目标附近的
 * 半倍频~两倍频范围内寻找“最早出现的可靠周期”。
 *
 * YIN的CMNDF会优先选择最短真实周期，因此：
 *   实际E5 -> 首个可靠周期约24采样
 *   E4的二倍周期约49采样虽然也可能很像，但不是首个周期
 *
 * 这正好补HPS容易出现的subharmonic/octave错误。
 */
static yin_verify_result_t yin_verify_frequency(
    const float *samples,
    float candidate_hz
)
{
    yin_verify_result_t result = {0};

#if !APP_HYBRID_YIN_ENABLED
    (void)samples;
    (void)candidate_hz;
    return result;
#else
    /*
     * V3.4：candidate_hz仅保留接口兼容，不再限制YIN搜索范围。
     * YIN独立估计真实基频，避免目标约束把错误谐波当成“正确目标”。
     */
    (void)candidate_hz;

    if (!samples) {
        return result;
    }

    float low_hz =
        APP_HYBRID_YIN_GLOBAL_MIN_HZ;

    float high_hz =
        fminf(
            APP_HYBRID_YIN_GLOBAL_MAX_HZ,
            APP_MIC_SAMPLE_RATE * 0.45f
        );

    if (high_hz <= low_hz) {
        return result;
    }

    int min_tau =
        (int)floorf(
            APP_MIC_SAMPLE_RATE / high_hz
        );

    int max_tau =
        (int)ceilf(
            APP_MIC_SAMPLE_RATE / low_hz
        );

    if (min_tau < 2) min_tau = 2;

    if (
        max_tau > APP_HYBRID_YIN_MAX_LAG
    ) {
        max_tau =
            APP_HYBRID_YIN_MAX_LAG;
    }

    if (max_tau <= min_tau + 2) {
        return result;
    }

    int analysis_samples =
        APP_HYBRID_YIN_FRAME_SAMPLES;

    if (analysis_samples > APP_FFT_SIZE) {
        analysis_samples = APP_FFT_SIZE;
    }

    /*
     * 所有tau用相同数量的样本，避免大tau因为累加项变少而
     * 人为得到更小difference。
     */
    int compare_count =
        analysis_samples - max_tau;

    if (compare_count < 512) {
        return result;
    }

    s_yin_difference[0] = 0.0f;

    for (
        int tau = 1;
        tau <= max_tau;
        ++tau
    ) {
        /* Single-precision input/output: avoid double math in this hot loop. */
        float difference = 0.0f;

        for (
            int index = 0;
            index < compare_count;
            ++index
        ) {
            float delta =
                samples[index]
                - samples[index + tau];

            difference +=
                delta * delta;
        }

        s_yin_difference[tau] =
            (float)difference;
    }

    /*
     * Cumulative Mean Normalized Difference Function.
     */
    float cumulative = 0.0f;

    for (
        int tau = 1;
        tau <= max_tau;
        ++tau
    ) {
        cumulative +=
            s_yin_difference[tau];

        if (cumulative > 1e-12f) {
            s_yin_difference[tau] *=
                (float)tau / cumulative;
        } else {
            s_yin_difference[tau] = 1.0f;
        }
    }

    int selected_tau = -1;

    /*
     * YIN关键：选择“第一个”低于阈值的谷值，
     * 而不是整个范围的绝对最小值。
     * 这样可避免把真实周期的2倍、3倍当基频。
     */
    for (
        int tau = min_tau;
        tau < max_tau;
        ++tau
    ) {
        if (
            s_yin_difference[tau]
                < APP_HYBRID_YIN_THRESHOLD
        ) {
            int local_tau = tau;

            while (
                local_tau + 1 <= max_tau
                && s_yin_difference[
                    local_tau + 1
                ]
                    < s_yin_difference[
                        local_tau
                    ]
            ) {
                ++local_tau;
            }

            selected_tau = local_tau;
            break;
        }
    }

    /*
     * 没有跨过严格阈值时，仍记录最好的谷值；
     * 但其confidence可能不足，不会强行否决HPS。
     */
    if (selected_tau < 0) {
        float best = 1e9f;

        for (
            int tau = min_tau;
            tau <= max_tau;
            ++tau
        ) {
            if (
                s_yin_difference[tau] < best
            ) {
                best =
                    s_yin_difference[tau];
                selected_tau = tau;
            }
        }
    }

    if (selected_tau < 0) {
        return result;
    }

    float refined_tau =
        yin_parabolic_tau(
            s_yin_difference,
            selected_tau,
            max_tau
        );

    if (refined_tau <= 1.0f) {
        return result;
    }

    float cmndf =
        s_yin_difference[selected_tau];

    result.available = true;
    result.frequency_hz =
        APP_MIC_SAMPLE_RATE
        / refined_tau;
    result.cmndf = cmndf;
    result.confidence =
        clamp01(1.0f - cmndf);

    return result;
#endif
}

static bool hybrid_octave_validation(
    const float *samples,
    float candidate_hz,
    bool spectral_octave_ok,
    float *yin_hz,
    float *yin_confidence,
    float *agreement_cents
)
{
    if (yin_hz) *yin_hz = 0.0f;
    if (yin_confidence) *yin_confidence = 0.0f;
    if (agreement_cents) *agreement_cents = 0.0f;

    yin_verify_result_t yin =
        yin_verify_frequency(
            samples,
            candidate_hz
        );

    if (!yin.available) {
        /*
         * 超高音或YIN不可用时继续使用V3.2频谱八度保护。
         */
        return spectral_octave_ok;
    }

    if (yin_hz) {
        *yin_hz = yin.frequency_hz;
    }
    if (yin_confidence) {
        *yin_confidence =
            yin.confidence;
    }

    float cents =
        pitch_cents(
            yin.frequency_hz,
            candidate_hz
        );

    if (agreement_cents) {
        *agreement_cents = cents;
    }

    if (
        yin.confidence
        >= APP_HYBRID_YIN_MIN_CONFIDENCE
    ) {
        /*
         * 高置信YIN拥有“否决权”：
         * 如果YIN认为实际周期和HPS候选差得太远，
         * 特别是±1200cent附近，则直接拒绝。
         */
        if (
            fabsf(cents)
                > APP_HYBRID_YIN_AGREEMENT_CENTS
        ) {
            return false;
        }

        /*
         * YIN明确同意当前基频时，可以解除V3.2单纯频谱法的
         * 过度拒绝。小提琴某些音的基频本来就可能弱于谐波。
         */
        return true;
    }

    /*
     * YIN本身不够确定，则不让它乱投票，退回频谱保护。
     */
    return spectral_octave_ok;
}

esp_err_t hps_pitch_analyze(
    const float *samples,
    float target_hz,
    float noise_rms,
    hps_result_t *result
)
{
    if (!samples || !result || target_hz <= 0.0f) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    double sum_square = 0.0;
    float mean = 0.0f;
    for (int index = 0; index < APP_FFT_SIZE; ++index) mean += samples[index];
    mean /= APP_FFT_SIZE;

    for (int index = 0; index < APP_FFT_SIZE; ++index) {
        float value = samples[index] - mean;
        sum_square += value * value;
        s_fft_complex[index * 2] = value * s_window[index];
        s_fft_complex[index * 2 + 1] = 0.0f;
    }
    result->rms = sqrtf((float)(sum_square / APP_FFT_SIZE));
    result->noise_gate = fmaxf(APP_HPS_MIN_RMS, noise_rms * APP_HPS_NOISE_MULT);
    if (result->rms < result->noise_gate) return ESP_OK;

    ESP_RETURN_ON_ERROR(dsps_fft2r_fc32(s_fft_complex, APP_FFT_SIZE), TAG, "FFT failed");
    ESP_RETURN_ON_ERROR(dsps_bit_rev_fc32(s_fft_complex, APP_FFT_SIZE), TAG, "bit reverse failed");
    /* ESP-DSP官方复数FFT流程：把实信号结果整理为前半谱。 */
    ESP_RETURN_ON_ERROR(dsps_cplx2reC_fc32(s_fft_complex, APP_FFT_SIZE), TAG, "real spectrum conversion failed");

    for (int bin = 0; bin < APP_FFT_SIZE / 2; ++bin) {
        float real = s_fft_complex[bin * 2];
        float imag = s_fft_complex[bin * 2 + 1];
        s_magnitude[bin] = sqrtf(real * real + imag * imag);
    }
    s_magnitude[APP_FFT_SIZE / 2] = s_magnitude[APP_FFT_SIZE / 2 - 1];

    const float bin_hz = (float)APP_MIC_SAMPLE_RATE / APP_FFT_SIZE;
    const float step_hz = bin_hz * 0.25f;
    float low = target_hz * APP_HPS_SEARCH_LOW;
    float high = target_hz * APP_HPS_SEARCH_HIGH;

    float best_frequency = 0.0f;
    float best_score = -FLT_MAX;
    float second_score = -FLT_MAX;
    for (float candidate = low; candidate <= high; candidate += step_hz) {
        float score = weighted_hps_score(candidate);
        if (score > best_score) {
            if (best_frequency > 0.0f && fabsf(candidate - best_frequency) > 8.0f) {
                second_score = best_score;
            }
            best_score = score;
            best_frequency = candidate;
        } else if (fabsf(candidate - best_frequency) > 8.0f && score > second_score) {
            second_score = score;
        }
    }

    if (best_frequency <= 0.0f) return ESP_OK;

    /* 抛物线细化：对搜索步长两侧重新计算HPS得分。 */
    float left_score = weighted_hps_score(best_frequency - step_hz);
    float right_score = weighted_hps_score(best_frequency + step_hz);
    float denominator = left_score - 2.0f * best_score + right_score;
    float offset = 0.0f;
    if (fabsf(denominator) > 1e-6f) {
        offset = 0.5f * (left_score - right_score) / denominator;
        if (offset < -1.0f) offset = -1.0f;
        if (offset > 1.0f) offset = 1.0f;
    }
    float refined = best_frequency + offset * step_hz;

    float contrast_db = 0.0f;
    if (second_score > -FLT_MAX / 2.0f) {
        contrast_db = (best_score - second_score) * (20.0f / logf(10.0f));
    }
    float confidence = clamp01((contrast_db - 0.5f) / 8.0f);
    /* RMS足够且目标范围已严格限定时，给基础置信度，避免相邻候选过密导致对比偏低。 */
    confidence = fmaxf(confidence, clamp01((result->rms / result->noise_gate - 1.0f) / 4.0f) * 0.35f);

    /*
     * V3.2 八度歧义保护。
     *
     * 例：目标E4=329.63Hz，但实际拉E5=659.26Hz。
     * 对“E4候选”而言：
     *   2f≈659Hz  会踩中E5基频
     *   4f≈1318Hz 会踩中E5二次谐波
     * 因此普通HPS可能把E5误判成E4。
     *
     * 这里额外比较 f/2、f、2f 三种八度假设。
     */
    float refined_score =
        weighted_hps_score(refined);

    float half_advantage_db =
        -FLT_MAX;
    float double_advantage_db =
        -FLT_MAX;

    bool spectral_octave_ok =
        octave_guard_passes(
            refined,
            refined_score,
            &half_advantage_db,
            &double_advantage_db
        );

    float yin_hz = 0.0f;
    float yin_confidence = 0.0f;
    float yin_agreement_cents = 0.0f;

    bool octave_ok =
        hybrid_octave_validation(
            samples,
            refined,
            spectral_octave_ok,
            &yin_hz,
            &yin_confidence,
            &yin_agreement_cents
        );

    /*
     * V3.4关键变化：
     *
     * HPS是“目标约束”算法，所以当目标A5而实际拉A4时，
     * HPS搜索区间根本不包含440Hz，最多只能被880Hz谐波吸住或拒绝。
     *
     * 全音域调音器不能只说“无效”，它应该尽可能告诉用户：
     * “你实际拉的是约440Hz，比A5低一个八度。”
     *
     * 因此当独立全局YIN以较高置信度发现一个与HPS明显不同的频率时，
     * 直接把YIN频率作为“实际检测频率”输出。
     */
    bool yin_override =
        yin_hz > 0.0f
        && yin_confidence
            >= APP_HYBRID_YIN_OVERRIDE_CONFIDENCE
        && fabsf(yin_agreement_cents)
            >= APP_HYBRID_YIN_OVERRIDE_MIN_CENTS;

    float output_frequency = refined;
    float output_confidence = confidence;
    bool output_valid =
        confidence >= APP_HPS_MIN_CONFIDENCE
        && octave_ok;

    if (yin_override) {
        output_frequency = yin_hz;
        output_confidence =
            fmaxf(confidence, yin_confidence);
        output_valid = true;

        ESP_LOGI(
            TAG,
            "ACTUAL_PITCH target=%.2f hps=%.2f yin=%.2f yin_conf=%.2f target_diff=%.1fc",
            target_hz,
            refined,
            yin_hz,
            yin_confidence,
            pitch_cents(yin_hz, target_hz)
        );
    } else {
#if APP_HYBRID_LOG_REJECT
        if (!octave_ok) {
            ESP_LOGW(
                TAG,
                "HYBRID_REJECT target=%.2f hps=%.2f yin=%.2f yin_conf=%.2f diff=%.1fc spectral=%d half=%.2fdB double=%.2fdB rms=%.5f hps_conf=%.2f",
                target_hz,
                refined,
                yin_hz,
                yin_confidence,
                yin_agreement_cents,
                spectral_octave_ok ? 1 : 0,
                half_advantage_db,
                double_advantage_db,
                result->rms,
                confidence
            );
        }
#endif
    }

    /*
     * Violin Coach PC/AR interface:
     * output the final selected pitch only.
     * This does not change HPS/YIN algorithm.
     */
    if (output_valid && output_frequency > 0.0f) {
        printf(
            "{\"vc\":\"pitch\",\"hz\":%.2f,\"cents\":%.2f,\"confidence\":%.3f}\n",
            output_frequency,
            pitch_cents(output_frequency, target_hz),
            output_confidence
        );
        fflush(stdout);
    }

    result->frequency_hz =
        output_frequency;
    result->cents =
        pitch_cents(
            output_frequency,
            target_hz
        );
    result->confidence =
        output_confidence;
    result->best_score =
        refined_score;
    result->valid =
        output_valid;

    return ESP_OK;
}
