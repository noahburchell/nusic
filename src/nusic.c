#include "decode.h"
#include "output.h"

#include <curses.h>
#include <libavutil/avutil.h>
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <string.h>

static void usage(void) {
	printf("usage: nusic [OPTION]... FILE...\n"
	       "\n"
	       "      --help    display this help and exit\n"
	       "      --version display the nusic and library versions and exit\n"
	       "\n"
	       "FILE is an audio file in any format ffmpeg decodes\n"
	       "the files play in order through pipewire\n");
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

static bool play(const char* path) {
	struct decoder d;
	uint8_t* dst;
	uint32_t room;
	bool ok;

	if(!dec_open(&d, path)) {
		return false;
	}

	printf("%s: %s, %u Hz, %u %s, %s\n", path, avcodec_get_name(d.codec->codec_id), d.format.rate, d.format.channels,
	       d.format.channels == 1 ? "channel" : "channels", av_get_sample_fmt_name(d.out_fmt));
	do {
		if(!out_set_format(&d.format, d.stride)) {
			break;
		}

		room = out_wait(&dst);
		if(!room) {
			break;
		}

		out_commit(dec_read(&d, dst, room));
	} while(dec_next(&d));

	ok = !d.failed;
	dec_close(&d);

	return ok;
}

int main(int argc, char** argv) {
	char** files = argv + 1;
	bool options = true;
	int status = 0;
	int n = 0;
	int k;

	for(k = 1; k < argc; k++) {
		const char* a = argv[k];

		if(!options || a[0] != '-' || !a[1]) {
			files[n++] = argv[k];
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

	if(!n) {
		fprintf(stderr, "nusic: missing file operand\n");
		try_help();
		return 2;
	}

	setvbuf(stdout, nullptr, _IOLBF, 0);
	av_log_set_level(AV_LOG_QUIET);
	if(!out_init()) {
		out_fini();
		return 1;
	}

	for(k = 0; k < n && !out_failed(); k++) {
		if(!play(files[k])) {
			status = 1;
		}
	}

	out_drain();
	if(out_failed()) {
		status = 1;
	}

	out_fini();

	return status;
}
