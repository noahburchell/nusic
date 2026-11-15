#include "library.h"

#include "report.h"

#include <dirent.h>
#include <errno.h>
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

struct arena {
	struct chunk* head;
};

struct paths {
	const char** v;
	uint32_t     n;
	uint32_t     cap;
};

struct raw {
	const char* path;
	const char* title;
	const char* artist;
	const char* album;
	const char* album_artist;
	uint32_t    duration;
	uint16_t    year;
	uint16_t    album_year;
	uint16_t    number;
	uint16_t    disc;
	bool        ok;
};

struct scan {
	const char** paths;
	struct raw*  raws;
	uint32_t     total;
	atomic_uint  next;
	atomic_uint  done;
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

static const char* arena_dup(struct arena* a, const char* s, size_t len) {
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

static void push(struct paths* ps, struct arena* a, const char* path) {
	if(ps->n == ps->cap) {
		ps->cap = ps->cap ? ps->cap * 2 : 1024;
		ps->v = xrealloc(ps->v, ps->cap * sizeof(*ps->v));
	}

	ps->v[ps->n++] = arena_dup(a, path, strlen(path));
}

// the depth limit ends a loop of symlinks
static void walk(struct paths* ps, struct arena* a, const char* dir, int depth) {
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

		if(e->d_name[0] == '.' || (size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path)) {
			continue;
		}

		if(type == DT_UNKNOWN || type == DT_LNK) {
			if(stat(path, &st) < 0) {
				continue;
			}

			type = DT_UNKNOWN;
			if(S_ISDIR(st.st_mode)) {
				type = DT_DIR;
			} else if(S_ISREG(st.st_mode)) {
				type = DT_REG;
			}
		}

		if(type == DT_DIR && depth < MAX_DEPTH) {
			walk(ps, a, path, depth + 1);
		} else if(type == DT_REG && is_audio(e->d_name)) {
			push(ps, a, path);
		}
	}

	closedir(d);
}

static void collect(struct paths* ps, struct arena* a, const char* operand) {
	char path[PATH_MAX];
	size_t len = strlen(operand);
	struct stat st;

	while(len > 1 && operand[len - 1] == '/') {
		len--;
	}

	if(len >= sizeof(path)) {
		report("%s: path is too long", operand);
		return;
	}

	memcpy(path, operand, len);
	path[len] = 0;
	if(stat(path, &st) < 0) {
		report("open %s: %s", path, strerror(errno));
	} else if(S_ISDIR(st.st_mode)) {
		walk(ps, a, path, 0);
	} else {
		push(ps, a, path);
	}
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

// the header of the file holds the tags, probing the streams is the slow path taken
// only when the header names no audio stream or no duration
static void read_tags(struct raw* t, const char* path, struct arena* a) {
	AVFormatContext* f = nullptr;
	const AVStream* st;
	const char* name = strrchr(path, '/');
	const char* dot;
	const char* v;
	int64_t us;

	if(avformat_open_input(&f, path, nullptr, nullptr) < 0) {
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

	name = name ? name + 1 : path;
	dot = strrchr(name, '.');
	v = tag(f, st, "title");
	t->title = v ? arena_dup(a, v, strlen(v)) : arena_dup(a, name, dot && dot > name ? (size_t)(dot - name) : strlen(name));
	v = tag(f, st, "artist");
	t->artist = v ? arena_dup(a, v, strlen(v)) : unknown_artist;
	v = tag(f, st, "album");
	t->album = v ? arena_dup(a, v, strlen(v)) : unknown_album;
	v = tag(f, st, "album_artist");
	if(!v) {
		v = tag(f, st, "album artist");
	}

	t->album_artist = v ? arena_dup(a, v, strlen(v)) : t->artist;
	v = tag(f, st, "date");
	if(!v) {
		v = tag(f, st, "year");
	}

	t->year = number(v);
	t->number = number(tag(f, st, "track"));
	t->disc = number(tag(f, st, "disc"));
	t->duration = (uint32_t)(us / 1000);
	t->path = path;
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

		read_tags(&s->raws[i], s->paths[i], &w->arena);
		atomic_fetch_add(&s->done, 1);
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

static void build(struct library* lib, const struct raw* r, uint32_t n) {
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

void lib_load(struct library* lib, char** paths, int n, void (*progress)(uint32_t done, uint32_t total)) {
	struct worker workers[MAX_WORKERS];
	struct arena arena = { nullptr };
	struct paths ps = { nullptr, 0, 0 };
	struct scan scan;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	uint32_t n_workers = cpus > MAX_WORKERS ? MAX_WORKERS : (uint32_t)cpus;
	uint32_t good = 0;
	uint32_t i;
	int k;

	*lib = (struct library){};
	if(cpus < 1) {
		n_workers = 1;
	}

	for(k = 0; k < n; k++) {
		collect(&ps, &arena, paths[k]);
	}

	scan.paths = ps.v;
	scan.total = ps.n;
	scan.raws = xrealloc(nullptr, (ps.n ? ps.n : 1) * sizeof(*scan.raws));
	memset(scan.raws, 0, ps.n * sizeof(*scan.raws));
	atomic_init(&scan.next, 0);
	atomic_init(&scan.done, 0);
	if(n_workers > ps.n) {
		n_workers = ps.n;
	}

	for(i = 0; i < n_workers; i++) {
		workers[i].scan = &scan;
		workers[i].arena.head = nullptr;
		if(pthread_create(&workers[i].thread, nullptr, scan_thread, &workers[i])) {
			break;
		}
	}

	// the calling thread reads too when no worker started
	n_workers = i;
	if(!n_workers && ps.n) {
		struct worker self = { .scan = &scan, .arena = arena };

		scan_thread(&self);
		arena = self.arena;
	}

	while(progress && n_workers && atomic_load(&scan.done) < scan.total) {
		progress(atomic_load(&scan.done), scan.total);
		usleep(50000);
	}

	for(i = 0; i < n_workers; i++) {
		struct chunk* c;

		pthread_join(workers[i].thread, nullptr);
		c = workers[i].arena.head;
		while(c) {
			struct chunk* next = c->next;

			c->next = arena.head;
			arena.head = c;
			c = next;
		}
	}

	for(i = 0; i < ps.n; i++) {
		if(scan.raws[i].ok) {
			scan.raws[good++] = scan.raws[i];
		}
	}

	qsort(scan.raws, good, sizeof(*scan.raws), cmp_raw);
	album_years(scan.raws, good);
	qsort(scan.raws, good, sizeof(*scan.raws), cmp_raw);
	build(lib, scan.raws, good);
	lib->memory = arena.head;
	free(scan.raws);
	free(ps.v);
}

void lib_free(struct library* lib) {
	struct chunk* c = lib->memory;

	while(c) {
		struct chunk* next = c->next;

		free(c);
		c = next;
	}

	free(lib->tracks);
	free(lib->albums);
	free(lib->artists);
	*lib = (struct library){};
}
