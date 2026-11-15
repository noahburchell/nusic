#include "player.h"

#include "decode.h"
#include "output.h"
#include "report.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <unistd.h>

#define MARKS      32
#define STOP       UINT32_MAX
#define RESTART_MS 3000

// DECODING fills the stream, SWITCHING waits for it to play out before a track of
// another format, ENDING waits for it to play out after the last track
enum phase {
	IDLE,
	DECODING,
	SWITCHING,
	ENDING,
};

// from frame at of the stream on, the audio is track, starting at its frame base
struct mark {
	const struct track* track;
	uint64_t            at;
	int64_t             base;
	uint32_t            index;
};

// lock guards queue to rate, the fields after rate belong to the player thread
// a command stays pending until the player thread has replaced the marks for it
static struct {
	pthread_t           thread;
	pthread_mutex_t     lock;
	sem_t               wake;
	int                 notify;
	const struct track* queue;
	uint32_t            count;
	uint32_t            cmd_seq;
	uint32_t            cmd_index;
	uint32_t            cmd_ms;
	bool                cmd_resume;
	bool                pending;
	bool                quit;
	enum play_state     state;
	struct mark         marks[MARKS];
	uint32_t            n_marks;
	uint64_t            flushed;
	uint32_t            rate;
	struct decoder      dec;
	const struct track* track;
	enum phase          phase;
	uint64_t            written;
	uint32_t            index;
	bool                open;
} p = { .lock = PTHREAD_MUTEX_INITIALIZER };

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

// lock is held, a pending command shows as if it had been carried out
static void status_locked(struct play_status* s, uint32_t* index) {
	const struct mark* m;
	uint64_t at;
	uint64_t ms;
	uint32_t i;

	*s = (struct play_status){ nullptr, PLAY_STOPPED, 0 };
	*index = 0;
	if(p.pending) {
		if(p.cmd_index == STOP) {
			return;
		}

		s->track = &p.queue[p.cmd_index];
		s->state = p.cmd_resume || p.state == PLAY_STOPPED ? PLAY_PLAYING : p.state;
		s->position = p.cmd_ms;
		*index = p.cmd_index;
		return;
	}

	if(p.state == PLAY_STOPPED || !p.n_marks) {
		return;
	}

	at = played();
	i = p.n_marks - 1;
	while(i && p.marks[i].at > at) {
		i--;
	}

	m = &p.marks[i];
	ms = ((uint64_t)m->base + (at > m->at ? at - m->at : 0)) * 1000 / p.rate;
	s->track = m->track;
	s->state = p.state;
	s->position = m->track->duration && ms > m->track->duration ? m->track->duration : (uint32_t)ms;
	*index = m->index;
}

// lock is held
static void post(uint32_t index, uint32_t ms, bool resume) {
	p.pending = true;
	p.cmd_seq++;
	p.cmd_index = index;
	p.cmd_ms = ms;
	p.cmd_resume = resume;
	out_interrupt(true);
	sem_post(&p.wake);
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

	p.marks[p.n_marks++] = (struct mark){ p.track, p.written, p.dec.pos, p.index };
}

// lock is held, the stream is opened for the decoder and holds nothing but its track
static bool reopen(void) {
	if(!out_open(&p.dec.format, p.dec.stride)) {
		return false;
	}

	p.written = 0;
	p.flushed = 0;
	p.n_marks = 0;
	p.rate = p.dec.format.rate;
	push_mark();
	out_pause(p.state == PLAY_PAUSED);

	return true;
}

// puts the decoder on the first track from index on that opens, ms into it
// false when no track is left
static bool load(uint32_t index, uint32_t ms) {
	const struct track* t;
	int r;

	for(;; index++, ms = 0) {
		pthread_mutex_lock(&p.lock);
		t = index < p.count ? &p.queue[index] : nullptr;
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

		close_decoder();
	}
}

// a command: the stream is kept when it is running in the same format, what it holds is skipped
static void begin(uint32_t seq, uint32_t index, uint32_t ms, bool resume) {
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
	keep = p.phase == DECODING && p.state == PLAY_PLAYING && out_matches(&p.dec.format);
	if(resume || p.state == PLAY_STOPPED) {
		p.state = PLAY_PLAYING;
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
	uint32_t next = p.index + 1;
	bool pending;

	close_decoder();
	pthread_mutex_lock(&p.lock);
	pending = p.pending;
	pthread_mutex_unlock(&p.lock);
	if(pending) {
		return false;
	}

	if(!load(next, 0)) {
		out_drain();
		p.phase = ENDING;
		return false;
	}

	pthread_mutex_lock(&p.lock);
	push_mark();
	pthread_mutex_unlock(&p.lock);

	return true;
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
	bool pending;

	for(;;) {
		pthread_mutex_lock(&p.lock);
		if(p.quit) {
			pthread_mutex_unlock(&p.lock);
			break;
		}

		pending = p.pending;
		seq = p.cmd_seq;
		index = p.cmd_index;
		ms = p.cmd_ms;
		resume = p.cmd_resume;
		pthread_mutex_unlock(&p.lock);
		if(pending) {
			begin(seq, index, ms, resume);
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
	p.notify = fd;
	sem_init(&p.wake, 0, 0);
	if(!out_init(wake)) {
		out_fini();
		return false;
	}

	if(pthread_create(&p.thread, nullptr, run, nullptr)) {
		report("pthread_create: %s", strerror(errno));
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
}

void player_play(const struct track* queue, uint32_t n, uint32_t start) {
	pthread_mutex_lock(&p.lock);
	p.queue = queue;
	p.count = n;
	post(start, 0, true);
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
	uint32_t index;

	pthread_mutex_lock(&p.lock);
	status_locked(&s, &index);
	if(s.track) {
		post(index + 1 < p.count ? index + 1 : STOP, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_prev(void) {
	struct play_status s;
	uint32_t index;

	pthread_mutex_lock(&p.lock);
	status_locked(&s, &index);
	if(s.track) {
		post(s.position > RESTART_MS || !index ? index : index - 1, 0, true);
	}

	pthread_mutex_unlock(&p.lock);
}

// lock is held, a seek keeps a paused track paused and a pending start starting
static void seek_locked(const struct play_status* s, uint32_t index, uint32_t ms) {
	if(s->track->duration && ms > s->track->duration) {
		ms = s->track->duration;
	}

	post(index, ms, p.pending && p.cmd_resume);
}

void player_seek_by(int32_t ms) {
	struct play_status s;
	uint32_t index;

	pthread_mutex_lock(&p.lock);
	status_locked(&s, &index);
	if(s.track) {
		seek_locked(&s, index, ms < 0 && s.position < (uint32_t)-ms ? 0 : s.position + (uint32_t)ms);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_seek_to(uint32_t ms) {
	struct play_status s;
	uint32_t index;

	pthread_mutex_lock(&p.lock);
	status_locked(&s, &index);
	if(s.track) {
		seek_locked(&s, index, ms);
	}

	pthread_mutex_unlock(&p.lock);
}

void player_status(struct play_status* s) {
	uint32_t index;

	pthread_mutex_lock(&p.lock);
	status_locked(s, &index);
	pthread_mutex_unlock(&p.lock);
}
