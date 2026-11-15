#pragma once

#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>

// one producer and one consumer, mapped twice in a row so a region never wraps
// the producer writes w and the wait state, the consumer writes r, on separate cache lines
struct ring {
	uint8_t*                base;
	uint32_t                size;
	uint32_t                mask;
	sem_t                   sem;
	alignas(64) atomic_uint w;
	atomic_uint             wake_at;
	atomic_bool             waiting;
	atomic_bool             aborted;
	alignas(64) atomic_uint r;
};

// the size is min rounded up to a power of two of at least one page
bool ring_init(struct ring* g, uint32_t min);
void ring_destroy(struct ring* g);
// producer side, sleeps until bytes are free, false once the ring is aborted
bool ring_wait(struct ring* g, uint32_t bytes);
void ring_abort(struct ring* g);

static inline uint32_t ring_writable(struct ring* g, uint8_t** p) {
	uint32_t w = atomic_load_explicit(&g->w, memory_order_relaxed);

	*p = g->base + (w & g->mask);

	return g->size - (w - atomic_load_explicit(&g->r, memory_order_acquire));
}

static inline void ring_commit(struct ring* g, uint32_t n) {
	atomic_store_explicit(&g->w, atomic_load_explicit(&g->w, memory_order_relaxed) + n, memory_order_release);
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
