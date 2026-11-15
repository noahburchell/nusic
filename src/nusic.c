#include "library.h"
#include "player.h"
#include "ui.h"

#include <curses.h>
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <locale.h>
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

static void usage(void) {
	printf("usage: nusic [OPTION]... [PATH]...\n"
	       "\n"
	       "      --help    display this help and exit\n"
	       "      --version display the nusic and library versions and exit\n"
	       "\n"
	       "keys:\n"
	       "  up, C-p         move up\n"
	       "  down, C-n       move down\n"
	       "  right, C-f      go to the tracks of the artist\n"
	       "  left, C-b       go back to the artists\n"
	       "  tab             switch between artists and tracks\n"
	       "  enter           open the artist, play the track or the album\n"
	       "  pgdn, C-v       move down a page\n"
	       "  pgup, M-v       move up a page\n"
	       "  home, M-<       move to the first row\n"
	       "  end, M->        move to the last row\n"
	       "  space           pause or resume\n"
	       "  <, >            previous track, next track\n"
	       "  ,  .            seek back 5 s, seek forward 5 s\n"
	       "  C-l             redraw the screen\n"
	       "  q, C-c          quit\n"
	       "\n"
	       "mouse:\n"
	       "  click           select a row, press a button of the play bar\n"
	       "  double click    play the track or the album\n"
	       "  wheel           scroll the list under the pointer\n"
	       "  progress bar    click to seek\n"
	       "\n"
	       "PATH is an audio file or a directory searched for audio files\n"
	       "PATH defaults to ~/Music\n");
}

static void try_help(void) {
	fprintf(stderr, "try 'nusic --help' for more information\n");
}

static void av_line(const char* name, unsigned v) {
	printf("%s %u.%u.%u\n", name, AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v));
}

static void version(void) {
	printf("nusic %s\n%s\npipewire %s\n", PACKAGE_VERSION, curses_version(), pw_get_library_version());
	av_line("libavformat", avformat_version());
	av_line("libavcodec", avcodec_version());
	av_line("libswresample", swresample_version());
	av_line("libavutil", avutil_version());
}

static void progress(uint32_t done, uint32_t total) {
	fprintf(stderr, "\rnusic: reading tags, %u of %u files", done, total);
}

int main(int argc, char** argv) {
	char home_music[4096];
	char* fallback[] = { home_music };
	char** paths = argv + 1;
	struct library lib;
	const char* home = getenv("HOME");
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

	if(!isatty(0) || !isatty(1)) {
		fprintf(stderr, "nusic: standard input and standard output must be a terminal\n");
		return 1;
	}

	if(!n) {
		if(!home || (size_t)snprintf(home_music, sizeof(home_music), "%s/Music", home) >= sizeof(home_music)) {
			fprintf(stderr, "nusic: HOME is not set, no default PATH\n");
			try_help();
			return 2;
		}

		paths = fallback;
		n = 1;
	}

	setlocale(LC_ALL, "");
	av_log_set_level(AV_LOG_QUIET);
	lib_load(&lib, paths, n, tty ? progress : nullptr);
	if(tty) {
		fprintf(stderr, "\r\033[K");
	}

	if(!lib.n_tracks) {
		fprintf(stderr, "nusic: no audio files in %s%s\n", paths[0], n > 1 ? " and the other paths" : "");
		lib_free(&lib);
		return 1;
	}

	notify = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if(notify < 0) {
		fprintf(stderr, "nusic: eventfd: %s\n", strerror(errno));
		lib_free(&lib);
		return 1;
	}

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
