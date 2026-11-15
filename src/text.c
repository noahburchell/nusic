#include "text.h"

#include <curses.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

// the next character of s and its width in columns, a byte that is not valid in the locale
// or does not print shows as a question mark
static int next_char(wchar_t* wc, const char** s, size_t* left, mbstate_t* st) {
	size_t n = mbrtowc(wc, *s, *left, st);
	int cw;

	if(n == (size_t)-1 || n == (size_t)-2 || !n) {
		memset(st, 0, sizeof(*st));
		*wc = L'?';
		n = 1;
	}

	cw = wcwidth(*wc);
	if(cw < 0) {
		*wc = L'?';
		cw = 1;
	}

	*s += n;
	*left -= n;

	return cw;
}

// writes s at y, x in at most w columns, cut with two dots when it is wider
// pad fills the rest of the w columns with spaces, returns the columns written
int text(int y, int x, int w, const char* s, bool pad) {
	wchar_t buf[256];
	mbstate_t st = { 0 };
	size_t left = strlen(s);
	bool cut = false;
	int used = 0;
	int len = 0;
	int i;

	if(w <= 0) {
		return 0;
	}

	while(left && len < 250) {
		int cw = next_char(&buf[len], &s, &left, &st);

		if(used + cw > w) {
			cut = true;
			break;
		}

		len++;
		used += cw;
	}

	if(cut && w >= 2) {
		while(len && used > w - 2) {
			used -= wcwidth(buf[--len]);
		}

		buf[len++] = L'.';
		buf[len++] = L'.';
		used += 2;
	}

	mvaddnwstr(y, x, buf, len);
	for(i = used; pad && i < w; i++) {
		addch(' ');
	}

	return pad ? w : used;
}

// like text without padding, but a string wider than w loses its start
// a diagnostic ends in the reason, which is the part worth keeping
int text_tail(int y, int x, int w, const char* s) {
	wchar_t buf[512];
	uint8_t width[512];
	mbstate_t st = { 0 };
	size_t left = strlen(s);
	int used = 0;
	int len = 0;
	int from = 0;

	while(left && len < 510) {
		width[len] = (uint8_t)next_char(&buf[len], &s, &left, &st);
		used += width[len++];
	}

	if(used <= w) {
		mvaddnwstr(y, x, buf, len);
		return used;
	}

	while(from < len && used > w - 2) {
		used -= width[from++];
	}

	mvaddstr(y, x, "..");
	addnwstr(&buf[from], len - from);

	return used + 2;
}

void time_text(char* buf, size_t n, uint32_t ms) {
	uint32_t s = ms / 1000;

	if(s >= 3600) {
		snprintf(buf, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(buf, n, "%u:%02u", s / 60, s % 60);
	}
}
