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
// ctl_x is where the gain and the speed start, 0 when they do not show, knob_w is 0 without the sliders
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
	int                   ctl_x;
	int                   knob_x;
	int                   knob_w;
	int                   value_x;
	uint32_t              bar_ms;
	enum pane             focus;
	enum pane             tab;
	enum pane             click_pane;
	bool                  split;
	bool                  semitones;
	bool                  quit;
};

extern struct screen ui;

int clamp(int v, int lo, int hi);
bool too_small(void);
bool shown(enum pane p);
// the column of a slider that stands for v on the scale from min to max
int knob_at(int v, int min, int max);
// that of the speed, the scale is the unit that shows, 1.00x is a third along it and 0 st in its middle
int speed_knob(void);
// draws everything, draw_bar only the play bar
void draw(void);
void draw_bar(void);
