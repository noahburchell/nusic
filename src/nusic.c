#include <curses.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <pipewire/pipewire.h>
#include <stdio.h>
#include <string.h>

static void usage(void) {
	printf("usage: nusic [OPTION]...\n"
	       "\n"
	       "      --help    display this help and exit\n"
	       "      --version display the nusic and library versions and exit\n");
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

int main(int argc, char** argv) {
	const char* a;

	if(argc < 2) {
		return 0;
	}

	a = argv[1];
	if(!strcmp(a, "--help")) {
		usage();
		return 0;
	}

	if(!strcmp(a, "--version")) {
		version();
		return 0;
	}

	if(a[0] == '-' && a[1]) {
		fprintf(stderr, "nusic: unknown option '%s'\n", a);
	} else {
		fprintf(stderr, "nusic: unexpected operand '%s'\n", a);
	}

	try_help();

	return 2;
}
