#pragma once

// one diagnostic, without the program name and without a newline
// goes to stderr as nusic: MESSAGE until a sink is set, then to the sink
void report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// any thread may report, so fn runs on any thread, nullptr restores stderr
void report_sink(void (*fn)(const char* line));
