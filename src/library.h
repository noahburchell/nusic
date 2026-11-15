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

// reads the tags of the audio files in paths, a path is a file or a directory searched in full
// progress runs on the calling thread while the files are read
void lib_load(struct library* lib, char** paths, int n, void (*progress)(uint32_t done, uint32_t total));
void lib_free(struct library* lib);
