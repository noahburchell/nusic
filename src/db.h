#pragma once

#include "scan.h"

enum db_result {
	DB_OK,
	DB_ABSENT,
	DB_STALE,
	DB_FAILED,
};

// $XDG_DATA_HOME/nusic/library.db, false when the environment names no such place, which is reported
bool db_path(char* buf, size_t n);
// appends the tracks of the database to rs, DB_STALE is a database of another version of nusic
// a failure is reported, as a warning when the database is about to be replaced
enum db_result db_read(const char* path, struct raws* rs, struct arena* a, bool replacing);
// replaces the database with the n tracks of r, a failure is reported and leaves the database as it was
bool db_write(const char* path, const struct raw* r, uint32_t n);
