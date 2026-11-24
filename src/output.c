#include "output.h"

#include "gain.h"
#include "report.h"
#include "ring.h"

#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <string.h>

// the ring holds about this much audio, the decoder refills it at half
// an eighth of it stays free: after a flush the stream has yet to skip what the ring holds,
// and the audio that follows has to be in the ring before the stream asks for it
#define RING_SECONDS 2
#define RING_MAX     (16u << 20)
#define RESERVE_DIV  8
#define URGENT_DIV   4
// the gain is kept in units of 1/65536, a change of it is spread over blocks of RAMP frames
#define GAIN_ONE 65536
#define RAMP     32

typedef void wake_fn(void);

static struct {
	struct pw_thread_loop*    loop;
	struct pw_loop*           events;
	struct pw_context*        context;
	struct pw_core*           core;
	struct pw_stream*         stream;
	struct spa_source*        drain;
	wake_fn*                  wake;
	struct spa_hook           core_hook;
	struct spa_hook           stream_hook;
	struct spa_audio_info_raw format;
	struct ring               ring;
	gain_fn*                  scale;
	uint32_t                  stride;
	uint32_t                  reserve;
	uint32_t                  urgent;
	atomic_ullong             played;
	atomic_bool               draining;
	atomic_bool               drained;
	atomic_bool               failed;
	atomic_bool               interrupt;
	atomic_bool               drain_sent;
	atomic_uint               gain;
	uint32_t                  gain_now;
	bool                      ring_ready;
} out = { .gain = GAIN_ONE };

// runs on the loop thread, only the first failure is reported
static void fail(const char* what, const char* detail) {
	if(atomic_exchange(&out.failed, true)) {
		return;
	}

	if(detail) {
		report("pipewire: %s: %s", what, detail);
	} else {
		report("pipewire: %s", what);
	}

	atomic_store(&out.interrupt, true);
	ring_wake(&out.ring);
	out.wake();
}

static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
	if(id == PW_ID_CORE && res == -EPIPE) {
		fail("connection lost", nullptr);
	}
}

static const struct pw_core_events core_events = {
	.version = PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_state(void* data, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
	if(state == PW_STREAM_STATE_ERROR) {
		fail("stream", error);
	} else if(state == PW_STREAM_STATE_UNCONNECTED) {
		fail("stream disconnected", nullptr);
	}
}

// runs on the realtime thread, gain_now is the gain the last frame went out with
// a new gain is reached in steps over the buffer, a single step would click
static void amplify(uint8_t* dst, const uint8_t* src, uint32_t frames) {
	uint32_t target = atomic_load_explicit(&out.gain, memory_order_relaxed);
	uint32_t ch = out.format.channels;
	float g = (float)out.gain_now / GAIN_ONE;
	float step;
	uint32_t n;

	if(target == out.gain_now || !frames) {
		out.gain_now = target;
		if(target == GAIN_ONE) {
			memcpy(dst, src, (size_t)frames * out.stride);
		} else {
			out.scale(dst, src, frames * ch, (float)target / GAIN_ONE);
		}

		return;
	}

	out.gain_now = target;
	step = ((float)target / GAIN_ONE - g) / (float)((frames + RAMP - 1) / RAMP);
	for(; frames; frames -= n) {
		n = frames < RAMP ? frames : RAMP;
		g += step;
		out.scale(dst, src, n * ch, g);
		dst += (size_t)n * out.stride;
		src += (size_t)n * out.stride;
	}
}

// runs on the realtime thread, takes no lock and makes no call that blocks
static void on_process(void* data) {
	struct pw_buffer* b = pw_stream_dequeue_buffer(out.stream);
	struct spa_data* d;
	const uint8_t* src;
	uint32_t skipped;
	uint32_t want;
	uint32_t have;
	bool draining;

	if(!b) {
		return;
	}

	d = &b->buffer->datas[0];
	if(!d->data) {
		pw_stream_queue_buffer(out.stream, b);
		return;
	}

	want = d->maxsize / out.stride;
	if(b->requested && b->requested < want) {
		want = (uint32_t)b->requested;
	}

	// draining is read before the ring, every frame committed ahead of it is then visible
	draining = atomic_load(&out.draining);
	skipped = ring_drop(&out.ring) / out.stride;
	have = ring_readable(&out.ring, &src) / out.stride;
	if(have > want) {
		have = want;
	}

	amplify(d->data, src, have);
	ring_consume(&out.ring, have * out.stride);
	if(skipped + have) {
		atomic_fetch_add_explicit(&out.played, skipped + have, memory_order_relaxed);
	}

	if(have < want) {
		memset((uint8_t*)d->data + (size_t)have * out.stride, 0, (size_t)(want - have) * out.stride);
		if(!have && draining && !atomic_load_explicit(&out.drain_sent, memory_order_relaxed)) {
			atomic_store(&out.drain_sent, true);
			pw_loop_signal_event(out.events, out.drain);
		}
	}

	d->chunk->offset = 0;
	d->chunk->stride = (int32_t)out.stride;
	d->chunk->size = want * out.stride;
	pw_stream_queue_buffer(out.stream, b);
}

// the stream that asked may have been replaced since, the one in its place has not asked
static void on_drain(void* data, uint64_t count) {
	if(out.stream && atomic_load(&out.drain_sent)) {
		pw_stream_flush(out.stream, true);
	}
}

static void on_drained(void* data) {
	atomic_store(&out.drained, true);
	out.wake();
}

static const struct pw_stream_events stream_events = {
	.version = PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state,
	.process = on_process,
	.drained = on_drained,
};

static bool open_core(void) {
	out.loop = pw_thread_loop_new("nusic", nullptr);
	if(!out.loop) {
		report("pw_thread_loop_new: %s", strerror(errno));
		return false;
	}

	out.events = pw_thread_loop_get_loop(out.loop);
	out.context = pw_context_new(out.events, nullptr, 0);
	if(!out.context) {
		report("pw_context_new: %s", strerror(errno));
		return false;
	}

	out.core = pw_context_connect(out.context, nullptr, 0);
	if(!out.core) {
		report("pw_context_connect: %s", strerror(errno));
		return false;
	}

	pw_core_add_listener(out.core, &out.core_hook, &core_events, nullptr);
	out.drain = pw_loop_add_event(out.events, on_drain, nullptr);
	if(!out.drain) {
		report("pw_loop_add_event: %s", strerror(errno));
		return false;
	}

	if(pw_thread_loop_start(out.loop) < 0) {
		report("pw_thread_loop_start: %s", strerror(errno));
		return false;
	}

	return true;
}

// failed is set again when the connection does not come up, so the caller knows to call out_init once more
bool out_init(void (*wake)(void)) {
	if(!out.ring_ready) {
		ring_init(&out.ring);
		out.ring_ready = true;
	}

	out.wake = wake;
	atomic_store(&out.failed, false);
	pw_init(nullptr, nullptr);
	if(open_core()) {
		return true;
	}

	atomic_store(&out.failed, true);

	return false;
}

static void close_stream(void) {
	spa_hook_remove(&out.stream_hook);
	pw_stream_destroy(out.stream);
	out.stream = nullptr;
	ring_unmap(&out.ring);
}

// leaves the state ready for another out_init
void out_fini(void) {
	if(out.loop) {
		pw_thread_loop_stop(out.loop);
	}

	if(out.stream) {
		close_stream();
	}

	if(out.core) {
		pw_core_disconnect(out.core);
		out.core = nullptr;
	}

	if(out.context) {
		pw_context_destroy(out.context);
		out.context = nullptr;
	}

	if(out.loop) {
		pw_thread_loop_destroy(out.loop);
		out.loop = nullptr;
	}

	pw_deinit();
}

// the loop lock is held, the stream is declared at the rate of the file times the speed
// pipewire resamples it from there, so the pitch moves with the speed
static bool open_stream(const struct spa_audio_info_raw* fmt, uint32_t stride, uint32_t speed) {
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod* params[1];
	struct pw_properties* props;
	struct spa_audio_info_raw wire = *fmt;
	gain_fn* scale = gain_for(fmt->format);
	uint64_t bytes;
	int r;

	wire.rate = (uint32_t)(((uint64_t)fmt->rate * speed + 50) / 100);
	bytes = (uint64_t)wire.rate * stride * RING_SECONDS;
	if(atomic_load(&out.failed) || !scale || !wire.rate || !ring_map(&out.ring, bytes < RING_MAX ? (uint32_t)bytes : RING_MAX)) {
		return false;
	}

	// node.rate lets pipewire run the graph at the rate of the stream where the device allows it
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
	                          PW_KEY_MEDIA_CATEGORY, "Playback",
	                          PW_KEY_MEDIA_ROLE, "Music",
	                          PW_KEY_APP_NAME, "nusic",
	                          PW_KEY_NODE_NAME, "nusic",
	                          nullptr);
	if(props) {
		pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", wire.rate);
	}

	out.stream = pw_stream_new(out.core, "nusic", props);
	if(!out.stream) {
		report("pw_stream_new: %s", strerror(errno));
		ring_unmap(&out.ring);
		return false;
	}

	out.format = *fmt;
	out.scale = scale;
	out.stride = stride;
	out.gain_now = atomic_load(&out.gain);
	out.reserve = out.ring.size / RESERVE_DIV / stride * stride;
	out.urgent = 0;
	atomic_store(&out.drain_sent, false);
	atomic_store(&out.played, 0);
	atomic_store(&out.draining, false);
	atomic_store(&out.drained, false);
	pw_stream_add_listener(out.stream, &out.stream_hook, &stream_events, nullptr);
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &wire);
	r = pw_stream_connect(out.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                      (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
	                      params, 1);
	if(r < 0) {
		report("pw_stream_connect: %s", spa_strerror(r));
		close_stream();
		return false;
	}

	return true;
}

bool out_open(const struct spa_audio_info_raw* fmt, uint32_t stride, uint32_t speed) {
	bool ok;

	pw_thread_loop_lock(out.loop);
	if(out.stream) {
		close_stream();
	}

	ok = open_stream(fmt, stride, speed);
	pw_thread_loop_unlock(out.loop);

	return ok;
}

void out_close(void) {
	if(!out.stream) {
		return;
	}

	pw_thread_loop_lock(out.loop);
	close_stream();
	pw_thread_loop_unlock(out.loop);
}

bool out_is_open(void) {
	return out.stream != nullptr;
}

bool out_matches(const struct spa_audio_info_raw* fmt) {
	return out.stream && fmt->format == out.format.format && fmt->rate == out.format.rate && fmt->channels == out.format.channels
	       && !memcmp(fmt->position, out.format.position, fmt->channels * sizeof(*fmt->position));
}

// the frames that may be written now, urgent bytes of them may come out of the reserve
static uint32_t writable(uint8_t** dst) {
	uint32_t room = ring_writable(&out.ring, dst);
	uint32_t open = room > out.reserve ? room - out.reserve : 0;

	if(open < out.urgent) {
		open = room < out.urgent ? room : out.urgent;
	}

	return open / out.stride;
}

uint32_t out_wait(uint8_t** dst) {
	uint32_t room;

	if(atomic_load(&out.interrupt) || atomic_load(&out.failed)) {
		return 0;
	}

	room = writable(dst);
	if(room) {
		return room;
	}

	if(!ring_wait(&out.ring, out.ring.size / 2, &out.interrupt)) {
		return 0;
	}

	return writable(dst);
}

void out_commit(uint32_t frames) {
	uint32_t bytes = frames * out.stride;

	ring_commit(&out.ring, bytes);
	out.urgent = bytes < out.urgent ? out.urgent - bytes : 0;
}

// pipewire holds up to a quantum of its own, that goes too
void out_flush(void) {
	ring_flush(&out.ring);
	out.urgent = out.reserve / URGENT_DIV / out.stride * out.stride;
	pw_thread_loop_lock(out.loop);
	pw_stream_flush(out.stream, false);
	pw_thread_loop_unlock(out.loop);
}

void out_drain(void) {
	atomic_store(&out.draining, true);
}

bool out_drained(void) {
	return atomic_load(&out.drained);
}

void out_pause(bool paused) {
	if(!out.loop) {
		return;
	}

	pw_thread_loop_lock(out.loop);
	if(out.stream) {
		pw_stream_set_active(out.stream, !paused);
	}

	pw_thread_loop_unlock(out.loop);
}

void out_gain(float gain) {
	atomic_store(&out.gain, (uint32_t)(gain * GAIN_ONE + 0.5f));
}

void out_interrupt(bool on) {
	atomic_store(&out.interrupt, on);
	if(on) {
		ring_wake(&out.ring);
	}
}

uint64_t out_played(void) {
	return atomic_load_explicit(&out.played, memory_order_relaxed);
}

bool out_failed(void) {
	return atomic_load(&out.failed);
}
