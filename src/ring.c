#include "ring.h"

#include "report.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void ring_init(struct ring* g) {
	sem_init(&g->sem, 0, 0);
	g->base = nullptr;
	atomic_store(&g->waiting, false);
}

bool ring_map(struct ring* g, uint32_t min) {
	uint32_t page = (uint32_t)sysconf(_SC_PAGESIZE);
	uint32_t size = min > page ? min : page;
	const char* call = "memfd_create";
	uint8_t* base = MAP_FAILED;
	int fd;

	size = 1u << (32 - __builtin_clz(size - 1));
	fd = memfd_create("nusic-ring", MFD_CLOEXEC);
	if(fd < 0) {
		goto fail;
	}

	call = "ftruncate";
	if(ftruncate(fd, size) < 0) {
		goto fail;
	}

	// both halves are populated now so the realtime thread never faults on them
	call = "mmap";
	base = mmap(nullptr, (size_t)size * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if(base == MAP_FAILED
	   || mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_POPULATE, fd, 0) == MAP_FAILED
	   || mmap(base + size, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_POPULATE, fd, 0) == MAP_FAILED) {
		goto fail;
	}

	close(fd);
	g->base = base;
	g->size = size;
	g->mask = size - 1;
	atomic_store(&g->w, 0);
	atomic_store(&g->r, 0);
	atomic_store(&g->wake_at, 0);
	atomic_store(&g->flush_to, 0);
	atomic_store(&g->flush, false);

	return true;

fail:
	report("%s: %s", call, strerror(errno));
	if(base != MAP_FAILED) {
		munmap(base, (size_t)size * 2);
	}

	if(fd >= 0) {
		close(fd);
	}

	return false;
}

void ring_unmap(struct ring* g) {
	munmap(g->base, (size_t)g->size * 2);
	g->base = nullptr;
}

bool ring_wait(struct ring* g, uint32_t bytes, atomic_bool* stop) {
	uint32_t target = atomic_load_explicit(&g->w, memory_order_relaxed) + bytes - g->size;

	atomic_store(&g->wake_at, target);
	for(;;) {
		atomic_store(&g->waiting, true);
		if(atomic_load(stop)) {
			atomic_store(&g->waiting, false);
			return false;
		}

		if((int32_t)(atomic_load(&g->r) - target) >= 0) {
			atomic_store(&g->waiting, false);
			return true;
		}

		while(sem_wait(&g->sem) < 0 && errno == EINTR) {
		}
	}
}

void ring_wake(struct ring* g) {
	if(atomic_exchange(&g->waiting, false)) {
		sem_post(&g->sem);
	}
}
