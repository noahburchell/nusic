#include "report.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

typedef void sink_fn(const char* line);

static _Atomic(sink_fn*) sink;

void report(const char* fmt, ...) {
	sink_fn* fn = atomic_load(&sink);
	char line[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if(fn) {
		fn(line);
	} else {
		fprintf(stderr, "nusic: %s\n", line);
	}
}

void report_sink(void (*fn)(const char* line)) {
	atomic_store(&sink, fn);
}
