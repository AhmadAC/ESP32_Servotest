#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_player_init(void);
void audio_set_volume(int volume_pct); // 0 to 100
int audio_get_volume(void);
void audio_play(const char *sound_name);
void audio_play_test_beep(void);
void audio_stop(void);

// Hardware pin mapping configuration
void audio_set_pins(int bclk, int lrc, int din);
void audio_get_pins(int *bclk, int *lrc, int *din);

#ifdef __cplusplus
}
#endif