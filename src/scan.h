#pragma once

#include <stddef.h>
#include <stdint.h>

struct chunk;

struct arena {
	struct chunk* head;
};

// what one file states, a text tag it does not have is a null pointer and a number 0
// mtime is in ns since the epoch, duration in ms, ok is set once the tags are known
struct raw {
	const char* path;
	const char* title;
	const char* artist;
	const char* album;
	const char* album_artist;
	int64_t     mtime;
	int64_t     size;
	uint32_t    duration;
	uint16_t    year;
	uint16_t    album_year;
	uint16_t    number;
	uint16_t    disc;
	bool        ok;
};

struct raws {
	struct raw* v;
	uint32_t    n;
	uint32_t    cap;
};

const char* arena_dup(struct arena* a, const char* s, size_t len);
// the next element of rs, zeroed
struct raw* raws_push(struct raws* rs);
