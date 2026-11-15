#include "ui.h"

#include "mpris.h"
#include "player.h"
#include "report.h"
#include "screen.h"

#include <curses.h>
#include <fcntl.h>
#include <langinfo.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MIN_W     40
#define MIN_H     9
#define SPLIT_W   120
#define SEEK_MS   5000
#define WHEEL     3
#define DOUBLE_MS 400

// control or meta with an arrow has no key code of its own, these stand for it
#define KEY_ENTRY_UP   (KEY_MAX + 1000)
#define KEY_ENTRY_DOWN (KEY_MAX + 1001)

static const struct glyphs cp437 = { "◄◄", "►►", "► ", "▌▌", "►", "▌", "═", "■", "∞" };
static const struct glyphs ascii = { "<<", ">>", "> ", "||", ">", "|", "=", "#", "R" };

struct screen ui = { .msg_lock = PTHREAD_MUTEX_INITIALIZER, .tab = PANE_PAGE };

// runs on any thread, a line that does not fit loses its start and keeps the reason at its end
static void on_report(const char* line) {
	uint64_t one = 1;
	size_t len = strlen(line);
	size_t skip = len < sizeof(ui.msg_in) ? 0 : len - (sizeof(ui.msg_in) - 1);
	ssize_t r;

	// the cut does not fall inside a character
	while(skip && ((unsigned char)line[skip] & 0xc0) == 0x80) {
		skip++;
	}

	pthread_mutex_lock(&ui.msg_lock);
	memcpy(ui.msg_in, line + skip, len - skip + 1);
	pthread_mutex_unlock(&ui.msg_lock);
	r = write(ui.notify, &one, sizeof(one));
	(void)r;
}

int clamp(int v, int lo, int hi) {
	if(v < lo) {
		return lo;
	}

	return v > hi ? hi : v;
}

// the page and the queue sit side by side from SPLIT_W columns on, each then has 41 or more
static void layout(void) {
	int half;

	getmaxyx(stdscr, ui.h, ui.w);
	ui.side = clamp(ui.w * 28 / 100, 16, 34);
	ui.main_x = ui.side + 2;
	ui.main_w = ui.w - 1 - ui.main_x;
	ui.rows = ui.h - 5;
	half = (ui.main_w - 1) / 2;
	ui.split = ui.w >= SPLIT_W;
	ui.x[PANE_ARTISTS] = 1;
	ui.pane_w[PANE_ARTISTS] = ui.side;
	ui.x[PANE_PAGE] = ui.main_x;
	ui.pane_w[PANE_PAGE] = ui.split ? half : ui.main_w;
	ui.x[PANE_QUEUE] = ui.split ? ui.main_x + half + 1 : ui.main_x;
	ui.pane_w[PANE_QUEUE] = ui.split ? ui.main_w - half - 1 : ui.main_w;
}

bool too_small(void) {
	return ui.w < MIN_W || ui.h < MIN_H;
}

bool shown(enum pane p) {
	return p == PANE_ARTISTS || ui.split || p == ui.tab;
}

// the pane with the focus always shows, so it becomes the tab
static void focus(enum pane p) {
	ui.focus = p;
	if(p != PANE_ARTISTS) {
		ui.tab = p;
	}
}

static void show(struct list* l) {
	if(l->sel < l->top) {
		l->top = l->sel;
	} else if(l->sel >= l->top + ui.rows) {
		l->top = l->sel - ui.rows + 1;
	}
}

static void scroll_by(struct list* l, int n) {
	l->top = clamp(l->top + n, 0, l->count > ui.rows ? l->count - ui.rows : 0);
}

// the rows of the selected artist: every album as a heading, its tracks and a blank row
static void build_page(void) {
	const struct artist* a = &ui.lib->artists[ui.lists[PANE_ARTISTS].sel];
	struct row* r = realloc(ui.page_rows, (a->tracks + 2 * a->count) * sizeof(*r));
	uint32_t i;
	uint32_t k;
	int n = 0;

	if(!r) {
		return;
	}

	for(i = a->first; i < a->first + a->count; i++) {
		const struct album* al = &ui.lib->albums[i];

		r[n++] = (struct row){ i, ROW_ALBUM };
		for(k = al->first; k < al->first + al->count; k++) {
			r[n++] = (struct row){ k, ROW_TRACK };
		}

		r[n++] = (struct row){ 0, ROW_SPACE };
	}

	ui.page_rows = r;
	ui.lists[PANE_PAGE] = (struct list){ 0, 1, n - 1 };
	ui.click_row = -1;
}

static void select_artist(int i) {
	struct list* l = &ui.lists[PANE_ARTISTS];

	i = clamp(i, 0, l->count - 1);
	if(i != l->sel) {
		l->sel = i;
		build_page();
	}

	show(l);
}

// a blank row is skipped in the direction of travel
static void select_row(int i, int dir) {
	struct list* l = &ui.lists[PANE_PAGE];

	i = clamp(i, 0, l->count - 1);
	while(ui.page_rows[i].kind == ROW_SPACE) {
		i += dir;
	}

	l->sel = i;
	show(l);
	// the heading of an album stays in view above its first track
	if(i && l->top == i && ui.page_rows[i - 1].kind == ROW_ALBUM) {
		l->top--;
	}
}

static void select_entry(int i) {
	struct list* l = &ui.lists[PANE_QUEUE];

	if(l->count) {
		l->sel = clamp(i, 0, l->count - 1);
		show(l);
	}
}

static void go(int i, int dir) {
	if(ui.focus == PANE_PAGE) {
		select_row(i, dir);
	} else if(ui.focus == PANE_QUEUE) {
		select_entry(i);
	} else {
		select_artist(i);
	}
}

// the selection of the queue follows the playing track unless the user is in the queue
// rewind puts it on the first entry, which is where a queue that has played out starts again
static void follow(bool rewind) {
	struct list* l = &ui.lists[PANE_QUEUE];

	l->count = (int)ui.s.count;
	if(ui.focus != PANE_QUEUE && ui.s.track) {
		l->sel = (int)ui.s.index;
	} else if(ui.focus != PANE_QUEUE && rewind) {
		l->sel = 0;
	}

	l->sel = clamp(l->sel, 0, l->count ? l->count - 1 : 0);
	scroll_by(l, 0);
	show(l);
}

// a track or an album goes to the top of the queue and plays, what the queue held follows it
static void activate(void) {
	const struct row* r = &ui.page_rows[ui.lists[PANE_PAGE].sel];
	const struct list* q = &ui.lists[PANE_QUEUE];

	if(ui.focus == PANE_ARTISTS) {
		focus(PANE_PAGE);
	} else if(ui.focus == PANE_PAGE && r->kind == ROW_ALBUM) {
		player_start(&ui.lib->tracks[ui.lib->albums[r->id].first], ui.lib->albums[r->id].count);
	} else if(ui.focus == PANE_PAGE) {
		player_start(&ui.lib->tracks[r->id], 1);
	} else if(q->sel < q->count) {
		player_jump((uint32_t)q->sel);
	}
}

// the selection goes to the end of the queue: a track, the tracks of an album or those of an artist
static void enqueue(void) {
	const struct artist* a = &ui.lib->artists[ui.lists[PANE_ARTISTS].sel];
	const struct row* r = &ui.page_rows[ui.lists[PANE_PAGE].sel];
	const struct track* first = &ui.lib->tracks[a->first_track];
	const char* name = a->name;
	uint32_t n = a->tracks;

	if(ui.focus == PANE_QUEUE) {
		return;
	}

	if(ui.focus == PANE_PAGE && r->kind == ROW_ALBUM) {
		first = &ui.lib->tracks[ui.lib->albums[r->id].first];
		name = ui.lib->albums[r->id].title;
		n = ui.lib->albums[r->id].count;
	} else if(ui.focus == PANE_PAGE) {
		first = &ui.lib->tracks[r->id];
		name = first->title;
		n = 1;
	}

	player_add(first, n);
	if(n == 1) {
		snprintf(ui.msg, sizeof(ui.msg), "queued: %s", name);
	} else {
		snprintf(ui.msg, sizeof(ui.msg), "queued %u tracks: %s", n, name);
	}
}

// the selected entry of the queue trades places with the one above or below it and stays selected
static void move_entry(int dir) {
	struct list* l = &ui.lists[PANE_QUEUE];
	int to = l->sel + dir;

	if(ui.focus == PANE_QUEUE && to >= 0 && to < l->count) {
		player_move((uint32_t)l->sel, (uint32_t)to);
		l->sel = to;
		show(l);
	}
}

// the entry that follows takes the place of the selected one, and with it the selection
static void remove_entry(void) {
	const struct list* l = &ui.lists[PANE_QUEUE];

	if(ui.focus == PANE_QUEUE && l->sel < l->count) {
		player_remove((uint32_t)l->sel);
	}
}

static void clear_queue(void) {
	if(ui.focus == PANE_QUEUE) {
		player_clear();
	}
}

// with nothing playing the queue starts at its selected entry
static void toggle(void) {
	if(!ui.s.track && ui.s.count) {
		player_jump((uint32_t)ui.lists[PANE_QUEUE].sel);
	} else {
		player_toggle();
	}
}

static bool double_click(enum pane p, int row) {
	struct timespec now;
	int64_t ms;
	bool twice;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (int64_t)(now.tv_sec - ui.click_time.tv_sec) * 1000 + (now.tv_nsec - ui.click_time.tv_nsec) / 1000000;
	twice = p == ui.click_pane && row == ui.click_row && ms < DOUBLE_MS;
	ui.click_pane = p;
	ui.click_row = twice ? -1 : row;
	ui.click_time = now;

	return twice;
}

// the pane that shows at column x
static enum pane pane_at(int x) {
	if(x <= ui.side) {
		return PANE_ARTISTS;
	}

	if(!ui.split) {
		return ui.tab;
	}

	return x >= ui.x[PANE_QUEUE] ? PANE_QUEUE : PANE_PAGE;
}

// add puts the row into the queue, otherwise a second click on it plays it
static void click_list(int x, int row, bool add) {
	enum pane p = pane_at(x);
	struct list* l = &ui.lists[p];
	int i = l->top + row;

	if(x < ui.x[p] || x >= ui.x[p] + ui.pane_w[p]) {
		return;
	}

	focus(p);
	if(i >= l->count) {
		return;
	}

	if(p == PANE_ARTISTS) {
		select_artist(i);
	} else if(p == PANE_QUEUE || ui.page_rows[i].kind != ROW_SPACE) {
		l->sel = i;
		if(!add && double_click(p, i)) {
			activate();
		}
	} else {
		return;
	}

	if(add) {
		enqueue();
	}
}

static void click_title(int x) {
	enum pane p;

	for(p = PANE_ARTISTS; p < PANES; p++) {
		if(x >= ui.title_x[p] && x < ui.title_end[p]) {
			focus(p);
		}
	}
}

static void click(int x, int y) {
	if(!y) {
		click_title(x);
	} else if(y >= 1 && y <= ui.rows) {
		click_list(x, y - 1, false);
	} else if(y == ui.h - 3) {
		if(x >= ui.prev_x - 1 && x <= ui.prev_x + 2) {
			player_prev();
		} else if(x >= ui.play_x - 1 && x <= ui.play_x + 2) {
			toggle();
		} else if(x >= ui.next_x - 1 && x <= ui.next_x + 2) {
			player_next();
		} else if(x >= ui.repeat_x - 1 && x <= ui.repeat_x + 2) {
			player_repeat();
		}
	} else if(y == ui.h - 2 && ui.bar_w > 1 && ui.bar_ms && x >= ui.bar_x && x < ui.bar_x + ui.bar_w) {
		player_seek_to((uint32_t)((uint64_t)ui.bar_ms * (uint32_t)(x - ui.bar_x) / (uint32_t)(ui.bar_w - 1)));
	}
}

// ncurses merges a press and a release that arrive together, and such clicks into multiples
static int clicks(mmask_t b) {
	if(b & BUTTON1_TRIPLE_CLICKED) {
		return 3;
	}

	if(b & BUTTON1_DOUBLE_CLICKED) {
		return 2;
	}

	return b & (BUTTON1_PRESSED | BUTTON1_CLICKED) ? 1 : 0;
}

// one KEY_MOUSE stands for every event that is queued
static void mouse(void) {
	MEVENT e;
	int n;

	while(getmouse(&e) == OK) {
		if(too_small()) {
			continue;
		}

		// a release does nothing and leaves the message of its press in place
		if(e.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED | BUTTON3_PRESSED | BUTTON3_CLICKED) || clicks(e.bstate)) {
			ui.msg[0] = 0;
		}

		if(e.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)) {
			if(e.y >= 1 && e.y <= ui.rows) {
				scroll_by(&ui.lists[pane_at(e.x)], e.bstate & BUTTON4_PRESSED ? -WHEEL : WHEEL);
			}

			continue;
		}

		if(e.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED)) {
			if(e.y >= 1 && e.y <= ui.rows) {
				click_list(e.x, e.y - 1, true);
			}

			continue;
		}

		for(n = clicks(e.bstate); n; n--) {
			click(e.x, e.y);
		}
	}
}

// meta arrives as escape followed by the key, on the linux console that holds for the arrows too
static void meta_key(int ch) {
	const struct list* l = &ui.lists[ui.focus];

	if(ch == 'v') {
		go(l->sel - (ui.rows - 1), -1);
	} else if(ch == '<') {
		go(0, 1);
	} else if(ch == '>') {
		go(l->count - 1, -1);
	} else if(ch == 'p' || ch == KEY_UP) {
		move_entry(-1);
	} else if(ch == 'n' || ch == KEY_DOWN) {
		move_entry(1);
	} else if(ch == 'k') {
		clear_queue();
	}
}

static void key(int ch) {
	const struct list* l = &ui.lists[ui.focus];

	if(ch != KEY_MOUSE) {
		ui.msg[0] = 0;
	}

	switch(ch) {
		case KEY_ENTRY_UP:
			move_entry(-1);
			break;
		case KEY_ENTRY_DOWN:
			move_entry(1);
			break;
		case KEY_DC:
		case 'K' & 0x1f:
			remove_entry();
			break;
		case KEY_UP:
		case 'P' & 0x1f:
			go(l->sel - 1, -1);
			break;
		case KEY_DOWN:
		case 'N' & 0x1f:
			go(l->sel + 1, 1);
			break;
		case KEY_PPAGE:
			go(l->sel - (ui.rows - 1), -1);
			break;
		case KEY_NPAGE:
		case 'V' & 0x1f:
			go(l->sel + (ui.rows - 1), 1);
			break;
		case KEY_HOME:
			go(0, 1);
			break;
		case KEY_END:
			go(l->count - 1, -1);
			break;
		case KEY_RIGHT:
		case 'F' & 0x1f:
			if(ui.focus < PANE_QUEUE) {
				focus((enum pane)(ui.focus + 1));
			}

			break;
		case KEY_LEFT:
		case 'B' & 0x1f:
			if(ui.focus > PANE_ARTISTS) {
				focus((enum pane)(ui.focus - 1));
			}

			break;
		case '\t':
			focus((enum pane)((ui.focus + 1) % PANES));
			break;
		case KEY_BTAB:
			focus((enum pane)((ui.focus + PANES - 1) % PANES));
			break;
		case '\n':
		case '\r':
		case KEY_ENTER:
			activate();
			break;
		case ' ':
			toggle();
			break;
		case 'a':
			enqueue();
			break;
		case '<':
			player_prev();
			break;
		case '>':
			player_next();
			break;
		case ',':
			player_seek_by(-SEEK_MS);
			break;
		case '.':
			player_seek_by(SEEK_MS);
			break;
		case 'r':
			player_repeat();
			break;
		case 27:
			meta_key(getch());
			break;
		case 'L' & 0x1f:
			clearok(stdscr, TRUE);
			break;
		case KEY_MOUSE:
			mouse();
			break;
		case KEY_RESIZE:
			layout();
			show(&ui.lists[PANE_ARTISTS]);
			select_row(ui.lists[PANE_PAGE].sel, 1);
			follow(false);
			break;
		case 'q':
		case 'C' & 0x1f:
			ui.quit = true;
			break;
		default:
			break;
	}
}

// wakes once a second on the second, and when the track ends
static int timeout_ms(const struct play_status* s) {
	uint32_t ms;
	uint32_t left;

	if(s->state != PLAY_PLAYING) {
		return -1;
	}

	ms = 1000 - s->position % 1000;
	left = s->track->duration > s->position ? s->track->duration - s->position : 1000;

	return (int)(left < ms ? left : ms) + 20;
}

static void take_message(void) {
	pthread_mutex_lock(&ui.msg_lock);
	if(ui.msg_in[0]) {
		memcpy(ui.msg, ui.msg_in, sizeof(ui.msg));
		ui.msg_in[0] = 0;
	}

	pthread_mutex_unlock(&ui.msg_lock);
}

// a library that writes to stderr would draw over the screen
static int hide_stderr(void) {
	int saved = -1;
	int null;

	if(!isatty(2)) {
		return -1;
	}

	null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if(null >= 0) {
		saved = fcntl(2, F_DUPFD_CLOEXEC, 3);
		dup2(null, 2);
		close(null);
	}

	return saved;
}

// terminfo names the sequence of an arrow with a modifier where the terminal has one, otherwise it is the one of xterm
static void bind_key(const char* cap, const char* xterm, int code) {
	const char* s = tigetstr(cap);

	define_key(xterm, code);
	if(s && s != (const char*)-1) {
		define_key(s, code);
	}
}

// true when the track or the queue is another one
static bool update_status(void) {
	struct play_status s;
	bool changed;
	bool played_out;

	player_status(&s);
	changed = s.track != ui.s.track || s.queue != ui.s.queue || s.count != ui.s.count || s.index != ui.s.index;
	played_out = ui.s.track && !s.track && ui.s.index + 1 == ui.s.count;
	ui.s = s;
	if(changed) {
		follow(played_out);
	}

	return changed;
}

void ui_run(const struct library* lib, int notify) {
	struct pollfd fds[3] = { { 0, POLLIN, 0 }, { notify, POLLIN, 0 }, { -1, POLLIN, 0 } };
	uint64_t count;
	bool dirty = true;
	int saved;
	int ch;

	ui.lib = lib;
	ui.notify = notify;
	ui.g = strcmp(nl_langinfo(CODESET), "UTF-8") ? &ascii : &cp437;
	ui.lists[PANE_ARTISTS] = (struct list){ 0, 0, (int)lib->n_artists };
	initscr();
	raw();
	noecho();
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);
	curs_set(0);
	set_escdelay(25);
	bind_key("kUP5", "\033[1;5A", KEY_ENTRY_UP);
	bind_key("kDN5", "\033[1;5B", KEY_ENTRY_DOWN);
	bind_key("kUP3", "\033[1;3A", KEY_ENTRY_UP);
	bind_key("kDN3", "\033[1;3B", KEY_ENTRY_DOWN);
	mouseinterval(0);
	// getch blocks after a mouse event that the mask leaves out, nodelay or not, so the mask takes every button event
	mousemask(ALL_MOUSE_EVENTS, nullptr);
	if(has_colors()) {
		start_color();
		use_default_colors();
		init_pair(1, COLOR_GREEN, -1);
	}

	saved = hide_stderr();
	report_sink(on_report);
	mpris_init(lib);
	layout();
	build_page();
	while(!ui.quit) {
		if(update_status()) {
			dirty = true;
		}

		mpris_update(&ui.s);

		if(dirty) {
			take_message();
			draw();
		} else if(!too_small()) {
			draw_bar();
		}

		refresh();
		dirty = false;
		fds[2].fd = mpris_fd();
		poll(fds, 3, timeout_ms(&ui.s));
		if(read(notify, &count, sizeof(count)) > 0) {
			dirty = true;
		}

		if(mpris_dispatch()) {
			ui.quit = true;
		}

		while((ch = getch()) != ERR) {
			key(ch);
			dirty = true;
		}
	}

	mpris_fini();
	report_sink(nullptr);
	endwin();
	if(saved >= 0) {
		dup2(saved, 2);
		close(saved);
	}

	free(ui.page_rows);
}
