#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { FT_COUNT_IN, FT_READY, FT_HOLDING, FT_RETRY, FT_COMPLETE, FT_DONE } ft_phase_t;
typedef struct {
    ft_phase_t phase;
    uint32_t target_ms, elapsed_ms, count_in_ms, remaining_ms;
    uint32_t bad_ms, release_ms, complete_ms, retries;
    unsigned retry_reason; /* 1=short, 2=pitch, 3=rest noise, 4=too long */
    int64_t last_us;
    bool rest, armed, previous_good, pitch_ok, sounding;
} follow_trainer_t;
void ft_begin(follow_trainer_t *t, uint32_t target_ms, bool rest, uint32_t count_in_ms, int64_t now_us);
/* Returns true once per completed note, after visible confirmation and release. */
bool ft_tick(follow_trainer_t *t, int64_t now_us, bool sounding, bool pitch_ok);
const char *ft_phase_name(ft_phase_t p);
