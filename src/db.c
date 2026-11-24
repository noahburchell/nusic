#include "db.h"

#include "path.h"
#include "report.h"

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// changes with the table and with what a column means, a database with another number is not read
#define SCHEMA  1
#define STR_(x) #x
#define STR(x)  STR_(x)

// the file is written in full and renamed over the database, so it needs no journal
static const char begin[] = "PRAGMA journal_mode = OFF; BEGIN; PRAGMA user_version = " STR(SCHEMA);
static const char create[] = "CREATE TABLE track("
                             "path TEXT NOT NULL,"
                             "mtime INTEGER NOT NULL,"
                             "size INTEGER NOT NULL,"
                             "title TEXT,"
                             "artist TEXT,"
                             "album TEXT,"
                             "album_artist TEXT,"
                             "year INTEGER NOT NULL,"
                             "disc INTEGER NOT NULL,"
                             "number INTEGER NOT NULL,"
                             "duration INTEGER NOT NULL"
                             ") STRICT";

static const char select_tracks[] = "SELECT path, mtime, size, title, artist, album, album_artist, year, disc, number, duration FROM track";
static const char insert_track[] = "INSERT INTO track VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

bool db_path(char* buf, size_t n) {
	return nusic_path(buf, n, "XDG_DATA_HOME", ".local/share", "library.db", "the database");
}

static void fail(sqlite3* db, const char* path, bool replacing) {
	if(replacing) {
		report("warning: %s: %s, reading every file", path, sqlite3_errmsg(db));
	} else {
		report("%s: %s", path, sqlite3_errmsg(db));
	}
}

// -1 when the file cannot be read as a database
static int schema_of(sqlite3* db) {
	sqlite3_stmt* st = nullptr;
	int version = -1;

	if(sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
		version = sqlite3_column_int(st, 0);
	}

	sqlite3_finalize(st);

	return version;
}

static const char* text(sqlite3_stmt* st, int i, struct arena* a) {
	const char* s = (const char*)sqlite3_column_text(st, i);

	return s && s[0] ? arena_dup(a, s, (size_t)sqlite3_column_bytes(st, i)) : nullptr;
}

static bool read_tracks(sqlite3* db, struct raws* rs, struct arena* a) {
	sqlite3_stmt* st = nullptr;
	int rc;

	if(sqlite3_prepare_v2(db, select_tracks, -1, &st, nullptr) != SQLITE_OK) {
		return false;
	}

	while((rc = sqlite3_step(st)) == SQLITE_ROW) {
		struct raw* r = raws_push(rs);

		r->path = text(st, 0, a);
		r->mtime = sqlite3_column_int64(st, 1);
		r->size = sqlite3_column_int64(st, 2);
		r->title = text(st, 3, a);
		r->artist = text(st, 4, a);
		r->album = text(st, 5, a);
		r->album_artist = text(st, 6, a);
		r->year = (uint16_t)sqlite3_column_int(st, 7);
		r->disc = (uint16_t)sqlite3_column_int(st, 8);
		r->number = (uint16_t)sqlite3_column_int(st, 9);
		r->duration = (uint32_t)sqlite3_column_int64(st, 10);
		r->ok = true;
		// only a database that nusic did not write has a row without a path
		if(!r->path) {
			rs->n--;
		}
	}

	sqlite3_finalize(st);

	return rc == SQLITE_DONE;
}

enum db_result db_read(const char* path, struct raws* rs, struct arena* a, bool replacing) {
	sqlite3* db = nullptr;
	enum db_result res = DB_OK;
	uint32_t before = rs->n;
	int version = -1;

	if(access(path, F_OK) < 0 && errno == ENOENT) {
		return DB_ABSENT;
	}

	if(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
		version = schema_of(db);
	}

	if(version < 0) {
		res = DB_FAILED;
	} else if(version != SCHEMA) {
		res = DB_STALE;
	} else if(!read_tracks(db, rs, a)) {
		res = DB_FAILED;
	}

	if(res == DB_FAILED) {
		fail(db, path, replacing);
		rs->n = before;
	}

	sqlite3_close(db);

	return res;
}

static bool write_tracks(sqlite3* db, const struct raw* r, uint32_t n) {
	sqlite3_stmt* st = nullptr;
	bool ok = sqlite3_prepare_v2(db, insert_track, -1, &st, nullptr) == SQLITE_OK;
	uint32_t i;

	// the strings outlive the statement, so sqlite does not copy them
	for(i = 0; ok && i < n; i++) {
		sqlite3_bind_text(st, 1, r[i].path, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 2, r[i].mtime);
		sqlite3_bind_int64(st, 3, r[i].size);
		sqlite3_bind_text(st, 4, r[i].title, -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 5, r[i].artist, -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 6, r[i].album, -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 7, r[i].album_artist, -1, SQLITE_STATIC);
		sqlite3_bind_int(st, 8, r[i].year);
		sqlite3_bind_int(st, 9, r[i].disc);
		sqlite3_bind_int(st, 10, r[i].number);
		sqlite3_bind_int64(st, 11, r[i].duration);
		ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_reset(st) == SQLITE_OK;
	}

	sqlite3_finalize(st);

	return ok && sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool db_write(const char* path, const struct raw* r, uint32_t n) {
	char tmp[PATH_MAX + 8];
	sqlite3* db = nullptr;
	bool ok;

	if((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp)) {
		report("%s: path is too long", path);
		return false;
	}

	if(!make_parents(tmp)) {
		report("mkdir %s: %s", tmp, strerror(errno));
		return false;
	}

	// a build that did not finish leaves its file behind
	unlink(tmp);
	ok = sqlite3_open_v2(tmp, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK
	     && sqlite3_exec(db, begin, nullptr, nullptr, nullptr) == SQLITE_OK
	     && sqlite3_exec(db, create, nullptr, nullptr, nullptr) == SQLITE_OK
	     && write_tracks(db, r, n);
	if(!ok) {
		fail(db, tmp, false);
	}

	sqlite3_close(db);
	if(ok && rename(tmp, path) < 0) {
		report("rename %s: %s", tmp, strerror(errno));
		ok = false;
	}

	if(!ok) {
		unlink(tmp);
	}

	return ok;
}
