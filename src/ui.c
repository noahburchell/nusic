#include "ui.h"

#include "player.h"
#include "report.h"

#include <curses.h>
#include <fcntl.h>
#include <langinfo.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define MIN_W     40
#define MIN_H     9
#define SEEK_MS   5000
#define WHEEL     3
#define DOUBLE_MS 400
#define ACCENT    COLOR_PAIR(1)

enum {
	ROW_ALBUM,
	ROW_TRACK,
	ROW_SPACE,
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
};

static const struct glyphs cp437 = { "◄◄", "►►", "► ", "▌▌", "►", "▌", "═", "■" };
static const struct glyphs ascii = { "<<", ">>", "> ", "||", ">", "|", "=", "#" };

struct row {
	uint32_t id;
	uint8_t  kind;
};

struct list {
	int top;
	int sel;
	int count;
};

// side and main are the inner widths of the two panes, rows their height
// the hit boxes of the bar are set when it is drawn
static struct {
	const struct library* lib;
	const struct glyphs*  g;
	const struct track*   playing;
	struct row*           page_rows;
	struct list           artists;
	struct list           page;
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
	int                   prev_x;
	int                   play_x;
	int                   next_x;
	int                   bar_x;
	int                   bar_w;
	uint32_t              bar_ms;
	bool                  focus_page;
	bool                  quit;
} ui = { .msg_lock = PTHREAD_MUTEX_INITIALIZER };

// runs on any thread
static void on_report(const char* line) {
	uint64_t one = 1;
	ssize_t r;

	pthread_mutex_lock(&ui.msg_lock);
	snprintf(ui.msg_in, sizeof(ui.msg_in), "%s", line);
	pthread_mutex_unlock(&ui.msg_lock);
	r = write(ui.notify, &one, sizeof(one));
	(void)r;
}

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
static int text(int y, int x, int w, const char* s, bool pad) {
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
static int text_tail(int y, int x, int w, const char* s) {
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

static void time_text(char* buf, size_t n, uint32_t ms) {
	uint32_t s = ms / 1000;

	if(s >= 3600) {
		snprintf(buf, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(buf, n, "%u:%02u", s / 60, s % 60);
	}
}

static int clamp(int v, int lo, int hi) {
	if(v < lo) {
		return lo;
	}

	return v > hi ? hi : v;
}

static void layout(void) {
	getmaxyx(stdscr, ui.h, ui.w);
	ui.side = clamp(ui.w * 28 / 100, 16, 34);
	ui.main_x = ui.side + 2;
	ui.main_w = ui.w - 1 - ui.main_x;
	ui.rows = ui.h - 5;
}

static bool too_small(void) {
	return ui.w < MIN_W || ui.h < MIN_H;
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
	const struct artist* a = &ui.lib->artists[ui.artists.sel];
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
	ui.page = (struct list){ 0, 1, n - 1 };
	ui.click_row = -1;
}

static void select_artist(int i) {
	i = clamp(i, 0, ui.artists.count - 1);
	if(i != ui.artists.sel) {
		ui.artists.sel = i;
		build_page();
	}

	show(&ui.artists);
}

// a blank row is skipped in the direction of travel
static void select_row(int i, int dir) {
	i = clamp(i, 0, ui.page.count - 1);
	while(ui.page_rows[i].kind == ROW_SPACE) {
		i += dir;
	}

	ui.page.sel = i;
	show(&ui.page);
	// the heading of an album stays in view above its first track
	if(i && ui.page.top == i && ui.page_rows[i - 1].kind == ROW_ALBUM) {
		ui.page.top--;
	}
}

static void go(int i, int dir) {
	if(ui.focus_page) {
		select_row(i, dir);
	} else {
		select_artist(i);
	}
}

static struct list* focused(void) {
	return ui.focus_page ? &ui.page : &ui.artists;
}

// a track plays in the context of its artist, so the albums after it follow on
static void activate(void) {
	const struct artist* a = &ui.lib->artists[ui.artists.sel];
	const struct row* r = &ui.page_rows[ui.page.sel];
	uint32_t t = r->kind == ROW_ALBUM ? ui.lib->albums[r->id].first : r->id;

	if(!ui.focus_page) {
		ui.focus_page = true;
		return;
	}

	player_play(&ui.lib->tracks[a->first_track], a->tracks, t - a->first_track);
}

static void draw_frame(void) {
	const char* name = ui.lib->artists[ui.artists.sel].name;
	int sep = ui.h - 4;
	int x;

	mvhline(0, 1, ACS_HLINE, ui.w - 2);
	mvhline(sep, 1, ACS_HLINE, ui.w - 2);
	mvhline(ui.h - 1, 1, ACS_HLINE, ui.w - 2);
	mvvline(1, 0, ACS_VLINE, ui.h - 2);
	mvvline(1, ui.w - 1, ACS_VLINE, ui.h - 2);
	mvvline(1, ui.side + 1, ACS_VLINE, ui.rows);
	mvaddch(0, 0, ACS_ULCORNER);
	mvaddch(0, ui.w - 1, ACS_URCORNER);
	mvaddch(0, ui.side + 1, ACS_TTEE);
	mvaddch(sep, 0, ACS_LTEE);
	mvaddch(sep, ui.w - 1, ACS_RTEE);
	mvaddch(sep, ui.side + 1, ACS_BTEE);
	mvaddch(ui.h - 1, 0, ACS_LLCORNER);
	mvaddch(ui.h - 1, ui.w - 1, ACS_LRCORNER);
	attrset(ui.focus_page ? A_NORMAL : A_BOLD);
	mvaddch(0, 2, ' ');
	x = 3 + text(0, 3, ui.side - 3, "album artists", false);
	mvaddch(0, x, ' ');
	attrset(ui.focus_page ? A_BOLD : A_NORMAL);
	mvaddch(0, ui.main_x + 1, ' ');
	x = ui.main_x + 2 + text(0, ui.main_x + 2, ui.main_w - 4, name, false);
	mvaddch(0, x, ' ');
	attrset(A_NORMAL);
	if(ui.msg[0]) {
		mvaddch(ui.h - 1, 2, ' ');
		x = 3 + text_tail(ui.h - 1, 3, ui.w - 6, ui.msg);
		mvaddch(ui.h - 1, x, ' ');
	}
}

static attr_t row_attr(bool selected, bool pane_focused) {
	if(!selected) {
		return A_NORMAL;
	}

	return pane_focused ? A_REVERSE : A_BOLD;
}

static void draw_artists(void) {
	int i;

	for(i = ui.artists.top; i < ui.artists.count && i < ui.artists.top + ui.rows; i++) {
		int y = 1 + i - ui.artists.top;
		bool selected = i == ui.artists.sel;

		attrset(row_attr(selected, !ui.focus_page));
		text(y, 1, 1, selected ? ui.g->select : " ", true);
		text(y, 2, ui.side - 1, ui.lib->artists[i].name, true);
	}

	attrset(A_NORMAL);
}

// marker 1 column, year or number 4, gap 2, title, duration 8
static void draw_page(void) {
	char dur[16];
	int x = ui.main_x;
	int title_w = ui.main_w - 15;
	int i;

	for(i = ui.page.top; i < ui.page.count && i < ui.page.top + ui.rows; i++) {
		const struct row* r = &ui.page_rows[i];
		int y = 1 + i - ui.page.top;
		bool selected = i == ui.page.sel;
		attr_t attr = row_attr(selected, ui.focus_page);

		if(r->kind == ROW_SPACE) {
			continue;
		}

		if(r->kind == ROW_ALBUM) {
			const struct album* al = &ui.lib->albums[r->id];

			attrset(attr | A_BOLD);
			text(y, x, 1, selected ? ui.g->select : " ", true);
			if(al->year) {
				printw("%4u  ", al->year);
			} else {
				addstr("      ");
			}

			text(y, x + 7, ui.main_w - 7, al->title, true);
		} else {
			const struct track* t = &ui.lib->tracks[r->id];
			bool playing = t == ui.playing;

			time_text(dur, sizeof(dur), t->duration);
			attrset(playing ? attr | ACCENT | A_BOLD : attr);
			text(y, x, 1, selected ? ui.g->select : " ", true);
			if(playing) {
				addstr("   ");
				text(y, x + 4, 1, ui.g->now, true);
				addstr("  ");
			} else if(t->number) {
				printw("%4u  ", t->number);
			} else {
				addstr("      ");
			}

			text(y, x + 7, title_w, t->title, true);
			mvprintw(y, x + 7 + title_w, "%7s ", t->duration ? dur : "");
		}
	}

	attrset(A_NORMAL);
}

static void draw_bar(const struct play_status* s) {
	char pos[16] = "";
	char dur[16] = "";
	char line[512];
	int y = ui.h - 3;
	int inner = ui.w - 2;
	int left = clamp(inner * 32 / 100, 12, 46);
	int cx;
	int cw;
	int tw;
	int filled = 0;
	int i;

	cx = 1 + left + 1;
	cw = ui.w - 2 - cx;
	attrset(A_NORMAL);
	mvhline(y, 1, ' ', inner);
	mvhline(y + 1, 1, ' ', inner);
	if(s->track) {
		const struct album* al = &ui.lib->albums[s->track->album];

		time_text(pos, sizeof(pos), s->position);
		time_text(dur, sizeof(dur), s->track->duration);
		snprintf(line, sizeof(line), "%s - %s", s->track->artist, al->title);
		attrset(A_BOLD);
		text(y, 2, left - 1, s->track->title, false);
		attrset(A_DIM);
		text(y + 1, 2, left - 1, line, false);
		attrset(A_NORMAL);
	}

	ui.prev_x = cx + (cw - 12) / 2;
	ui.play_x = ui.prev_x + 5;
	ui.next_x = ui.prev_x + 10;
	text(y, ui.prev_x, 2, ui.g->prev, false);
	text(y, ui.play_x, 2, s->state == PLAY_PLAYING ? ui.g->pause : ui.g->play, false);
	text(y, ui.next_x, 2, ui.g->next, false);
	tw = (int)strlen(dur) < 4 ? 4 : (int)strlen(dur);
	ui.bar_x = cx + tw + 1;
	ui.bar_w = cw - 2 * tw - 3;
	ui.bar_ms = s->track ? s->track->duration : 0;
	if(ui.bar_w < 4) {
		ui.bar_w = 0;
		return;
	}

	mvprintw(y + 1, cx, "%*s", tw, pos);
	mvprintw(y + 1, ui.bar_x + ui.bar_w + 1, "%s", dur);
	if(ui.bar_ms) {
		filled = (int)((uint64_t)s->position * (uint32_t)(ui.bar_w - 1) / ui.bar_ms);
		filled = filled > ui.bar_w - 1 ? ui.bar_w - 1 : filled;
	}

	attrset(ACCENT | A_BOLD);
	for(i = 0; i < filled; i++) {
		text(y + 1, ui.bar_x + i, 1, ui.g->elapsed, false);
	}

	if(s->track) {
		text(y + 1, ui.bar_x + filled, 1, ui.g->knob, false);
		filled++;
	}

	attrset(A_DIM);
	mvhline(y + 1, ui.bar_x + filled, ACS_HLINE, ui.bar_w - filled);
	attrset(A_NORMAL);
}

static void draw(const struct play_status* s) {
	erase();
	if(too_small()) {
		mvaddnstr(0, 0, "terminal is too small", ui.w);
		return;
	}

	draw_frame();
	draw_artists();
	draw_page();
	draw_bar(s);
}

static bool double_click(int row) {
	struct timespec now;
	int64_t ms;
	bool twice;

	clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (int64_t)(now.tv_sec - ui.click_time.tv_sec) * 1000 + (now.tv_nsec - ui.click_time.tv_nsec) / 1000000;
	twice = row == ui.click_row && ms < DOUBLE_MS;
	ui.click_row = twice ? -1 : row;
	ui.click_time = now;

	return twice;
}

static void click(int x, int y) {
	int row = y - 1;

	if(row >= 0 && row < ui.rows && x >= 1 && x <= ui.side) {
		ui.focus_page = false;
		if(ui.artists.top + row < ui.artists.count) {
			select_artist(ui.artists.top + row);
		}
	} else if(row >= 0 && row < ui.rows && x >= ui.main_x && x < ui.main_x + ui.main_w) {
		row += ui.page.top;
		ui.focus_page = true;
		if(row < ui.page.count && ui.page_rows[row].kind != ROW_SPACE) {
			ui.page.sel = row;
			if(double_click(row)) {
				activate();
			}
		}
	} else if(y == ui.h - 3) {
		if(x >= ui.prev_x - 1 && x <= ui.prev_x + 2) {
			player_prev();
		} else if(x >= ui.play_x - 1 && x <= ui.play_x + 2) {
			player_toggle();
		} else if(x >= ui.next_x - 1 && x <= ui.next_x + 2) {
			player_next();
		}
	} else if(y == ui.h - 2 && ui.bar_w > 1 && ui.bar_ms && x >= ui.bar_x && x < ui.bar_x + ui.bar_w) {
		player_seek_to((uint32_t)((uint64_t)ui.bar_ms * (uint32_t)(x - ui.bar_x) / (uint32_t)(ui.bar_w - 1)));
	}
}

static void mouse(void) {
	MEVENT e;

	if(getmouse(&e) != OK || too_small()) {
		return;
	}

	if(e.bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)) {
		if(e.y >= 1 && e.y <= ui.rows) {
			scroll_by(e.x > ui.side ? &ui.page : &ui.artists, e.bstate & BUTTON4_PRESSED ? -WHEEL : WHEEL);
		}
	} else if(e.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED)) {
		click(e.x, e.y);
	}
}

// meta arrives as escape followed by the key
static void meta_key(int ch) {
	if(ch == 'v') {
		go(focused()->sel - (ui.rows - 1), -1);
	} else if(ch == '<') {
		go(0, 1);
	} else if(ch == '>') {
		go(focused()->count - 1, -1);
	}
}

static void key(int ch) {
	struct list* l = focused();

	ui.msg[0] = 0;
	switch(ch) {
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
			ui.focus_page = true;
			break;
		case KEY_LEFT:
		case 'B' & 0x1f:
			ui.focus_page = false;
			break;
		case '\t':
			ui.focus_page = !ui.focus_page;
			break;
		case '\n':
		case '\r':
		case KEY_ENTER:
			activate();
			break;
		case ' ':
			player_toggle();
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
			show(&ui.artists);
			select_row(ui.page.sel, 1);
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

void ui_run(const struct library* lib, int notify) {
	struct pollfd fds[2] = { { 0, POLLIN, 0 }, { notify, POLLIN, 0 } };
	struct play_status s;
	uint64_t count;
	bool dirty = true;
	int saved;
	int ch;

	ui.lib = lib;
	ui.notify = notify;
	ui.g = strcmp(nl_langinfo(CODESET), "UTF-8") ? &ascii : &cp437;
	ui.artists = (struct list){ 0, 0, (int)lib->n_artists };
	initscr();
	raw();
	noecho();
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);
	curs_set(0);
	set_escdelay(25);
	mouseinterval(0);
	mousemask(BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED | BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
	if(has_colors()) {
		start_color();
		use_default_colors();
		init_pair(1, COLOR_GREEN, -1);
	}

	saved = hide_stderr();
	report_sink(on_report);
	layout();
	build_page();
	while(!ui.quit) {
		player_status(&s);
		if(s.track != ui.playing) {
			ui.playing = s.track;
			dirty = true;
		}

		if(dirty) {
			take_message();
			draw(&s);
		} else if(!too_small()) {
			draw_bar(&s);
		}

		refresh();
		dirty = false;
		poll(fds, 2, timeout_ms(&s));
		if(read(notify, &count, sizeof(count)) > 0) {
			dirty = true;
		}

		while((ch = getch()) != ERR) {
			key(ch);
			dirty = true;
		}
	}

	report_sink(nullptr);
	endwin();
	if(saved >= 0) {
		dup2(saved, 2);
		close(saved);
	}

	free(ui.page_rows);
}
