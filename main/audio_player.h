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
void audio_stop(void);

#ifdef __cplusplus
}
#endif