#include "screen.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define MIDDLE_W 28
#define KNOB_W   14

static attr_t title_attr(enum pane p) {
	if(p == ui.focus) {
		return A_BOLD;
	}

	return shown(p) ? A_NORMAL : A_DIM;
}

// a title sits in the top border between two spaces and takes at most w columns
// returns the column after it
static int title(enum pane p, int x, int w, const char* s) {
	int end;

	attrset(title_attr(p));
	mvaddch(0, x, ' ');
	end = x + 1 + text(0, x + 1, w - 2, s, false);
	mvaddch(0, end, ' ');
	attrset(A_NORMAL);
	ui.title_x[p] = x;
	ui.title_end[p] = end + 1;

	return end + 1;
}

static void divider(int x) {
	mvvline(1, x, ACS_VLINE, ui.rows);
	mvaddch(0, x, ACS_TTEE);
	mvaddch(ui.h - 4, x, ACS_BTEE);
}

// without the split the two titles of the main pane are its tabs, the one that does not show is dim
static void draw_frame(void) {
	const char* name = ui.lib->artists[ui.lists[PANE_ARTISTS].sel].name;
	int sep = ui.h - 4;
	int x;

	mvhline(0, 1, ACS_HLINE, ui.w - 2);
	mvhline(sep, 1, ACS_HLINE, ui.w - 2);
	mvhline(ui.h - 1, 1, ACS_HLINE, ui.w - 2);
	mvvline(1, 0, ACS_VLINE, ui.h - 2);
	mvvline(1, ui.w - 1, ACS_VLINE, ui.h - 2);
	mvaddch(0, 0, ACS_ULCORNER);
	mvaddch(0, ui.w - 1, ACS_URCORNER);
	mvaddch(sep, 0, ACS_LTEE);
	mvaddch(sep, ui.w - 1, ACS_RTEE);
	mvaddch(ui.h - 1, 0, ACS_LLCORNER);
	mvaddch(ui.h - 1, ui.w - 1, ACS_LRCORNER);
	divider(ui.side + 1);
	title(PANE_ARTISTS, 2, ui.side - 1, "album artists");
	if(ui.split) {
		divider(ui.x[PANE_QUEUE] - 1);
		title(PANE_PAGE, ui.main_x + 1, ui.pane_w[PANE_PAGE] - 2, name);
		title(PANE_QUEUE, ui.x[PANE_QUEUE] + 1, ui.pane_w[PANE_QUEUE] - 2, "queue");
	} else {
		x = title(PANE_PAGE, ui.main_x + 1, ui.main_w - 10, name);
		title(PANE_QUEUE, x + 1, 7, "queue");
	}

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
	const struct list* l = &ui.lists[PANE_ARTISTS];
	int i;

	for(i = l->top; i < l->count && i < l->top + ui.rows; i++) {
		int y = 1 + i - l->top;
		bool selected = i == l->sel;

		attrset(row_attr(selected, ui.focus == PANE_ARTISTS));
		text(y, 1, 1, selected ? ui.g->select : " ", true);
		text(y, 2, ui.side - 1, ui.lib->artists[i].name, true);
	}

	attrset(A_NORMAL);
}

// marker 1 column, year or number 4, gap 2, title, duration 8
static void draw_page(void) {
	const struct list* l = &ui.lists[PANE_PAGE];
	char dur[16];
	int x = ui.x[PANE_PAGE];
	int w = ui.pane_w[PANE_PAGE];
	int title_w = w - 15;
	int i;

	for(i = l->top; i < l->count && i < l->top + ui.rows; i++) {
		const struct row* r = &ui.page_rows[i];
		int y = 1 + i - l->top;
		bool selected = i == l->sel;
		attr_t attr = row_attr(selected, ui.focus == PANE_PAGE);

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

			text(y, x + 7, w - 7, al->title, true);
		} else {
			const struct track* t = &ui.lib->tracks[r->id];
			bool playing = t == ui.s.track;

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

// marker 1 column, playing mark 1, gap 1, title, duration 8
static void draw_queue(void) {
	const struct list* l = &ui.lists[PANE_QUEUE];
	char dur[16];
	int x = ui.x[PANE_QUEUE];
	int title_w = ui.pane_w[PANE_QUEUE] - 11;
	int i;

	for(i = l->top; i < l->count && i < l->top + ui.rows; i++) {
		const struct track* t = ui.s.queue[i].track;
		int y = 1 + i - l->top;
		bool selected = i == l->sel;
		bool playing = ui.s.track && (uint32_t)i == ui.s.index;
		attr_t attr = row_attr(selected, ui.focus == PANE_QUEUE);

		time_text(dur, sizeof(dur), t->duration);
		attrset(playing ? attr | ACCENT | A_BOLD : attr);
		text(y, x, 1, selected ? ui.g->select : " ", true);
		text(y, x + 1, 1, playing ? ui.g->now : " ", true);
		addch(' ');
		text(y, x + 3, title_w, t->title, true);
		mvprintw(y, x + 3 + title_w, "%7s ", t->duration ? dur : "");
	}

	attrset(A_NORMAL);
}

int knob_at(int v, int min, int max) {
	return (v - min) * (ui.knob_w - 1) / (max - min);
}

// label 6 columns, slider, gap 1, value 6, without the slider only the value shows
static void draw_control(int y, const char* label, const char* value, int at, bool changed) {
	int i;

	attrset(changed ? ACCENT | A_BOLD : A_NORMAL);
	mvprintw(y, ui.w - 8, "%6s", value);
	if(!ui.knob_w) {
		attrset(A_NORMAL);
		return;
	}

	attrset(A_DIM);
	mvaddstr(y, ui.ctl_x, label);
	attrset(ACCENT | A_BOLD);
	for(i = 0; i < at; i++) {
		text(y, ui.knob_x + i, 1, ui.g->elapsed, false);
	}

	text(y, ui.knob_x + at, 1, ui.g->knob, false);
	attrset(A_DIM);
	mvhline(y, ui.knob_x + at + 1, ACS_HLINE, ui.knob_w - at - 1);
	attrset(A_NORMAL);
}

// the gain and the speed sit at the right end of the bar and leave the middle at least MIDDLE_W columns
// returns the columns they take with their gap, 0 when even the values do not fit
static int draw_controls(int y, int cw) {
	const struct play_status* s = &ui.s;
	char gain[16];
	char speed[16];
	int room = cw - MIDDLE_W;

	ui.knob_w = clamp(room - 15, 0, KNOB_W);
	if(ui.knob_w < 6) {
		ui.knob_w = 0;
	}

	if(room < 8) {
		ui.ctl_x = 0;
		return 0;
	}

	ui.knob_x = ui.w - 9 - ui.knob_w;
	ui.ctl_x = ui.knob_w ? ui.knob_x - 6 : ui.w - 8;
	snprintf(gain, sizeof(gain), s->gain ? "%+d dB" : "%d dB", s->gain);
	snprintf(speed, sizeof(speed), "%d.%02dx", s->speed / 100, s->speed % 100);
	draw_control(y, "gain", gain, knob_at(s->gain, GAIN_MIN, GAIN_MAX), s->gain != 0);
	draw_control(y + 1, "speed", speed, knob_at(s->speed, SPEED_MIN, SPEED_MAX), s->speed != SPEED_UNITY);

	return ui.w - 2 - ui.ctl_x + 2;
}

void draw_bar(void) {
	const struct play_status* s = &ui.s;
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
	cw -= draw_controls(y, cw);
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

	// the three transport buttons are centred, the repeat mark hangs off their right
	ui.prev_x = cx + (cw - 12) / 2;
	ui.play_x = ui.prev_x + 5;
	ui.next_x = ui.prev_x + 10;
	ui.repeat_x = ui.prev_x + 15;
	text(y, ui.prev_x, 2, ui.g->prev, false);
	text(y, ui.play_x, 2, s->state == PLAY_PLAYING ? ui.g->pause : ui.g->play, false);
	text(y, ui.next_x, 2, ui.g->next, false);
	attrset(s->repeat == REPEAT_OFF ? A_DIM : ACCENT | A_BOLD);
	text(y, ui.repeat_x, 1, ui.g->repeat, false);
	if(s->repeat == REPEAT_TRACK) {
		addch('1');
	}

	attrset(A_NORMAL);
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

void draw(void) {
	erase();
	if(too_small()) {
		mvaddnstr(0, 0, "terminal is too small", ui.w);
		return;
	}

	draw_frame();
	draw_artists();
	if(shown(PANE_PAGE)) {
		draw_page();
	}

	if(shown(PANE_QUEUE)) {
		draw_queue();
	}

	draw_bar();
}
