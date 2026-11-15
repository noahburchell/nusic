#pragma once

#include <stddef.h>
#include <stdint.h>

// writes s at y, x in at most w columns, cut with two dots when it is wider
// pad fills the rest of the w columns with spaces, returns the columns written
int text(int y, int x, int w, const char* s, bool pad);
// like text without padding, but a string wider than w loses its start and keeps its end
int text_tail(int y, int x, int w, const char* s);
// m:ss, h:mm:ss from one hour on
void time_text(char* buf, size_t n, uint32_t ms);
