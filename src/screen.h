#pragma once

#include "library.h"
#include "player.h"

#include <curses.h>
#include <pthread.h>
#include <time.h>

#define ACCENT COLOR_PAIR(1)

enum {
	ROW_ALBUM,
	ROW_TRACK,
	ROW_SPACE,
};

// in the order they sit on the screen
enum pane {
	PANE_ARTISTS,
	PANE_PAGE,
	PANE_QUEUE,
	PANES,
};

// every glyph is in code page 437, so the linux console font has it
struct glyphs {
	const char* prev;
	const char* next;
	const char* play;
	const char* pause;
	const char* now;
	const char* select;
	const char* elapsed;
	const char* knob;
	const char* repeat;
};

struct row {
	uint32_t id;
	uint8_t  kind;
};

struct list {
	int top;
	int sel;
	int count;
};

// side is the inner width of the artists pane, rows the height of every pane
// the page and the queue share the main_w columns from main_x on, side by side when split,
// otherwise only tab shows, x and w give every pane its columns either way
// the hit boxes of the titles and of the bar are set when they are drawn
struct screen {
	const struct library* lib;
	const struct glyphs*  g;
	struct row*           page_rows;
	struct play_status    s;
	struct list           lists[PANES];
	pthread_mutex_t       msg_lock;
	char                  msg_in[256];
	char                  msg[256];
	struct timespec       click_time;
	int                   click_row;
	int                   notify;
	int                   w;
	int                   h;
	int                   side;
	int                   main_x;
	int                   main_w;
	int                   rows;
	int                   x[PANES];
	int                   pane_w[PANES];
	int                   title_x[PANES];
	int                   title_end[PANES];
	int                   prev_x;
	int                   play_x;
	int                   next_x;
	int                   repeat_x;
	int                   bar_x;
	int                   bar_w;
	uint32_t              bar_ms;
	enum pane             focus;
	enum pane             tab;
	enum pane             click_pane;
	bool                  split;
	bool                  quit;
};

extern struct screen ui;

int clamp(int v, int lo, int hi);
bool too_small(void);
bool shown(enum pane p);
// draws everything, draw_bar only the play bar
void draw(void);
void draw_bar(void);
