#pragma once

#include "library.h"

enum play_state {
	PLAY_STOPPED,
	PLAY_PLAYING,
	PLAY_PAUSED,
};

// track is nullptr when stopped, position and the duration of the track are in ms
struct play_status {
	const struct track* track;
	enum play_state     state;
	uint32_t            position;
};

// notify is an eventfd, written whenever the status changes other than by time passing
bool player_init(int notify);
void player_fini(void);
// plays queue[start] and then the tracks after it, queue stays valid until the next call
void player_play(const struct track* queue, uint32_t n, uint32_t start);
void player_stop(void);
void player_toggle(void);
void player_next(void);
// restarts the track when it is more than 3 s in, otherwise plays the one before it
void player_prev(void);
void player_seek_by(int32_t ms);
void player_seek_to(uint32_t ms);
void player_status(struct play_status* s);
