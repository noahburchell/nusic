#pragma once

#include <spa/param/audio/raw.h>

bool out_init(void);
void out_fini(void);
// a new format first plays out what is queued, then reopens the stream
bool out_set_format(const struct spa_audio_info_raw* fmt, uint32_t stride);
// blocks until there is room, returns the frames that fit at dst, 0 once the output failed
uint32_t out_wait(uint8_t** dst);
void out_commit(uint32_t frames);
// blocks until every committed frame has played, then closes the stream
void out_drain(void);
bool out_failed(void);
