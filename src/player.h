#pragma once

#include "library.h"

// the gain is in dB, the speed in millionths of the speed of the recording
// the pitch of a speed is in tenths of a semitone from the pitch of the recording
#define GAIN_MIN    (-40)
#define GAIN_MAX    24
#define SPEED_MIN   500000
#define SPEED_MAX   2000000
#define SPEED_UNITY 1000000
#define PITCH_MIN   (-120)
#define PITCH_MAX   120

enum play_state {
	PLAY_STOPPED,
	PLAY_PLAYING,
	PLAY_PAUSED,
};

enum play_repeat {
	REPEAT_OFF,
	REPEAT_QUEUE,
	REPEAT_TRACK,
};

// id tells two entries of the same track apart and stays with the entry when it moves
struct play_entry {
	const struct track* track;
	uint32_t            id;
};

// track is nullptr when stopped, otherwise that of queue[index], position and the duration of the track are in ms
// queue outlasts a stop, the array is the player's and moves in player_play and player_add
struct play_status {
	const struct track*      track;
	const struct play_entry* queue;
	uint32_t                 count;
	uint32_t                 index;
	uint32_t                 position;
	enum play_state          state;
	enum play_repeat         repeat;
	int                      gain;
	int                      speed;
};

// notify is an eventfd, written whenever the status changes other than by time passing
bool player_init(int notify);
void player_fini(void);
// every call but player_status is for one thread, the one that reads the queue of a status
// the n tracks from first on go to the top of the queue and the first of them plays
void player_start(const struct track* first, uint32_t n);
// the n tracks from first on go to the end of the queue, what plays goes on
void player_add(const struct track* first, uint32_t n);
// entry from takes the place of entry to, the entries between them shift by one, what plays goes on
void player_move(uint32_t from, uint32_t to);
// the entry leaves the queue, when it is the one that plays the entry after it plays in its place
void player_remove(uint32_t index);
// empties the queue and stops
void player_clear(void);
// plays the queue from entry index on
void player_jump(uint32_t index);
void player_stop(void);
void player_toggle(void);
void player_next(void);
// restarts the track when it is more than 3 s in, otherwise plays the one before it
void player_prev(void);
void player_seek_by(int32_t ms);
void player_seek_to(uint32_t ms);
// off, then the queue starts over when it ends, then the track plays again when it ends, then off
// next goes from the last track to the first while the queue repeats
void player_repeat(void);
void player_set_repeat(enum play_repeat repeat);
// the audio is scaled by the gain and clipped at full scale, 0 dB leaves it as it is
void player_gain_by(int db);
void player_gain_to(int db);
// the speed moves the pitch with it, what plays goes on from where it is after a short break
// position and duration stay in the time of the recording
void player_speed_to(int speed);
// a step ends on the next multiple of 0.05x, with semitones on the next whole semitone
// it starts from the speed as speed_ratio or speed_pitch gives it
void player_speed_step(int dir, bool semitones);
// the speed in hundredths of the speed of the recording, and its pitch
int speed_ratio(int speed);
int speed_pitch(int speed);
void player_status(struct play_status* s);
