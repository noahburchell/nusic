#pragma once

#include <spa/param/audio/raw.h>

// one thread, the producer, calls out_open, out_close, out_wait, out_commit, out_flush and out_drain
// the others are safe from any thread

// wake runs on a pipewire thread when the stream has drained or the output failed
bool out_init(void (*wake)(void));
void out_fini(void);
// speed is in percent, the frames of fmt play at that share of their rate and the pitch moves with it
bool out_open(const struct spa_audio_info_raw* fmt, uint32_t stride, uint32_t speed);
// closes at once, what is queued is not played
void out_close(void);
bool out_is_open(void);
bool out_matches(const struct spa_audio_info_raw* fmt);
// blocks until there is room, returns the frames that fit at dst
// 0 when interrupted or once the output failed
uint32_t out_wait(uint8_t** dst);
void out_commit(uint32_t frames);
// what is queued is skipped, frames committed after this still play
void out_flush(void);
// out_drained turns true once every committed frame has played
void out_drain(void);
bool out_drained(void);
void out_pause(bool paused);
// every sample is multiplied by gain and clipped at full scale, 1 leaves the audio as it is
void out_gain(float gain);
// while set, out_wait returns 0 without blocking
void out_interrupt(bool on);
// frames played or skipped since out_open
uint64_t out_played(void);
bool out_failed(void);
