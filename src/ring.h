#pragma once

#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>

// one producer and one consumer, mapped twice in a row so a region never wraps
// the producer writes w and the wait and flush state, the consumer writes r, on separate cache lines
struct ring {
	uint8_t*                base;
	uint32_t                size;
	uint32_t                mask;
	sem_t                   sem;
	alignas(64) atomic_uint w;
	atomic_uint             wake_at;
	atomic_uint             flush_to;
	atomic_bool             waiting;
	atomic_bool             flush;
	alignas(64) atomic_uint r;
};

// once per ring, ring_wake is valid from then on whether or not memory is mapped
void ring_init(struct ring* g);
// the size is min rounded up to a power of two of at least one page
bool ring_map(struct ring* g, uint32_t min);
void ring_unmap(struct ring* g);
// producer side, sleeps until bytes are free, false as soon as stop is set
bool ring_wait(struct ring* g, uint32_t bytes, atomic_bool* stop);
// ends a ring_wait whose stop was set just before
void ring_wake(struct ring* g);

static inline uint32_t ring_writable(struct ring* g, uint8_t** p) {
	uint32_t w = atomic_load_explicit(&g->w, memory_order_relaxed);

	*p = g->base + (w & g->mask);

	return g->size - (w - atomic_load_explicit(&g->r, memory_order_acquire));
}

static inline void ring_commit(struct ring* g, uint32_t n) {
	atomic_store_explicit(&g->w, atomic_load_explicit(&g->w, memory_order_relaxed) + n, memory_order_release);
}

// producer side, the consumer skips what is queued now and keeps what is written from here on
static inline void ring_flush(struct ring* g) {
	atomic_store(&g->flush_to, atomic_load_explicit(&g->w, memory_order_relaxed));
	atomic_store(&g->flush, true);
}

static inline uint32_t ring_readable(struct ring* g, const uint8_t** p) {
	uint32_t r = atomic_load_explicit(&g->r, memory_order_relaxed);

	*p = g->base + (r & g->mask);

	return atomic_load_explicit(&g->w, memory_order_acquire) - r;
}

// the store of r and the load of waiting are sequentially consistent, as are their
// counterparts in ring_wait, so one side always sees the other and no wakeup is lost
static inline void ring_consume(struct ring* g, uint32_t n) {
	uint32_t r = atomic_load_explicit(&g->r, memory_order_relaxed) + n;

	atomic_store(&g->r, r);
	if(atomic_load(&g->waiting) && (int32_t)(r - atomic_load(&g->wake_at)) >= 0 && atomic_exchange(&g->waiting, false)) {
		sem_post(&g->sem);
	}
}

// consumer side, applies a pending flush and returns the bytes it skipped
static inline uint32_t ring_drop(struct ring* g) {
	uint32_t n;

	if(!atomic_load_explicit(&g->flush, memory_order_relaxed) || !atomic_exchange(&g->flush, false)) {
		return 0;
	}

	// a flush the consumer already read past leaves nothing to skip
	n = atomic_load(&g->flush_to) - atomic_load_explicit(&g->r, memory_order_relaxed);
	if((int32_t)n <= 0) {
		return 0;
	}

	ring_consume(g, n);

	return n;
}
