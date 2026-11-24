#include "db.h"
#include "library.h"
#include "player.h"
#include "ui.h"

#include <curses.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <limits.h>
#include <locale.h>
#include <pipewire/pipewire.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void usage(void) {
	printf("usage: nusic [OPTION]... [PATH]...\n"
	       "\n"
	       "  -b, --build-db  build the database from the audio files under PATH and exit\n"
	       "      --help      display this help and exit\n"
	       "      --version   display the nusic and library versions and exit\n"
	       "\n"
	       "keys:\n"
	       "  up, C-p         move up\n"
	       "  down, C-n       move down\n"
	       "  right, C-f      go to the pane on the right\n"
	       "  left, C-b       go to the pane on the left\n"
	       "  tab, S-tab      go to the next pane, the previous pane\n"
	       "  enter           open the artist, play the queue entry,\n"
	       "                  put the track or the album on top of the queue and play it\n"
	       "  pgdn, C-v       move down a page\n"
	       "  pgup, M-v       move up a page\n"
	       "  home, M-<       move to the first row\n"
	       "  end, M->        move to the last row\n"
	       "  a               add the track, the album or the artist to the end of the queue\n"
	       "  space           pause or resume, start the queue when nothing plays\n"
	       "  <, >            previous track, next track\n"
	       "  ,  .            seek back 5 s, seek forward 5 s\n"
	       "  r               repeat the queue, then the track, then nothing\n"
	       "  -, =            lower, raise the gain by 1 dB, above 0 dB the loudest parts clip\n"
	       "  [, ]            lower, raise the rate by 0.05x, speed and pitch move together\n"
	       "  0, 1            set the gain to 0 dB, the rate to 1.00x\n"
	       "  C-l             redraw the screen\n"
	       "  q, C-c          quit\n"
	       "\n"
	       "keys with the focus on the queue pane:\n"
	       "  M-p, M-n        move the queue entry up, down, as do the up and down arrows with control or meta\n"
	       "  del, C-k        remove the queue entry\n"
	       "  M-k             clear the queue and stop\n"
	       "\n"
	       "mouse:\n"
	       "  click           select a row, go to the pane of a title, press a button of the play bar\n"
	       "  double click    the same as enter on a track, an album or a queue entry\n"
	       "  right click     add the track, the album or the artist to the end of the queue\n"
	       "  wheel           scroll the list under the pointer\n"
	       "  progress bar    click to seek\n"
	       "  sliders         click beside the knob or use the wheel to move the gain by 1 dB\n"
	       "                  or the rate by 0.05x, right click to set it to 0 dB or 1.00x\n"
	       "\n"
	       "the gain goes from -40 dB to 24 dB, the rate from 0.50x to 2.00x\n"
	       "\n"
	       "the panes are the album artists, the albums of the selected artist and the queue\n"
	       "a terminal under 120 columns shows the albums and the queue one at a time\n"
	       "\n"
	       "other programs read what plays and control it through the mpris interface of the session bus\n"
	       "\n"
	       "PATH is an audio file or a directory searched for audio files\n"
	       "without PATH the library is the database, with PATH it is the files under PATH\n"
	       "--build-db replaces what the database held, a file that has not changed is not read again\n"
	       "the database is $XDG_DATA_HOME/nusic/library.db, XDG_DATA_HOME defaults to ~/.local/share\n");
}

static void try_help(void) {
	fprintf(stderr, "try 'nusic --help' for more information\n");
}

static void av_line(const char* name, unsigned v) {
	printf("%s %u.%u.%u\n", name, AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v));
}

static void version(void) {
	int major;
	int minor;
	int micro;

	dbus_get_version(&major, &minor, &micro);
	printf("nusic %s\n%s\npipewire %s\nsqlite %s\ndbus %d.%d.%d\n", PACKAGE_VERSION, curses_version(), pw_get_library_version(), sqlite3_libversion(), major, minor, micro);
	av_line("libavformat", avformat_version());
	av_line("libavcodec", avcodec_version());
	av_line("libswresample", swresample_version());
	av_line("libavutil", avutil_version());
}

// the line goes once every file is read
static void progress(uint32_t done, uint32_t total) {
	if(done < total) {
		fprintf(stderr, "\rnusic: reading tags, %u of %u files", done, total);
	} else {
		fprintf(stderr, "\r\033[K");
	}
}

static const char* plural(uint32_t n) {
	return n == 1 ? "" : "s";
}

static int build_db(char** paths, int n, bool tty) {
	char db[PATH_MAX];
	struct lib_build b;

	if(!db_path(db, sizeof(db)) || !lib_build_db(db, paths, n, tty ? progress : nullptr, &b)) {
		return 1;
	}

	printf("%s: %u track%s, %u file%s read, %u unchanged", db, b.tracks, plural(b.tracks), b.read, plural(b.read), b.kept);
	if(b.read + b.kept > b.tracks) {
		printf(", %u not audio", b.read + b.kept - b.tracks);
	}

	putchar('\n');

	return 0;
}

int main(int argc, char** argv) {
	char db[PATH_MAX];
	char** paths = argv + 1;
	struct library lib;
	const char* build = nullptr;
	bool options = true;
	bool tty = isatty(2);
	int n = 0;
	int notify;
	int k;

	for(k = 1; k < argc; k++) {
		const char* a = argv[k];

		if(!options || a[0] != '-' || !a[1]) {
			paths[n++] = argv[k];
		} else if(!strcmp(a, "--")) {
			options = false;
		} else if(!strcmp(a, "-b") || !strcmp(a, "--build-db")) {
			build = a;
		} else if(!strcmp(a, "--help")) {
			usage();
			return 0;
		} else if(!strcmp(a, "--version")) {
			version();
			return 0;
		} else {
			fprintf(stderr, "nusic: unknown option '%s'\n", a);
			try_help();
			return 2;
		}
	}

	if(build && !n) {
		fprintf(stderr, "nusic: '%s' requires a PATH\n", build);
		try_help();
		return 2;
	}

	setlocale(LC_ALL, "");
	av_log_set_level(AV_LOG_QUIET);
	if(build) {
		return build_db(paths, n, tty);
	}

	if(!isatty(0) || !isatty(1)) {
		fprintf(stderr, "nusic: standard input and standard output must be a terminal\n");
		return 1;
	}

	if(!n) {
		if(!db_path(db, sizeof(db)) || !lib_open(&lib, db)) {
			return 1;
		}
	} else {
		lib_scan(&lib, paths, n, tty ? progress : nullptr);
		if(!lib.n_tracks) {
			fprintf(stderr, "nusic: no audio files in %s%s\n", paths[0], n > 1 ? " and the other paths" : "");
			lib_free(&lib);
			return 1;
		}
	}

	notify = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if(notify < 0) {
		fprintf(stderr, "nusic: eventfd: %s\n", strerror(errno));
		lib_free(&lib);
		return 1;
	}

	// ncurses takes LINES and COLUMNS over the size of the terminal and then never follows a resize
	// bash exports both, they go before the first thread starts
	unsetenv("LINES");
	unsetenv("COLUMNS");
	if(!player_init(notify)) {
		lib_free(&lib);
		return 1;
	}

	ui_run(&lib, notify);
	player_fini();
	lib_free(&lib);
	close(notify);

	return 0;
}
