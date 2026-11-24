#include "player.h"

#include "decode.h"
#include "output.h"
#include "report.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MARKS      32
#define STOP       UINT32_MAX
#define RESTART_MS 3000
// 0.05x in the hundredths of speed_ratio
#define RATIO_STEP 5

// DECODING fills the stream, SWITCHING waits for it to play out before a track of
// another format, ENDING waits for it to play out after the last track
enum phase {
	IDLE,
	DECODING,
	SWITCHING,
	ENDING,
};

// from frame at of the stream on, the audio is the track of entry id, starting at its frame base
// index is where the entry was in the queue, which is where the search for it starts
struct mark {
	const struct track* track;
	uint64_t            at;
	int64_t             base;
	uint32_t            id;
	uint32_t            index;
};

// lock guards queue to speed, the fields after speed belong to the player thread
// a command stays pending until the player thread has replaced the marks for it
// version changes with the queue and with the repeat mode, ending is set once the stream
// holds the last audio of the queue
static struct {
	pthread_t           thread;
	pthread_mutex_t     lock;
	sem_t               wake;
	int                 notify;
	struct play_entry*  queue;
	uint32_t            count;
	uint32_t            cap;
	uint32_t            next_id;
	uint32_t            version;
	uint32_t            cmd_seq;
	uint32_t            cmd_index;
	uint32_t            cmd_id;
	uint32_t            cmd_ms;
	bool                cmd_resume;
	bool                cmd_here;
	bool                pending;
	bool                quit;
	bool                ending;
	enum play_state     state;
	enum play_repeat    repeat;
	struct mark         marks[MARKS];
	uint32_t            n_marks;
	uint64_t            flushed;
	uint32_t            rate;
	int                 gain;
	int                 speed;
	struct decoder      dec;
	const struct track* track;
	enum phase          phase;
	uint64_t            written;
	uint32_t            index;
	uint32_t            id;
	int                 stream_speed;
	bool                open;
} p = { .lock = PTHREAD_MUTEX_INITIALIZER, .next_id = 1, .speed = SPEED_UNITY };

static void wake(void) {
	sem_post(&p.wake);
}

static void notify(void) {
	uint64_t one = 1;
	ssize_t r = write(p.notify, &one, sizeof(one));

	(void)r;
}

// the stream position counts skipped frames as soon as the flush is asked for
static uint64_t played(void) {
	uint64_t n = out_played();

	return n < p.flushed ? p.flushed : n;
}

// lock is held and there is a mark, the one that covers frame at of the stream
static struct mark* mark_at(uint64_t at) {
	uint32_t i = p.n_marks - 1;

	while(i && p.marks[i].at > at) {
		i--;
	}

	return &p.marks[i];
}

// lock is held, the place of entry id in the queue, the search starts where the entry was last
// returns count when the queue has no such entry
static uint32_t find(uint32_t id, uint32_t hint) {
	uint32_t i;

	if(hint < p.count && p.queue[hint].id == id) {
		return hint;
	}

	for(i = 0; i < p.count && p.queue[i].id != id; i++) {
	}

	return i;
}

// lock is held, a pending command shows as if it had been carried out
static void status_locked(struct play_status* s) {
	struct mark* m;
	uint64_t at;
	uint64_t ms;
	uint32_t i;

	*s = (struct play_status){ .queue = p.queue, .count = p.count, .state = PLAY_STOPPED, .repeat = p.repeat, .gain = p.gain, .speed = p.speed };
	if(p.pending) {
		i = p.cmd_index == STOP ? p.count : find(p.cmd_id, p.cmd_index);
		if(i == p.count) {
			return;
		}

		s->track = p.queue[i].track;
		s->state = p.cmd_resume || p.state == PLAY_STOPPED ? PLAY_PLAYING : p.state;
		s->position = p.cmd_ms;
		s->index = i;
		return;
	}

	if(p.state == PLAY_STOPPED || !p.n_marks) {
		return;
	}

	at = played();
	m = mark_at(at);
	i = find(m->id, m->index);
	m->index = i;
	ms = ((uint64_t)m->base + (at > m->at ? at - m->at : 0)) * 1000 / p.rate;
	s->track = m->track;
	s->state = p.state;
	s->position = m->track->duration && ms > m->track->duration ? m->track->duration : (uint32_t)ms;
	s->index = i < p.count ? i : 0;
}

// lock is held, the id of the entry that plays after entry id, 0 when the queue ends with it
static uint32_t follower(uint32_t id, uint32_t hint) {
	uint32_t i = find(id, hint);

	if(i == p.count) {
		return 0;
	}

	if(p.repeat == REPEAT_TRACK) {
		return id;
	}

	if(i + 1 < p.count) {
		return p.queue[i + 1].id;
	}

	return p.repeat == REPEAT_QUEUE ? p.queue[0].id : 0;
}

// lock is held and there is a mark, the decoder runs up to 2 s ahead of what plays
// true when every track the stream holds after the playing one still follows the one before it
static bool in_order(void) {
	const struct mark* m = mark_at(played());
	const struct mark* last = &p.marks[p.n_marks - 1];

	for(; m < last; m++) {
		if(m[1].id != follower(m->id, m->index)) {
			return false;
		}
	}

	return !p.ending || !follower(last->id, last->index);
}

// lock is held
static void post(uint32_t index, uint32_t ms, bool resume) {
	p.pending = true;
	p.cmd_seq++;
	p.cmd_index = index < p.count ? index : STOP;
	p.cmd_id = index < p.count ? p.queue[index].id : 0;
	p.cmd_ms = ms;
	p.cmd_resume = resume;
	p.cmd_here = false;
	out_interrupt(true);
	sem_post(&p.wake);
}

// lock is held, the queue or the repeat mode has changed
// when the stream holds audio that no longer follows, the playing track starts over from the frame
// that plays, which drops that audio and makes the choice of what follows again
static void replan(void) {
	struct play_status s;

	p.version++;
	if(p.pending || p.state == PLAY_STOPPED || !p.n_marks || in_order()) {
		return;
	}

	status_locked(&s);
	post(s.index, s.position, false);
	p.cmd_here = true;
}

// lock is held, the command numbered seq is done unless a newer one came in
static void done(uint32_t seq) {
	if(p.cmd_seq == seq) {
		p.pending = false;
		out_interrupt(false);
	}
}

static void close_decoder(void) {
	if(p.open) {
		dec_close(&p.dec);
		p.open = false;
	}
}

static void stop(uint32_t seq, bool command) {
	close_decoder();
	pthread_mutex_lock(&p.lock);
	out_close();
	p.state = PLAY_STOPPED;
	p.n_marks = 0;
	p.ending = false;
	if(command) {
		done(seq);
	}

	pthread_mutex_unlock(&p.lock);
	p.phase = IDLE;
	notify();
}

// lock is held, the marks that lie behind the stream position go
static void push_mark(void) {
	uint64_t at = played();

	while(p.n_marks > 1 && (p.marks[1].at <= at || p.n_marks == MARKS)) {
		memmove(&p.marks[0], &p.marks[1], (p.n_marks - 1) * sizeof(p.marks[0]));
		p.n_marks--;
	}

	p.marks[p.n_marks++] = (struct mark){ p.track, p.written, p.dec.pos, p.id, p.index };
}

// lock is held, the stream is opened for the decoder and holds nothing but its track
static bool reopen(void) {
	if(!out_open(&p.dec.format, p.dec.stride, (uint32_t)p.speed)) {
		return false;
	}

	p.stream_speed = p.speed;
	p.written = 0;
	p.flushed = 0;
	p.n_marks = 0;
	p.rate = p.dec.format.rate;
	push_mark();
	out_pause(p.state == PLAY_PAUSED);

	return true;
}

// puts the decoder on the first track from index on that opens, ms into it
// a repeating queue goes on from its last track to its first, false when no track is left or none opens
static bool load(uint32_t index, uint32_t ms) {
	const struct track* t = nullptr;
	enum play_repeat repeat;
	uint32_t failed = 0;
	int r;

	for(;; index++, ms = 0, t = nullptr) {
		pthread_mutex_lock(&p.lock);
		repeat = p.repeat;
		if(repeat == REPEAT_QUEUE && index >= p.count) {
			index = 0;
		}

		if(index < p.count && failed < p.count) {
			t = p.queue[index].track;
			p.id = p.queue[index].id;
		}

		pthread_mutex_unlock(&p.lock);
		if(!t) {
			close_decoder();
			return false;
		}

		p.track = t;
		p.index = index;
		if(!p.open || p.dec.path != t->path) {
			close_decoder();
			if(!dec_open(&p.dec, t->path)) {
				failed++;
				continue;
			}

			p.open = true;
			if(!ms) {
				return true;
			}
		}

		// a file that cannot seek plays on from where it is
		r = dec_seek(&p.dec, (int64_t)ms * p.dec.format.rate / 1000);
		if(r) {
			return true;
		}

		// ms is at or past the end, so the track has ended, one that repeats starts over
		if(repeat == REPEAT_TRACK && dec_seek(&p.dec, 0) > 0) {
			return true;
		}

		close_decoder();
	}
}

// lock is held, puts the decoder on the frame that plays next when the playing entry is the one it holds,
// so that the track goes on unbroken
// the stream takes its audio once per cycle, the flush that follows comes within the same cycle but for bad luck
static void catch_up(void) {
	const struct mark* m;
	uint64_t at;
	int64_t frame;
	int tries;

	for(tries = 0; tries < 3; tries++) {
		at = played();
		m = mark_at(at);
		frame = m->base + (int64_t)(at > m->at ? at - m->at : 0);
		if(m->id != p.id || frame == p.dec.pos || dec_seek(&p.dec, frame) <= 0) {
			return;
		}
	}
}

// a command: the stream is kept when it is running in the same format and speed, what it holds is skipped
// here asks for the playing track to go on from where it is, with ms as the place when the stream is not kept
static void begin(uint32_t seq, uint32_t index, uint32_t ms, bool resume, bool here) {
	bool ok = true;
	bool keep;

	if(index == STOP) {
		stop(seq, true);
		return;
	}

	if(out_failed()) {
		pthread_mutex_lock(&p.lock);
		out_fini();
		ok = out_init(wake);
		pthread_mutex_unlock(&p.lock);
	}

	if(!ok || !load(index, ms)) {
		stop(seq, true);
		return;
	}

	// a paused stream still holds a quantum of the old audio inside pipewire, so it is replaced
	pthread_mutex_lock(&p.lock);
	keep = p.phase == DECODING && p.state == PLAY_PLAYING && p.speed == p.stream_speed && out_matches(&p.dec.format);
	p.ending = false;
	if(resume || p.state == PLAY_STOPPED) {
		p.state = PLAY_PLAYING;
	}

	if(keep && here && p.n_marks) {
		catch_up();
	}

	if(keep) {
		out_flush();
		p.flushed = p.written;
		p.n_marks = 0;
		push_mark();
		out_pause(p.state == PLAY_PAUSED);
	} else {
		ok = reopen();
	}

	if(ok) {
		done(seq);
	}

	pthread_mutex_unlock(&p.lock);
	if(!ok) {
		stop(seq, true);
		return;
	}

	p.phase = DECODING;
	notify();
}

// the track ended by itself, false when the stream gets nothing more for now
static bool advance(void) {
	uint32_t ended = p.id;
	uint32_t hint = p.index;
	uint32_t version;
	uint32_t next;
	bool pending;
	bool loaded;
	bool settled;

	// a file takes a while to open, the choice is made again when the queue or the repeat mode changed meanwhile
	do {
		close_decoder();
		pthread_mutex_lock(&p.lock);
		pending = p.pending;
		version = p.version;
		next = find(ended, hint);
		if(next < p.count && p.repeat != REPEAT_TRACK) {
			next++;
		}

		pthread_mutex_unlock(&p.lock);
		if(pending) {
			return false;
		}

		loaded = load(next, 0);
		pthread_mutex_lock(&p.lock);
		settled = p.pending || p.version == version;
		if(settled && loaded) {
			push_mark();
		} else if(settled) {
			p.ending = true;
		}

		pthread_mutex_unlock(&p.lock);
	} while(!settled);

	if(!loaded) {
		out_drain();
		p.phase = ENDING;
	}

	return loaded;
}

static void pump(void) {
	uint8_t* dst;
	uint32_t room;
	uint32_t n;

	for(;;) {
		// only a track that follows on by itself gets here with another format
		if(!out_matches(&p.dec.format)) {
			out_drain();
			p.phase = SWITCHING;
			return;
		}

		room = out_wait(&dst);
		if(!room) {
			return;
		}

		n = dec_read(&p.dec, dst, room);
		out_commit(n);
		p.written += n;
		if(!dec_next(&p.dec) && !advance()) {
			return;
		}
	}
}

static void sleep_until_woken(void) {
	while(sem_wait(&p.wake) < 0 && errno == EINTR) {
	}
}

static void settle(void) {
	bool ok;

	if(!out_drained()) {
		sleep_until_woken();
		return;
	}

	if(p.phase == ENDING) {
		stop(0, false);
		return;
	}

	pthread_mutex_lock(&p.lock);
	ok = reopen();
	pthread_mutex_unlock(&p.lock);
	if(!ok) {
		stop(0, false);
		return;
	}

	p.phase = DECODING;
}

static void* run(void* arg) {
	uint32_t seq;
	uint32_t index;
	uint32_t ms;
	bool resume;
	bool here;
	bool pending;

	for(;;) {
		pthread_mutex_lock(&p.lock);
		if(p.quit) {
			pthread_mutex_unlock(&p.lock);
			break;
		}

		pending = p.pending;
		seq = p.cmd_seq;
		index = p.cmd_index == STOP ? p.count : find(p.cmd_id, p.cmd_index);
		index = index < p.count ? index : STOP;
		ms = p.cmd_ms;
		resume = p.cmd_resume;
		here = p.cmd_here;
		pthread_mutex_unlock(&p.lock);
		if(pending) {
			begin(seq, index, ms, resume, here);
		} else if(p.phase != IDLE && out_failed()) {
			stop(0, false);
		} else if(p.phase == DECODING) {
			pump();
		} else if(p.phase == IDLE) {
			sleep_until_woken();
		} else {
			settle();
		}
	}

	close_decoder();
	out_close();

	return nullptr;
}

bool player_init(int fd) {
	int r;

	p.notify = fd;
	sem_init(&p.wake, 0, 0);
	if(!out_init(wake)) {
		out_fini();
		return false;
	}

	r = pthread_create(&p.thread, nullptr, run, nullptr);
	if(r) {
		report("pthread_create: %s", strerror(r));
		out_fini();
		return false;
	}

	return true;
}

void player_fini(void) {
	pthread_mutex_lock(&p.lock);
	p.quit = true;
	out_interrupt(true);
	pthread_mutex_unlock(&p.lock);
	sem_post(&p.wake);
	pthread_join(p.thread, nullptr);
	out_fini();
	free(p.queue);
	p.queue = nullptr;
	p.count = 0;
	p.cap = 0;
}

// lock is held, the n tracks from first on go into the queue at place at, false when there is no memory
static bool insert(uint32_t at, const struct track* first, uint32_t n) {
	struct play_entry* q = p.queue;
	uint32_t cap = p.cap ? p.cap : 256;
	uint32_t i;

	while(cap - p.count < n) {
		cap *= 2;
	}

	if(cap != p.cap) {
		q = realloc(p.queue, cap * sizeof(*q));
		if(!q) {
			report("malloc: %s", strerror(ENOMEM));
			return false;
		}

		p.queue = q;
		p.cap = cap;
	}

	memmove(&q[at + n], &q[at], (p.count - at) * sizeof(*q));
	for(i = 0; i < n; i++) {
		q[at + i] = (struct play_entry){ &first[i], p.next_id++ };
	}

	p.count += n;

	return true;
}

void player_start(const struct track* first, uint32_t n) {
	pthread_mutex_lock(&p.lock);
	if(n && insert(0, first, n)) {
		p.version++;
		post(0, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_add(const struct track* first, uint32_t n) {
	pthread_mutex_lock(&p.lock);
	if(insert(p.count, first, n)) {
		replan();
	}

	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_remove(uint32_t index) {
	struct play_status s;
	uint32_t next = STOP;

	pthread_mutex_lock(&p.lock);
	if(index < p.count) {
		status_locked(&s);
		memmove(&p.queue[index], &p.queue[index + 1], (p.count - index - 1) * sizeof(*p.queue));
		p.count--;
		if(s.track && s.index == index) {
			// a paused player stays paused on the entry that takes the place
			if(index < p.count) {
				next = index;
			} else if(p.repeat == REPEAT_QUEUE && p.count) {
				next = 0;
			}

			p.version++;
			post(next, 0, false);
		} else {
			replan();
		}
	}

	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_clear(void) {
	pthread_mutex_lock(&p.lock);
	p.count = 0;
	p.version++;
	post(STOP, 0, false);
	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_move(uint32_t from, uint32_t to) {
	struct play_entry e;

	pthread_mutex_lock(&p.lock);
	if(from < p.count && to < p.count && from != to) {
		e = p.queue[from];
		if(from < to) {
			memmove(&p.queue[from], &p.queue[from + 1], (to - from) * sizeof(e));
		} else {
			memmove(&p.queue[to + 1], &p.queue[to], (from - to) * sizeof(e));
		}

		p.queue[to] = e;
		replan();
	}

	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_jump(uint32_t index) {
	pthread_mutex_lock(&p.lock);
	if(index < p.count) {
		post(index, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_stop(void) {
	pthread_mutex_lock(&p.lock);
	post(STOP, 0, false);
	pthread_mutex_unlock(&p.lock);
}

void player_toggle(void) {
	pthread_mutex_lock(&p.lock);
	if(p.state == PLAY_PLAYING) {
		p.state = PLAY_PAUSED;
		out_pause(true);
	} else if(p.state == PLAY_PAUSED) {
		p.state = PLAY_PLAYING;
		out_pause(false);
	}

	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_next(void) {
	struct play_status s;
	uint32_t next = STOP;

	pthread_mutex_lock(&p.lock);
	status_locked(&s);
	if(s.index + 1 < p.count) {
		next = s.index + 1;
	} else if(p.repeat == REPEAT_QUEUE) {
		next = 0;
	}

	if(s.track) {
		post(next, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_prev(void) {
	struct play_status s;

	pthread_mutex_lock(&p.lock);
	status_locked(&s);
	if(s.track) {
		post(s.position > RESTART_MS || !s.index ? s.index : s.index - 1, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

// lock is held, a seek keeps a paused track paused and a pending start starting
static void seek_locked(const struct play_status* s, uint32_t ms) {
	if(s->track->duration && ms > s->track->duration) {
		ms = s->track->duration;
	}

	post(s->index, ms, p.pending && p.cmd_resume);
}

void player_seek_by(int32_t ms) {
	struct play_status s;

	pthread_mutex_lock(&p.lock);
	status_locked(&s);
	if(s.track) {
		seek_locked(&s, ms < 0 && s.position < (uint32_t)-ms ? 0 : s.position + (uint32_t)ms);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_seek_to(uint32_t ms) {
	struct play_status s;

	pthread_mutex_lock(&p.lock);
	status_locked(&s);
	if(s.track) {
		seek_locked(&s, ms);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_repeat(void) {
	pthread_mutex_lock(&p.lock);
	p.repeat = (enum play_repeat)((p.repeat + 1) % 3);
	replan();
	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_set_repeat(enum play_repeat repeat) {
	pthread_mutex_lock(&p.lock);
	if(p.repeat != repeat) {
		p.repeat = repeat;
		replan();
	}

	pthread_mutex_unlock(&p.lock);
	notify();
}

static int limit(int v, int lo, int hi) {
	if(v < lo) {
		return lo;
	}

	return v > hi ? hi : v;
}

// lock is held
static void gain_locked(int db) {
	p.gain = limit(db, GAIN_MIN, GAIN_MAX);
	out_gain(powf(10.0f, (float)p.gain / 20.0f));
}

void player_gain_by(int db) {
	pthread_mutex_lock(&p.lock);
	gain_locked(p.gain + db);
	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_gain_to(int db) {
	pthread_mutex_lock(&p.lock);
	gain_locked(db);
	pthread_mutex_unlock(&p.lock);
	notify();
}

// lock is held, the stream has its speed from the time it opens
// so what plays starts over from where it is in a stream of the new speed
static void speed_locked(int speed) {
	struct play_status s;

	speed = limit(speed, SPEED_MIN, SPEED_MAX);
	if(speed == p.speed) {
		return;
	}

	p.speed = speed;
	status_locked(&s);
	if(s.track) {
		seek_locked(&s, s.position);
	}
}

int speed_ratio(int speed) {
	return (speed + SPEED_UNITY / 200) / (SPEED_UNITY / 100);
}

int speed_pitch(int speed) {
	return (int)lround(120.0 * log2((double)speed / SPEED_UNITY));
}

// the first step past the speed in the direction of dir
static int speed_step(int speed, int dir, bool semitones) {
	int v;

	if(semitones) {
		v = speed_pitch(speed) - PITCH_MIN;
		v = dir > 0 ? v / 10 + 1 : (v + 9) / 10 - 1;
		return (int)lround(exp2((double)(v * 10 + PITCH_MIN) / 120.0) * SPEED_UNITY);
	}

	v = speed_ratio(speed);
	v = dir > 0 ? v / RATIO_STEP + 1 : (v + RATIO_STEP - 1) / RATIO_STEP - 1;

	return v * RATIO_STEP * (SPEED_UNITY / 100);
}

void player_speed_step(int dir, bool semitones) {
	pthread_mutex_lock(&p.lock);
	speed_locked(speed_step(p.speed, dir, semitones));
	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_speed_to(int speed) {
	pthread_mutex_lock(&p.lock);
	speed_locked(speed);
	pthread_mutex_unlock(&p.lock);
	notify();
}

void player_status(struct play_status* s) {
	pthread_mutex_lock(&p.lock);
	status_locked(s);
	pthread_mutex_unlock(&p.lock);
}
