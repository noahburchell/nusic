#pragma once

#include <stdint.h>

// tracks are sorted by album artist, album year, album, disc and number,
// so the tracks of an album and the albums and tracks of an artist are contiguous
struct track {
	const char* path;
	const char* title;
	const char* artist;
	uint32_t    album;
	uint32_t    duration;
	uint16_t    number;
	uint16_t    disc;
};

struct album {
	const char* title;
	uint32_t    artist;
	uint32_t    first;
	uint32_t    count;
	uint16_t    year;
};

struct artist {
	const char* name;
	uint32_t    first;
	uint32_t    count;
	uint32_t    first_track;
	uint32_t    tracks;
};

struct chunk;

// duration is in ms, year is 0 when no file of the album states one
// first and count of an artist index albums, of an album tracks
struct library {
	struct track*  tracks;
	struct album*  albums;
	struct artist* artists;
	struct chunk*  memory;
	uint32_t       n_tracks;
	uint32_t       n_albums;
	uint32_t       n_artists;
};

// tracks is what the database holds now, read the files whose tags were read
// kept the files taken from the database as it was
struct lib_build {
	uint32_t tracks;
	uint32_t read;
	uint32_t kept;
};

typedef void lib_progress(uint32_t done, uint32_t total);

// the library is the audio files in paths, a path is a file or a directory searched in full
// progress runs on the calling thread while the files are read, and once with done equal to total
void lib_scan(struct library* lib, char** paths, int n, lib_progress* progress);
// the library is the database at db, false when it has no tracks to give, which is reported
bool lib_open(struct library* lib, const char* db);
// replaces the database at db with the audio files in paths, a file it has with the same
// size and modification time is not read again, a failure is reported and leaves it as it was
bool lib_build_db(const char* db, char** paths, int n, lib_progress* progress, struct lib_build* out);
void lib_free(struct library* lib);
