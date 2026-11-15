#include "output.h"

#include "report.h"
#include "ring.h"

#include <errno.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <string.h>

// the ring holds at least this much audio, the decoder refills it at half
#define RING_SECONDS 2
#define RING_MAX     (16u << 20)

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
	uint32_t                  stride;
	atomic_ullong             played;
	atomic_bool               draining;
	atomic_bool               drained;
	atomic_bool               failed;
	atomic_bool               interrupt;
	bool                      drain_sent;
	bool                      ring_ready;
} out;

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

	memcpy(d->data, src, (size_t)have * out.stride);
	ring_consume(&out.ring, have * out.stride);
	if(skipped + have) {
		atomic_fetch_add_explicit(&out.played, skipped + have, memory_order_relaxed);
	}

	if(have < want) {
		memset((uint8_t*)d->data + (size_t)have * out.stride, 0, (size_t)(want - have) * out.stride);
		if(!have && draining && !out.drain_sent) {
			out.drain_sent = true;
			pw_loop_signal_event(out.events, out.drain);
		}
	}

	d->chunk->offset = 0;
	d->chunk->stride = (int32_t)out.stride;
	d->chunk->size = want * out.stride;
	pw_stream_queue_buffer(out.stream, b);
}

static void on_drain(void* data, uint64_t count) {
	if(out.stream) {
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

bool out_init(void (*wake)(void)) {
	if(!out.ring_ready) {
		ring_init(&out.ring);
		out.ring_ready = true;
	}

	out.wake = wake;
	atomic_store(&out.failed, false);
	pw_init(nullptr, nullptr);
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

// the loop lock is held
static bool open_stream(const struct spa_audio_info_raw* fmt, uint32_t stride) {
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod* params[1];
	struct pw_properties* props;
	uint64_t bytes = (uint64_t)fmt->rate * stride * RING_SECONDS;
	int r;

	if(atomic_load(&out.failed) || !ring_map(&out.ring, bytes < RING_MAX ? (uint32_t)bytes : RING_MAX)) {
		return false;
	}

	// node.rate lets pipewire run the graph at the rate of the file where the device allows it
	props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
	                          PW_KEY_MEDIA_CATEGORY, "Playback",
	                          PW_KEY_MEDIA_ROLE, "Music",
	                          PW_KEY_APP_NAME, "nusic",
	                          PW_KEY_NODE_NAME, "nusic",
	                          nullptr);
	if(props) {
		pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", fmt->rate);
	}

	out.stream = pw_stream_new(out.core, "nusic", props);
	if(!out.stream) {
		report("pw_stream_new: %s", strerror(errno));
		ring_unmap(&out.ring);
		return false;
	}

	out.format = *fmt;
	out.stride = stride;
	out.drain_sent = false;
	atomic_store(&out.played, 0);
	atomic_store(&out.draining, false);
	atomic_store(&out.drained, false);
	pw_stream_add_listener(out.stream, &out.stream_hook, &stream_events, nullptr);
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &out.format);
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

bool out_open(const struct spa_audio_info_raw* fmt, uint32_t stride) {
	bool ok;

	pw_thread_loop_lock(out.loop);
	if(out.stream) {
		close_stream();
	}

	ok = open_stream(fmt, stride);
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

uint32_t out_wait(uint8_t** dst) {
	uint32_t room;

	if(atomic_load(&out.interrupt) || atomic_load(&out.failed)) {
		return 0;
	}

	room = ring_writable(&out.ring, dst) / out.stride;
	if(room) {
		return room;
	}

	if(!ring_wait(&out.ring, out.ring.size / 2, &out.interrupt)) {
		return 0;
	}

	return ring_writable(&out.ring, dst) / out.stride;
}

void out_commit(uint32_t frames) {
	ring_commit(&out.ring, frames * out.stride);
}

// pipewire holds up to a quantum of its own, that goes too
void out_flush(void) {
	ring_flush(&out.ring);
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
