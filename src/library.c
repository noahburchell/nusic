#include "library.h"

#include "db.h"
#include "report.h"
#include "scan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK       (64u << 10)
#define MAX_DEPTH   32
#define MAX_WORKERS 16
#define NO_YEAR     UINT16_MAX

struct chunk {
	struct chunk* next;
	size_t        used;
	size_t        size;
	char          data[];
};

struct scan {
	struct raw* raws;
	uint32_t    total;
	atomic_uint next;
	atomic_uint done;
};

struct worker {
	pthread_t    thread;
	struct scan* scan;
	struct arena arena;
};

static const char unknown_artist[] = "unknown artist";
static const char unknown_album[] = "unknown album";

static const char* const extensions[] = {
	"flac",
	"mp3",
	"m4a",
	"ogg",
	"opus",
	"oga",
	"wav",
	"aiff",
	"aif",
	"wv",
	"ape",
	"aac",
	"mka",
	"wma",
	"mpc",
	"tta",
	"dsf",
	"caf",
	"m4b",
	"mp2",
	"ac3",
};

static void* xrealloc(void* p, size_t n) {
	p = realloc(p, n);
	if(!p) {
		report("malloc: %s", strerror(ENOMEM));
		exit(1);
	}

	return p;
}

static char* arena_alloc(struct arena* a, size_t n) {
	struct chunk* c = a->head;
	char* p;

	if(!c || c->size - c->used < n) {
		size_t size = n > CHUNK ? n : CHUNK;

		c = xrealloc(nullptr, sizeof(*c) + size);
		c->next = a->head;
		c->used = 0;
		c->size = size;
		a->head = c;
	}

	p = c->data + c->used;
	c->used += n;

	return p;
}

static void arena_free(struct arena* a) {
	struct chunk* c = a->head;

	while(c) {
		struct chunk* next = c->next;

		free(c);
		c = next;
	}

	a->head = nullptr;
}

const char* arena_dup(struct arena* a, const char* s, size_t len) {
	char* p = arena_alloc(a, len + 1);

	memcpy(p, s, len);
	p[len] = 0;

	return p;
}

static bool is_audio(const char* name) {
	const char* ext = strrchr(name, '.');
	size_t i;

	if(!ext) {
		return false;
	}

	for(i = 0; i < sizeof(extensions) / sizeof(*extensions); i++) {
		if(!strcasecmp(ext + 1, extensions[i])) {
			return true;
		}
	}

	return false;
}

struct raw* raws_push(struct raws* rs) {
	struct raw* r;

	if(rs->n == rs->cap) {
		rs->cap = rs->cap ? rs->cap * 2 : 1024;
		rs->v = xrealloc(rs->v, rs->cap * sizeof(*rs->v));
	}

	r = &rs->v[rs->n++];
	*r = (struct raw){};

	return r;
}

// st is null when the size and modification time are of no use
static void push(struct raws* rs, struct arena* a, const char* path, const struct stat* st) {
	struct raw* r = raws_push(rs);

	r->path = arena_dup(a, path, strlen(path));
	if(st) {
		r->mtime = (int64_t)st->st_mtim.tv_sec * 1000000000 + st->st_mtim.tv_nsec;
		r->size = st->st_size;
	}
}

// the depth limit ends a loop of symlinks
static void walk(struct raws* rs, struct arena* a, const char* dir, int depth, bool stats) {
	char path[PATH_MAX];
	struct dirent* e;
	struct stat st;
	DIR* d = opendir(dir);

	if(!d) {
		report("open %s: %s", dir, strerror(errno));
		return;
	}

	while((e = readdir(d))) {
		unsigned type = e->d_type;
		bool have = false;

		if(e->d_name[0] == '.' || (size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
			continue;
		}

		if(type == DT_UNKNOWN || type == DT_LNK) {
			if(stat(path, &st) < 0) {
				continue;
			}

			have = true;
			type = DT_UNKNOWN;
			if(S_ISDIR(st.st_mode)) {
				type = DT_DIR;
			} else if(S_ISREG(st.st_mode)) {
				type = DT_REG;
			}
		}

		if(type == DT_DIR && depth < MAX_DEPTH) {
			walk(rs, a, path, depth + 1, stats);
		} else if(type == DT_REG && is_audio(e->d_name)) {
			if(stats && !have && fstatat(dirfd(d), e->d_name, &st, 0) < 0) {
				continue;
			}

			push(rs, a, path, stats ? &st : nullptr);
		}
	}

	closedir(d);
}

// the paths are absolute, the database outlives the working directory
// false when the operand names nothing that can be read, which is reported
static bool collect(struct raws* rs, struct arena* a, const char* operand, bool stats) {
	char path[PATH_MAX];
	struct stat st;

	if(!realpath(operand, path) || stat(path, &st) < 0) {
		report("open %s: %s", operand, strerror(errno));
		return false;
	}

	if(S_ISDIR(st.st_mode)) {
		walk(rs, a, path, 0, stats);
	} else {
		push(rs, a, path, stats ? &st : nullptr);
	}

	return true;
}

static const char* tag(const AVFormatContext* f, const AVStream* st, const char* key) {
	const AVDictionaryEntry* e = av_dict_get(f->metadata, key, nullptr, 0);

	// ogg keeps its tags on the stream
	if(!e || !e->value[0]) {
		e = av_dict_get(st->metadata, key, nullptr, 0);
	}

	return e && e->value[0] ? e->value : nullptr;
}

static uint16_t number(const char* s) {
	long v = s ? strtol(s, nullptr, 10) : 0;

	return v > 0 && v < UINT16_MAX ? (uint16_t)v : 0;
}

static const AVStream* audio_stream(const AVFormatContext* f) {
	unsigned i;

	for(i = 0; i < f->nb_streams; i++) {
		if(f->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			return f->streams[i];
		}
	}

	return nullptr;
}

static int64_t duration_us(const AVFormatContext* f, const AVStream* st) {
	if(st->duration != AV_NOPTS_VALUE && st->duration > 0) {
		return av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
	}

	return f->duration != AV_NOPTS_VALUE && f->duration > 0 ? f->duration : 0;
}

static const char* keep(struct arena* a, const char* v) {
	return v ? arena_dup(a, v, strlen(v)) : nullptr;
}

// the header of the file holds the tags, probing the streams is the slow path taken
// only when the header names no audio stream or no duration
static void read_tags(struct raw* t, struct arena* a) {
	AVFormatContext* f = nullptr;
	const AVStream* st;
	const char* v;
	int64_t us;

	if(avformat_open_input(&f, t->path, nullptr, nullptr) < 0) {
		return;
	}

	st = audio_stream(f);
	us = st ? duration_us(f, st) : 0;
	if(!us) {
		// a file that is audio by name only still opens, probing finds no sample rate in it
		if(avformat_find_stream_info(f, nullptr) >= 0) {
			st = audio_stream(f);
			us = st ? duration_us(f, st) : 0;
		}

		if(st && st->codecpar->sample_rate < 1) {
			st = nullptr;
		}
	}

	if(!st) {
		avformat_close_input(&f);
		return;
	}

	t->title = keep(a, tag(f, st, "title"));
	t->artist = keep(a, tag(f, st, "artist"));
	t->album = keep(a, tag(f, st, "album"));
	v = tag(f, st, "album_artist");
	if(!v) {
		v = tag(f, st, "album artist");
	}

	t->album_artist = keep(a, v);
	v = tag(f, st, "date");
	if(!v) {
		v = tag(f, st, "year");
	}

	t->year = number(v);
	t->number = number(tag(f, st, "track"));
	t->disc = number(tag(f, st, "disc"));
	t->duration = (uint32_t)(us / 1000);
	t->ok = true;
	avformat_close_input(&f);
}

static void* scan_thread(void* arg) {
	struct worker* w = arg;
	struct scan* s = w->scan;

	for(;;) {
		uint32_t i = atomic_fetch_add(&s->next, 1);

		if(i >= s->total) {
			return nullptr;
		}

		if(!s->raws[i].ok) {
			read_tags(&s->raws[i], &w->arena);
			atomic_fetch_add(&s->done, 1);
		}
	}
}

// a file without a title is known by its name, an album without an album artist is its artist's
static void fill(struct raw* r, uint32_t n, struct arena* a) {
	uint32_t i;

	for(i = 0; i < n; i++) {
		struct raw* t = &r[i];

		if(!t->title) {
			const char* name = strrchr(t->path, '/');
			const char* dot;

			name = name ? name + 1 : t->path;
			dot = strrchr(name, '.');
			t->title = arena_dup(a, name, dot && dot > name ? (size_t)(dot - name) : strlen(name));
		}

		if(!t->artist) {
			t->artist = unknown_artist;
		}

		if(!t->album) {
			t->album = unknown_album;
		}

		if(!t->album_artist) {
			t->album_artist = t->artist;
		}
	}
}

static int cmp_raw(const void* pa, const void* pb) {
	const struct raw* a = pa;
	const struct raw* b = pb;
	int c = strcasecmp(a->album_artist, b->album_artist);

	if(c) {
		return c;
	}

	if(a->album_year != b->album_year) {
		return a->album_year < b->album_year ? -1 : 1;
	}

	c = strcasecmp(a->album, b->album);
	if(c) {
		return c;
	}

	if(a->disc != b->disc) {
		return a->disc < b->disc ? -1 : 1;
	}

	if(a->number != b->number) {
		return a->number < b->number ? -1 : 1;
	}

	return strcmp(a->path, b->path);
}

static bool same_album(const struct raw* a, const struct raw* b) {
	return !strcasecmp(a->album_artist, b->album_artist) && !strcasecmp(a->album, b->album);
}

// an album takes the earliest year its files state, albums without one sort last
static void album_years(struct raw* r, uint32_t n) {
	uint32_t i;
	uint32_t k;

	for(i = 0; i < n; i = k) {
		uint16_t year = NO_YEAR;

		for(k = i; k < n && same_album(&r[i], &r[k]); k++) {
			if(r[k].year && r[k].year < year) {
				year = r[k].year;
			}
		}

		for(k = i; k < n && same_album(&r[i], &r[k]); k++) {
			r[k].album_year = year;
		}
	}
}

static void group(struct library* lib, const struct raw* r, uint32_t n) {
	uint32_t albums = 0;
	uint32_t artists = 0;
	uint32_t i;

	for(i = 0; i < n; i++) {
		if(!i || strcasecmp(r[i].album_artist, r[i - 1].album_artist)) {
			artists++;
			albums++;
		} else if(!same_album(&r[i], &r[i - 1])) {
			albums++;
		}
	}

	lib->tracks = xrealloc(nullptr, (n + 1) * sizeof(*lib->tracks));
	lib->albums = xrealloc(nullptr, (albums + 1) * sizeof(*lib->albums));
	lib->artists = xrealloc(nullptr, (artists + 1) * sizeof(*lib->artists));
	lib->n_tracks = n;
	lib->n_albums = 0;
	lib->n_artists = 0;
	for(i = 0; i < n; i++) {
		bool new_artist = !i || strcasecmp(r[i].album_artist, r[i - 1].album_artist);

		if(new_artist) {
			lib->artists[lib->n_artists++] = (struct artist){ .name = r[i].album_artist, .first = lib->n_albums, .first_track = i };
		}

		if(new_artist || !same_album(&r[i], &r[i - 1])) {
			lib->albums[lib->n_albums++] = (struct album){
				.title = r[i].album,
				.artist = lib->n_artists - 1,
				.first = i,
				.year = r[i].album_year == NO_YEAR ? 0 : r[i].album_year,
			};
			lib->artists[lib->n_artists - 1].count++;
		}

		lib->albums[lib->n_albums - 1].count++;
		lib->artists[lib->n_artists - 1].tracks++;
		lib->tracks[i] = (struct track){
			.path = r[i].path,
			.title = r[i].title,
			.artist = r[i].artist,
			.album = lib->n_albums - 1,
			.duration = r[i].duration,
			.number = r[i].number,
			.disc = r[i].disc,
		};
	}
}

// reads the tags of the todo files of rs that are not ok, then drops the files that are not audio
static void read_files(struct raws* rs, struct arena* arena, uint32_t todo, lib_progress* progress) {
	struct worker workers[MAX_WORKERS];
	struct scan scan;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t n_workers = cpus > MAX_WORKERS ? MAX_WORKERS : (uint32_t)cpus;
	uint32_t good = 0;
	uint32_t i;

	if(cpus < 1) {
		n_workers = 1;
	}

	if(n_workers > todo) {
		n_workers = todo;
	}

	scan.raws = rs->v;
	scan.total = rs->n;
	atomic_init(&scan.next, 0);
	atomic_init(&scan.done, 0);
	for(i = 0; i < n_workers; i++) {
		workers[i].scan = &scan;
		workers[i].arena.head = nullptr;
		if(pthread_create(&workers[i].thread, nullptr, scan_thread, &workers[i])) {
			break;
		}
	}

	// the calling thread reads too when no worker started
	n_workers = i;
	if(!n_workers && todo) {
		struct worker self = { .scan = &scan, .arena = *arena };

		scan_thread(&self);
		*arena = self.arena;
	}

	while(progress && n_workers && atomic_load(&scan.done) < todo) {
		progress(atomic_load(&scan.done), todo);
		usleep(50000);
	}

	for(i = 0; i < n_workers; i++) {
		struct chunk* c;

		pthread_join(workers[i].thread, nullptr);
		c = workers[i].arena.head;
		while(c) {
			struct chunk* next = c->next;

			c->next = arena->head;
			arena->head = c;
			c = next;
		}
	}

	if(progress && todo) {
		progress(todo, todo);
	}

	for(i = 0; i < rs->n; i++) {
		if(rs->v[i].ok) {
			rs->v[good++] = rs->v[i];
		}
	}

	rs->n = good;
}

// the library takes the arena, rs is freed
static void finish(struct library* lib, struct raws* rs, struct arena* arena) {
	fill(rs->v, rs->n, arena);
	if(rs->n) {
		qsort(rs->v, rs->n, sizeof(*rs->v), cmp_raw);
		album_years(rs->v, rs->n);
		qsort(rs->v, rs->n, sizeof(*rs->v), cmp_raw);
	}

	group(lib, rs->v, rs->n);
	lib->memory = arena->head;
	free(rs->v);
}

void lib_scan(struct library* lib, char** paths, int n, lib_progress* progress) {
	struct arena arena = { nullptr };
	struct raws rs = { nullptr, 0, 0 };
	int k;

	*lib = (struct library){};
	for(k = 0; k < n; k++) {
		collect(&rs, &arena, paths[k], false);
	}

	read_files(&rs, &arena, rs.n, progress);
	finish(lib, &rs, &arena);
}

bool lib_open(struct library* lib, const char* db) {
	struct arena arena = { nullptr };
	struct raws rs = { nullptr, 0, 0 };
	enum db_result res = db_read(db, &rs, &arena, false);

	*lib = (struct library){};
	if(res == DB_ABSENT) {
		report("open %s: %s, use --build-db PATH to create it", db, strerror(ENOENT));
	} else if(res == DB_STALE) {
		report("%s: made by another version of nusic, use --build-db PATH to replace it", db);
	} else if(res == DB_OK && !rs.n) {
		report("%s: no tracks, use --build-db PATH to replace it", db);
	}

	if(res != DB_OK || !rs.n) {
		arena_free(&arena);
		free(rs.v);
		return false;
	}

	finish(lib, &rs, &arena);

	return true;
}

static int cmp_path(const void* a, const void* b) {
	return strcmp(((const struct raw*)a)->path, ((const struct raw*)b)->path);
}

// a file that old has with the same size and modification time takes its tags from there
// a file that two operands reach stays once, returns the number of files taken from old
static uint32_t reuse(struct raws* found, struct raws* old) {
	uint32_t kept = 0;
	uint32_t n = 0;
	uint32_t k = 0;
	uint32_t i;

	if(!found->n) {
		return 0;
	}

	qsort(found->v, found->n, sizeof(*found->v), cmp_path);
	if(old->n) {
		qsort(old->v, old->n, sizeof(*old->v), cmp_path);
	}

	for(i = 0; i < found->n; i++) {
		struct raw* r = &found->v[i];

		if(n && !strcmp(found->v[n - 1].path, r->path)) {
			continue;
		}

		while(k < old->n && strcmp(old->v[k].path, r->path) < 0) {
			k++;
		}

		if(k < old->n && old->v[k].mtime == r->mtime && old->v[k].size == r->size && !strcmp(old->v[k].path, r->path)) {
			*r = old->v[k];
			kept++;
		}

		found->v[n++] = *r;
	}

	found->n = n;

	return kept;
}

static void no_audio(char** paths, int n) {
	report("no audio files in %s%s", paths[0], n > 1 ? " and the other paths" : "");
}

bool lib_build_db(const char* db, char** paths, int n, lib_progress* progress, struct lib_build* out) {
	struct arena arena = { nullptr };
	struct raws rs = { nullptr, 0, 0 };
	struct raws old = { nullptr, 0, 0 };
	uint32_t kept;
	bool ok = true;
	int k;

	*out = (struct lib_build){};
	for(k = 0; k < n; k++) {
		if(!collect(&rs, &arena, paths[k], true)) {
			ok = false;
		}
	}

	// an operand that is mistyped or holds no audio would empty the database, so it stays as it is
	if(ok && !rs.n) {
		no_audio(paths, n);
		ok = false;
	}

	if(ok) {
		if(db_read(db, &old, &arena, true) != DB_OK) {
			old.n = 0;
		}

		kept = reuse(&rs, &old);
		out->read = rs.n - kept;
		out->kept = kept;
		read_files(&rs, &arena, rs.n - kept, progress);
		out->tracks = rs.n;
		if(rs.n) {
			ok = db_write(db, rs.v, rs.n);
		} else {
			no_audio(paths, n);
			ok = false;
		}
	}

	arena_free(&arena);
	free(rs.v);
	free(old.v);

	return ok;
}

void lib_free(struct library* lib) {
	struct arena arena = { lib->memory };

	arena_free(&arena);
	free(lib->tracks);
	free(lib->albums);
	free(lib->artists);
	*lib = (struct library){};
}
