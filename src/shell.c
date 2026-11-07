/* The XP shell: Bliss desktop with icons, the Luna taskbar, and the two-column
 * Start menu. Each is one cairo-drawn buffer in the scene graph, redrawn when
 * its state changes.
 *
 * The Start menu has two styles. Classic is XP's: All Programs opens a cascade
 * of every installed app. Modern (start_menu = modern) adds a search box at the
 * bottom of the left column: typing filters apps, and All Programs swaps the
 * left column for a scrolling list. */
#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "expede.h"

#define START_W 99
#define MENU_W 380
#define MENU_H 440
#define SEARCH_H 30
#define USER_H 64
#define LOGOFF_H 40
#define MORE_H 30
#define COL_W 190
#define ROW_H 24       /* modern list rows */
#define CASCADE_ROW 22 /* XP menu rows */
#define CASCADE_COL 220
/* Windows 7 */
#define W7_ORB 54      /* the start orb, 96 dpi */
#define W7_OVER 8      /* the orb rises this far above the superbar */
#define W7_TASK_W 60   /* icon-only task buttons */
#define W7_SHOWDESK 15
#define W7_MENU_W 400
#define W7_MENU_H 520
#define W7_LEFT_W 250
#define W7_PIC_OVER 20 /* the account picture rises above the menu */

enum item_kind { ITEM_PINNED, ITEM_MFU, ITEM_PLACE_BOLD, ITEM_PLACE, ITEM_SEP_LEFT, ITEM_SEP_RIGHT,
	ITEM_ALL_PROGRAMS, ITEM_LOGOFF, ITEM_TURNOFF };

struct menu_item {
	const char *label7; /* Windows 7's wording; NULL hides the item there */
	enum item_kind kind;
	const char *label, *icon, *cmd;
	struct icon_set img;
	struct wlr_box box;
};

struct desk_icon {
	const char *label, *label7, *icon, *cmd;
	struct icon_set img;
	struct wlr_box box;
};

struct task {
	struct view *view;
	struct wlr_box box;
};

struct shell {
	struct server *server;
	int w, h;
	struct wlr_scene_buffer *desktop, *taskbar, *menu, *cascade;
	struct wlr_scene_buffer *bar_glass, *menu_glass;
	char date[24];
	struct wlr_box showdesk_box; /* taskbar coords */
	bool showdesk_hot;
	struct view *peeked[64]; /* windows Show desktop minimised, to bring back */
	int n_peeked;
	struct menu_item items[24];
	int n_items, menu_hot;
	struct desk_icon icons[8];
	int n_icons, icon_sel;
	struct task tasks[64];
	int n_tasks, task_hot, start_hot;
	bool start_open;
	char clock[16];
	struct wl_event_source *timer;

	int menu_h;
	/* classic: the All Programs cascade */
	bool cascade_open;
	struct wlr_box cascade_box; /* layout coords */
	int cascade_rows, cascade_hot;
	/* modern: search and the app list */
	bool modern, all_mode;
	char query[128];
	int *results, n_results, scroll, list_sel, list_hot;
	/* the tray speaker and XP's Volume pop-up */
	bool vol_ok, vol_open, vol_drag, vol_muted;
	int vol_pct;
	struct wlr_box vol_icon_box; /* taskbar coords */
	struct wlr_box vol_box;      /* layout coords */
	struct icon_set vol_icon, vol_icon_muted;
	struct wlr_scene_buffer *volpop;
};

#define VOL_W 72
#define VOL_H 160
#define VOL_TRACK_TOP 30
#define VOL_TRACK_H 88

static void show(struct wlr_scene_buffer **node, struct wlr_scene_tree *parent, cairo_surface_t *s, int x, int y) {
	scene_show(node, parent, s, x, y);
}

static bool in_box(struct wlr_box *b, double x, double y) {
	return x >= b->x && x < b->x + b->width && y >= b->y && y < b->y + b->height;
}

static void draw_scaled(cairo_t *cr, cairo_surface_t *s, double x, double y, double size) {
	if (!s)
		return;
	double k = size / cairo_image_surface_get_width(s);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void fill(cairo_t *cr, double x, double y, double w, double h, uint32_t rgb) {
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

/* which Windows draws each part (the advanced options can mix them) */
static struct theme *bar_theme(struct shell *sh) { return theme_of(sh->server, sh->server->st_taskbar); }
static struct theme *menu_theme(struct shell *sh) { return theme_of(sh->server, sh->server->st_start); }
static struct theme *desk_theme(struct shell *sh) { return theme_of(sh->server, sh->server->st_icons); }
static bool bar7(struct shell *sh) { return bar_theme(sh)->version == 7; }
static bool menu7(struct shell *sh) { return menu_theme(sh)->version == 7; }
static bool desk7(struct shell *sh) { return desk_theme(sh)->version == 7; }
static int bar_h(struct shell *sh) { return taskbar_h(sh->server); }
static int start_w(struct shell *sh) { return bar7(sh) ? W7_ORB + 2 : START_W; }
static int menu_w(struct shell *sh) { return menu7(sh) ? W7_MENU_W : MENU_W; }

/* Windows 7 hover: a pale blue rounded box */
static void hl7(cairo_t *cr, struct wlr_box b) {
	double x = b.x + 0.5, y = b.y + 0.5, w = b.width - 1, h = b.height - 1, r = 3;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
	cairo_pattern_add_color_stop_rgb(p, 0, 0xf2 / 255.0, 0xf8 / 255.0, 0xfe / 255.0);
	cairo_pattern_add_color_stop_rgb(p, 1, 0xd5 / 255.0, 0xe7 / 255.0, 0xfa / 255.0);
	cairo_set_source(cr, p);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(p);
	cairo_set_source_rgb(cr, 0x7d / 255.0, 0xa2 / 255.0, 0xce / 255.0);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* hover on glass: a lighter pane with a white rim */
static void glass_hl(cairo_t *cr, struct wlr_box b) {
	cairo_rectangle(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.22);
	cairo_fill_preserve(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	cairo_rectangle(cr, b.x - 0.5, b.y - 0.5, b.width + 1, b.height + 1);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
	cairo_stroke(cr);
}

/* one blur pass along a line of n pixels, step apart (ARGB, premultiplied) */
static void blur_line(uint32_t *px, int n, int step, int r, uint32_t *tmp) {
	for (int i = 0; i < n; i++)
		tmp[i] = px[i * step];
	int sum[4] = {0}, cnt = 0;
	for (int i = -r; i <= r; i++)
		if (i >= 0 && i < n) {
			for (int c = 0; c < 4; c++)
				sum[c] += (tmp[i] >> (c * 8)) & 255;
			cnt++;
		}
	for (int i = 0; i < n; i++) {
		uint32_t v = 0;
		for (int c = 0; c < 4; c++)
			v |= (uint32_t)(sum[c] / cnt) << (c * 8);
		px[i * step] = v;
		int out = i - r, in = i + r + 1;
		if (out >= 0) {
			for (int c = 0; c < 4; c++)
				sum[c] -= (tmp[out] >> (c * 8)) & 255;
			cnt--;
		}
		if (in < n) {
			for (int c = 0; c < 4; c++)
				sum[c] += (tmp[in] >> (c * 8)) & 255;
			cnt++;
		}
	}
}

/* Aero glass shows what's behind it blurred; Expede blurs the desktop picture
 * (wallpaper and icons) once, small, and frames show the part under them */
static void make_glass(struct shell *sh, cairo_surface_t *desk) {
	struct server *s = sh->server;
	int gw = sh->w / GLASS_DIV + 1, gh = sh->h / GLASS_DIV + 1;
	cairo_surface_t *g = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
	cairo_t *cr = cairo_create(g);
	cairo_scale(cr, (double)gw / sh->w, (double)gh / sh->h);
	cairo_set_source_surface(cr, desk, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_flush(g);
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(g);
	int st = cairo_image_surface_get_stride(g) / 4;
	uint32_t *tmp = malloc(sizeof(uint32_t) * (gw > gh ? gw : gh));
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < gh; y++)
			blur_line(px + y * st, gw, 1, 3, tmp);
		for (int x = 0; x < gw; x++)
			blur_line(px + x, gh, st, 3, tmp);
	}
	free(tmp);
	cairo_surface_mark_dirty(g);
	if (s->glass_buf)
		wlr_buffer_drop(s->glass_buf);
	s->glass_buf = buffer_from_cairo(g);
	server_glass_changed(s);
}

/* ---------------------------------------------------------------- desktop */

static void draw_desktop(struct shell *sh) {
	struct theme *t = desk_theme(sh);
	theme_use_fonts(t);
	cairo_surface_t *s = canvas_new(sh->w, sh->h);
	cairo_t *cr = cairo_create(s);
	cairo_set_source_rgb(cr, t->desktop_bg[0], t->desktop_bg[1], t->desktop_bg[2]);
	cairo_paint(cr);
	if (t->wallpaper) {
		/* XP's three positions: Stretch, Center (on the desktop colour) and Tile */
		double ww = cairo_image_surface_get_width(t->wallpaper), wh = cairo_image_surface_get_height(t->wallpaper);
		cairo_save(cr);
		if (t->wallpaper_style == WALL_TILE) {
			cairo_set_source_surface(cr, t->wallpaper, 0, 0);
			cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
		} else if (t->wallpaper_style == WALL_CENTER) {
			cairo_set_source_surface(cr, t->wallpaper, (int)((sh->w - ww) / 2), (int)((sh->h - wh) / 2));
		} else {
			cairo_scale(cr, sh->w / ww, sh->h / wh);
			cairo_set_source_surface(cr, t->wallpaper, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		}
		cairo_paint(cr);
		cairo_restore(cr);
	}
	for (int i = 0; i < sh->n_icons; i++) {
		struct desk_icon *d = &sh->icons[i];
		bool sel = i == sh->icon_sel;
		int ix = d->box.x + (d->box.width - 32) / 2;
		draw_surface(cr, d->img.s32, ix, d->box.y + 2);
		if (sel && d->img.s32) {
			/* XP tints the selected icon with the highlight colour */
			cairo_save(cr);
			cairo_set_source_rgba(cr, 49 / 255.0, 106 / 255.0, 197 / 255.0, 0.5);
			cairo_mask_surface(cr, d->img.s32, ix, d->box.y + 2);
			cairo_restore(cr);
		}
		const char *label = desk7(sh) && d->label7 ? d->label7 : d->label;
		draw_label(cr, "Tahoma 8", label, d->box.x + d->box.width / 2.0, d->box.y + 38, d->box.width - 2,
			0xffffff, 0x000000, sel ? (desk7(sh) ? 0x3399ff : 0x316ac5) : -1);
	}
	cairo_destroy(cr);
	make_glass(sh, s);
	show(&sh->desktop, sh->server->layer_desktop, s, 0, 0);
}

static void add_icon(struct shell *sh, const char *label, const char *label7, const char *icon, const char *cmd) {
	struct desk_icon *d = &sh->icons[sh->n_icons];
	d->label = label;
	d->label7 = label7;
	d->icon = icon;
	d->cmd = cmd;
	d->img = theme_icon(desk_theme(sh), icon);
	d->box = (struct wlr_box){4, 6 + sh->n_icons * 72, 75, 60};
	sh->n_icons++;
}

/* ---------------------------------------------------------------- taskbar */

static void tick(struct shell *sh) {
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(sh->clock, sizeof(sh->clock), sh->server->opt_clock24 ? "%H:%M" : "%-I:%M %p", &tm);
	strftime(sh->date, sizeof(sh->date), "%-m/%-d/%Y", &tm);
}

/* Windows 7's superbar: glass, the orb, icon-only buttons, a two-line clock, Show desktop */
static void draw_taskbar7(struct shell *sh) {
	struct server *srv = sh->server;
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	int bh = bar_h(sh);
	cairo_surface_t *s = canvas_new(sh->w, bh + W7_OVER);
	cairo_t *cr = cairo_create(s);
	cairo_translate(cr, 0, W7_OVER);
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, sh->w, bh);
	cairo_clip(cr);
	draw_glass(srv, cr, -2, 0, sh->w + 4, bh + 2, 0, 0, true);
	cairo_set_source_rgba(cr, 0, 0, 0, srv->opt_glass ? 0.3 : 0.15);
	cairo_paint(cr);
	cairo_restore(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
	cairo_rectangle(cr, 0, 0, sh->w, 1);
	cairo_fill(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
	cairo_rectangle(cr, 0, 1, sh->w, 1);
	cairo_fill(cr);

	/* tray: speaker, clock and date, then Show desktop at the very edge */
	sh->showdesk_box = (struct wlr_box){sh->w - W7_SHOWDESK, 0, W7_SHOWDESK, bh};
	draw_image(cr, &t->w7_show_desktop, sh->showdesk_hot ? 1 : 0, sh->showdesk_box.x, 0, W7_SHOWDESK, bh);
	int cw = text_width("Segoe UI 9", sh->clock), dw = text_width("Segoe UI 9", sh->date);
	int clock_w = (cw > dw ? cw : dw) + 16;
	int cx = sh->showdesk_box.x - clock_w;
	draw_text(cr, "Segoe UI 9", sh->clock, cx + (clock_w - cw) / 2, 4, 0, 0xffffff, 0, 0, 0, false);
	draw_text(cr, "Segoe UI 9", sh->date, cx + (clock_w - dw) / 2, 20, 0, 0xffffff, 0, 0, 0, false);
	int tx = cx;
	if (sh->vol_ok) {
		tx -= 26;
		sh->vol_icon_box = (struct wlr_box){tx, 8, 24, 24};
		draw_surface(cr, (sh->vol_muted ? sh->vol_icon_muted : sh->vol_icon).s16, tx + 4, 12);
	}

	/* task buttons: running windows, icon only */
	int x0 = start_w(sh) + 4, avail = tx - x0 - 8;
	int bw = sh->n_tasks && avail / sh->n_tasks < W7_TASK_W ? avail / sh->n_tasks : W7_TASK_W;
	for (int i = 0; i < sh->n_tasks; i++) {
		struct task *k = &sh->tasks[i];
		k->box = (struct wlr_box){x0 + i * bw, 0, bw - 1, bh};
		bool act = k->view->active && !k->view->minimized, hot = i == sh->task_hot;
		/* superbar button frames: normal hot pressed, active normal hot pressed, flash... */
		int f = act ? (hot ? 4 : 3) : (hot ? 1 : 0);
		draw_image(cr, &t->w7_task, f, k->box.x, 0, k->box.width, bh);
		struct icon_set *ic = &k->view->icon;
		if (ic->s32)
			draw_scaled(cr, ic->s32, k->box.x + (k->box.width - 32) / 2.0, (bh - 32) / 2.0, 32);
		else if (ic->s16)
			draw_scaled(cr, ic->s16, k->box.x + (k->box.width - 24) / 2.0, (bh - 24) / 2.0, 24);
	}

	/* the orb, last: it rises out of the bar */
	int sf = sh->start_open ? 2 : sh->start_hot ? 1 : 0;
	draw_frame_dpi(cr, t->orb[0], t->orb[1], 3, sf, (start_w(sh) - W7_ORB) / 2, (bh - W7_ORB) / 2);
	cairo_destroy(cr);
	show(&sh->taskbar, srv->layer_shell, s, 0, sh->h - bh - W7_OVER);
	glass_backdrop(srv, &sh->bar_glass, srv->layer_shell, 0, sh->h - bh, 0, sh->h - bh, sh->w, bh);
	if (sh->bar_glass)
		wlr_scene_node_place_below(&sh->bar_glass->node, &sh->taskbar->node);
}

static void draw_taskbar(struct shell *sh) {
	if (bar7(sh)) {
		draw_taskbar7(sh);
		return;
	}
	if (sh->bar_glass)
		wlr_scene_node_set_enabled(&sh->bar_glass->node, false);
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	cairo_surface_t *s = canvas_new(sh->w, TASKBAR_H);
	cairo_t *cr = cairo_create(s);
	draw_image(cr, &t->taskbar, 0, 0, 0, sh->w, TASKBAR_H);

	/* start button: frames normal / hot / pressed */
	int sf = sh->start_open ? 2 : sh->start_hot ? 1 : 0;
	draw_image(cr, &t->start_button, sf, 0, 0, START_W, TASKBAR_H);
	draw_surface(cr, t->start_flag, 7, 5);
	draw_text(cr, "Franklin Gothic Medium, Italic 14", "start", 35, 2, 0, 0xffffff, 1, 1, 0x454c10, false);

	/* tray: the speaker, then the clock */
	int cw = text_width("Tahoma 8", sh->clock);
	int icons_w = sh->vol_ok ? 22 : 0;
	int trayw = cw + 19 + 14 + icons_w;
	if (trayw < 62)
		trayw = 62;
	int tx = sh->w - trayw;
	draw_image(cr, &t->tray, 0, tx, 0, trayw, TASKBAR_H);
	if (sh->vol_ok) {
		sh->vol_icon_box = (struct wlr_box){tx + 17, 5, 20, 20};
		draw_surface(cr, (sh->vol_muted ? sh->vol_icon_muted : sh->vol_icon).s16, tx + 19, 7);
	}
	draw_text(cr, "Tahoma 8", sh->clock, tx + trayw - 12 - cw, 8, 0, 0xffffff, 0, 0, 0, true);

	/* task buttons */
	int x0 = START_W + 8, avail = tx - x0 - 6;
	int bw = sh->n_tasks ? avail / sh->n_tasks : 0;
	if (bw > 160)
		bw = 160;
	for (int i = 0; i < sh->n_tasks; i++) {
		struct task *k = &sh->tasks[i];
		k->box = (struct wlr_box){x0 + i * (bw + 2), 3, bw, 25};
		bool act = k->view->active && !k->view->minimized, hot = i == sh->task_hot;
		/* toolbar states: normal hot pressed disabled checked hotchecked */
		int f = act ? (hot ? 5 : 4) : (hot ? 1 : 0);
		draw_image(cr, &t->task_button, f, k->box.x, k->box.y, k->box.width, k->box.height);
		draw_surface(cr, k->view->icon.s16, k->box.x + 8, k->box.y + 5);
		const char *title = k->view->xt->title && *k->view->xt->title ? k->view->xt->title : "Untitled";
		draw_text(cr, act ? "Tahoma Bold 8" : "Tahoma 8", title, k->box.x + 28, k->box.y + 6,
			k->box.width - 34, 0xffffff, 0, 0, 0, true);
	}
	cairo_destroy(cr);
	show(&sh->taskbar, sh->server->layer_shell, s, 0, sh->h - bar_h(sh));
}

void shell_views_changed(struct shell *sh) {
	if (!sh)
		return;
	sh->n_tasks = 0;
	struct view *v;
	wl_list_for_each(v, &sh->server->views, link)
		if (v->mapped && sh->n_tasks < 64)
			sh->tasks[sh->n_tasks++] = (struct task){v, {0}};
	if (sh->task_hot >= sh->n_tasks)
		sh->task_hot = -1;
	draw_taskbar(sh);
}

static int clock_timer(void *data) {
	struct shell *sh = data;
	char old[16];
	memcpy(old, sh->clock, sizeof(old));
	tick(sh);
	if (strcmp(old, sh->clock))
		draw_taskbar(sh);
	wl_event_source_timer_update(sh->timer, 1000);
	return 0;
}

/* ---------------------------------------------------------------- volume */

static void draw_volpop(struct shell *sh) {
	if (!sh->vol_open) {
		if (sh->volpop)
			wlr_scene_node_set_enabled(&sh->volpop->node, false);
		return;
	}
	struct theme *t = &sh->server->theme;
	cairo_surface_t *s = canvas_new(VOL_W, VOL_H);
	cairo_t *cr = cairo_create(s);
	/* dialog face with a thin raised frame, like XP's tray volume pop-up */
	fill(cr, 0, 0, VOL_W, VOL_H, 0x716f64);
	fill(cr, 1, 1, VOL_W - 2, VOL_H - 2, 0xffffff);
	fill(cr, 2, 2, VOL_W - 3, VOL_H - 3, 0xaca899);
	fill(cr, 2, 2, VOL_W - 4, VOL_H - 4, 0xece9d8);
	int tw = text_width("Tahoma 8", "Volume");
	draw_text(cr, "Tahoma 8", "Volume", (VOL_W - tw) / 2, 9, 0, 0x000000, 0, 0, 0, true);

	int cx = VOL_W / 2;
	draw_image(cr, &t->slider_track, 0, cx - 2, VOL_TRACK_TOP, 5, VOL_TRACK_H);
	for (int i = 0; i <= 5; i++) {
		int y = VOL_TRACK_TOP + i * VOL_TRACK_H / 5;
		fill(cr, cx - 16, y, 4, 1, 0x000000);
		fill(cr, cx + 13, y, 4, 1, 0x000000);
	}
	int pct = sh->vol_pct > 100 ? 100 : sh->vol_pct;
	int ty = VOL_TRACK_TOP + (100 - pct) * VOL_TRACK_H / 100 - 5;
	/* thumb frames: normal hot pressed focused disabled */
	draw_image(cr, &t->slider_thumb, sh->vol_drag ? 2 : 0, cx - 10, ty, 21, 11);

	/* Mute checkbox: unchecked frames 0-3, checked 4-7 */
	draw_frame_true(cr, t->checkbox, 12, sh->vol_muted ? 4 : 0, 12, VOL_H - 28);
	draw_text(cr, "Tahoma 8", "Mute", 31, VOL_H - 28, 0, 0x000000, 0, 0, 0, true);
	cairo_destroy(cr);
	show(&sh->volpop, sh->server->layer_menu, s, sh->vol_box.x, sh->vol_box.y);
	wlr_scene_node_set_enabled(&sh->volpop->node, true);
}

static void vol_refresh(struct shell *sh) {
	volume_read(&sh->vol_pct, &sh->vol_muted);
}

static void vol_set_open(struct shell *sh, bool open) {
	sh->vol_open = open;
	sh->vol_drag = false;
	if (open) {
		vol_refresh(sh);
		int x = sh->vol_icon_box.x + sh->vol_icon_box.width / 2 - VOL_W / 2;
		if (x + VOL_W > sh->w)
			x = sh->w - VOL_W;
		sh->vol_box = (struct wlr_box){x, sh->h - bar_h(sh) - VOL_H - 2, VOL_W, VOL_H};
	}
	draw_volpop(sh);
	draw_taskbar(sh);
}

static void vol_drag_to(struct shell *sh, double y) {
	int pct = 100 - (int)((y - sh->vol_box.y - VOL_TRACK_TOP) * 100 / VOL_TRACK_H + 0.5);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	if (pct != sh->vol_pct) {
		sh->vol_pct = pct;
		volume_write(pct);
	}
	draw_volpop(sh);
}

/* true when the pop-up used the event */
static bool vol_button(struct shell *sh, double x, double y, bool pressed) {
	if (sh->vol_drag && !pressed) {
		sh->vol_drag = false;
		draw_volpop(sh);
		sound_play(sh->server, "ding"); /* XP's Default Beep when you let go of the slider */
		return true;
	}
	if (!in_box(&sh->vol_box, x, y))
		return false;
	if (!pressed)
		return true;
	double lx = x - sh->vol_box.x, ly = y - sh->vol_box.y;
	if (ly >= VOL_H - 32 && ly < VOL_H - 10) {
		sh->vol_muted = !sh->vol_muted;
		volume_mute(sh->vol_muted);
		draw_volpop(sh);
		draw_taskbar(sh);
	} else if (lx >= VOL_W / 2 - 16 && lx <= VOL_W / 2 + 16 && ly >= VOL_TRACK_TOP - 8 && ly <= VOL_TRACK_TOP + VOL_TRACK_H + 8) {
		sh->vol_drag = true;
		vol_drag_to(sh, y);
	}
	return true;
}

void shell_volume_key(struct shell *sh, int step, bool toggle_mute) {
	if (!sh->vol_ok)
		return;
	vol_refresh(sh);
	if (toggle_mute) {
		sh->vol_muted = !sh->vol_muted;
		volume_mute(sh->vol_muted);
	} else {
		volume_step(step);
		if (sh->vol_muted) {
			sh->vol_muted = false;
			volume_mute(false);
		}
		vol_refresh(sh);
		sound_play(sh->server, "ding");
	}
	draw_volpop(sh);
	draw_taskbar(sh);
}

/* ---------------------------------------------------------------- start menu */

static void add_item(struct shell *sh, enum item_kind kind, const char *label, const char *icon, const char *cmd) {
	struct menu_item *m = &sh->items[sh->n_items++];
	m->kind = kind;
	m->label = label;
	m->icon = icon;
	m->cmd = cmd;
	if (icon)
		m->img = theme_icon(&sh->server->theme, icon);
}

static int bottom_band(struct shell *sh) {
	return MORE_H + (sh->modern ? SEARCH_H : 0);
}

/* menu-relative area the modern app list fills */
static struct wlr_box list_box(struct shell *sh) {
	int top = USER_H + 6;
	return (struct wlr_box){6, top, COL_W - 15, sh->menu_h - LOGOFF_H - bottom_band(sh) - top - 2};
}

static struct wlr_box search_box(struct shell *sh) {
	return (struct wlr_box){8, sh->menu_h - LOGOFF_H - SEARCH_H + 4, COL_W - 17, 22};
}

/* the modern left column shows the app list instead of pinned/recent items */
static bool list_mode(struct shell *sh) {
	return sh->modern && (sh->all_mode || sh->query[0]);
}

static void update_results(struct shell *sh) {
	int n;
	struct app *apps = apps_all(&n);
	sh->results = realloc(sh->results, (n ? n : 1) * sizeof(int));
	sh->n_results = 0;
	for (int i = 0; i < n; i++)
		if (app_matches(&apps[i], sh->query))
			sh->results[sh->n_results++] = i;
	sh->scroll = 0;
	sh->list_sel = sh->query[0] && sh->n_results ? 0 : -1;
	sh->list_hot = -1;
}

static void layout_menu(struct shell *sh) {
	int ly = USER_H + 6, ry = USER_H + 8;
	int mh = sh->menu_h;
	for (int i = 0; i < sh->n_items; i++) {
		struct menu_item *m = &sh->items[i];
		switch (m->kind) {
		case ITEM_PINNED: case ITEM_MFU:
			m->box = (struct wlr_box){6, ly, COL_W - 15, 38};
			ly += 38;
			break;
		case ITEM_SEP_LEFT:
			m->box = (struct wlr_box){6, ly, COL_W - 15, 9};
			ly += 9;
			break;
		case ITEM_PLACE_BOLD: case ITEM_PLACE:
			m->box = (struct wlr_box){COL_W + 4, ry, COL_W - 13, 30};
			ry += 30;
			break;
		case ITEM_SEP_RIGHT:
			m->box = (struct wlr_box){COL_W + 4, ry, COL_W - 13, 9};
			ry += 9;
			break;
		case ITEM_ALL_PROGRAMS:
			m->box = (struct wlr_box){6, mh - LOGOFF_H - bottom_band(sh) + 3, COL_W - 15, MORE_H - 8};
			break;
		case ITEM_LOGOFF:
			m->box = (struct wlr_box){MENU_W - 222, mh - LOGOFF_H + 7, 76, 26};
			break;
		case ITEM_TURNOFF:
			m->box = (struct wlr_box){MENU_W - 140, mh - LOGOFF_H + 7, 134, 26};
			break;
		}
	}
}

static bool item_shown(struct shell *sh, struct menu_item *m) {
	if (m->kind == ITEM_SEP_LEFT || m->kind == ITEM_SEP_RIGHT)
		return false;
	if (list_mode(sh) && (m->kind == ITEM_PINNED || m->kind == ITEM_MFU))
		return false;
	return true;
}

static void draw_logoff_glyph(cairo_t *cr, cairo_surface_t *s, int frame, double x, double y) {
	struct image img = {.s = s, .count = 3, .vertical = false};
	draw_image(cr, &img, frame, x, y, 24, 24);
}

static void draw_app_list(struct shell *sh, cairo_t *cr) {
	struct theme *t = &sh->server->theme;
	struct wlr_box lb = list_box(sh);
	int n;
	struct app *apps = apps_all(&n);
	cairo_save(cr);
	cairo_rectangle(cr, lb.x, lb.y, lb.width, lb.height);
	cairo_clip(cr);
	if (!sh->n_results)
		draw_text(cr, "Tahoma 8", "No items match your search.", lb.x + 8, lb.y + 8, lb.width - 12,
			0x808080, 0, 0, 0, true);
	int rows = lb.height / ROW_H;
	for (int r = 0; r < rows + 1 && sh->scroll + r < sh->n_results; r++) {
		int idx = sh->scroll + r;
		struct app *a = &apps[sh->results[idx]];
		int y = lb.y + r * ROW_H;
		bool hl = idx == sh->list_hot || (sh->list_hot < 0 && idx == sh->list_sel);
		if (hl)
			fill(cr, lb.x, y, lb.width, ROW_H, 0x3070d0);
		draw_scaled(cr, app_icon(t, a), lb.x + 4, y + 2, 20);
		draw_text(cr, "Tahoma 8", a->name, lb.x + 30, y + 5, lb.width - 34, hl ? 0xffffff : 0x373738, 0, 0, 0, true);
	}
	cairo_restore(cr);
	/* scroll hint: a thin XP-blue bar on the right edge */
	if (sh->n_results > rows && rows > 0) {
		double frac = (double)rows / sh->n_results, pos = (double)sh->scroll / sh->n_results;
		fill(cr, lb.x + lb.width - 4, lb.y + pos * lb.height, 3, frac * lb.height, 0x7ba2e7);
	}
}

static void draw_search(struct shell *sh, cairo_t *cr) {
	struct wlr_box b = search_box(sh);
	/* XP text field: white with the Luna blue-grey outline */
	fill(cr, b.x, b.y, b.width, b.height, 0x7f9db9);
	fill(cr, b.x + 1, b.y + 1, b.width - 2, b.height - 2, 0xffffff);
	if (sh->query[0]) {
		int w = draw_text(cr, "Tahoma 8", sh->query, b.x + 5, b.y + 4, b.width - 12, 0x000000, 0, 0, 0, true);
		fill(cr, b.x + 6 + (w < b.width - 12 ? w : b.width - 12), b.y + 4, 1, 13, 0x000000);
	} else {
		draw_text(cr, "Tahoma, Italic 8", "Start Search", b.x + 5, b.y + 4, 0, 0x8d8d8d, 0, 0, 0, true);
		fill(cr, b.x + 4, b.y + 4, 1, 13, 0x000000);
	}
}

static void draw_menu(struct shell *sh) {
	struct theme *t = &sh->server->theme;
	int mh = sh->menu_h;
	cairo_surface_t *s = canvas_new(MENU_W, mh);
	cairo_t *cr = cairo_create(s);
	int body_h = mh - USER_H - LOGOFF_H;
	draw_image(cr, &t->prog_list, 0, 0, USER_H, COL_W, body_h - bottom_band(sh));
	draw_image(cr, &t->more_progs, 0, 0, mh - LOGOFF_H - bottom_band(sh), COL_W, bottom_band(sh));
	draw_image(cr, &t->places_list, 0, COL_W, USER_H, MENU_W - COL_W, body_h);
	draw_image(cr, &t->user_pane, 0, 0, 0, MENU_W, USER_H);
	draw_image(cr, &t->logoff, 0, 0, mh - LOGOFF_H, MENU_W, LOGOFF_H);

	/* user tile and name */
	draw_image(cr, &t->user_tile, 0, 5, 5, 56, 56);
	if (t->user_pic)
		draw_scaled(cr, t->user_pic, 9, 9, 48);
	const char *user = getenv("USER") ? getenv("USER") : "User";
	draw_text(cr, "Franklin Gothic Medium 14", user, 70, 19, 0, 0xffffff, 1, 1, 0x09428b, false);

	if (list_mode(sh))
		draw_app_list(sh, cr);
	if (sh->modern)
		draw_search(sh, cr);

	for (int i = 0; i < sh->n_items; i++) {
		struct menu_item *m = &sh->items[i];
		struct wlr_box b = m->box;
		bool hot = i == sh->menu_hot;
		if (list_mode(sh) && (m->kind == ITEM_PINNED || m->kind == ITEM_MFU || m->kind == ITEM_SEP_LEFT))
			continue;
		switch (m->kind) {
		case ITEM_PINNED: case ITEM_MFU:
			if (hot)
				fill(cr, b.x, b.y, b.width, b.height, 0x3070d0);
			draw_surface(cr, m->img.s32, b.x + 4, b.y + 3);
			draw_text(cr, m->kind == ITEM_PINNED ? "Tahoma Bold 8" : "Tahoma 8", m->label, b.x + 42, b.y + 12,
				b.width - 44, hot ? 0xffffff : 0x373738, 0, 0, 0, true);
			break;
		case ITEM_SEP_LEFT:
			if (t->prog_sep)
				draw_image(cr, &(struct image){.s = t->prog_sep, .count = 1}, 0, b.x, b.y + 3, b.width, cairo_image_surface_get_height(t->prog_sep));
			break;
		case ITEM_SEP_RIGHT:
			if (t->places_sep)
				draw_image(cr, &(struct image){.s = t->places_sep, .count = 1}, 0, b.x, b.y + 3, b.width, cairo_image_surface_get_height(t->places_sep));
			break;
		case ITEM_PLACE_BOLD: case ITEM_PLACE:
			if (hot)
				fill(cr, b.x, b.y, b.width, b.height, 0x3070d0);
			draw_scaled(cr, m->img.s32, b.x + 3, b.y + 3, 24);
			draw_text(cr, m->kind == ITEM_PLACE_BOLD ? "Tahoma Bold 8" : "Tahoma 8", m->label, b.x + 32, b.y + 8,
				b.width - 34, hot ? 0xffffff : 0x0a246a, 0, 0, 0, true);
			break;
		case ITEM_ALL_PROGRAMS: {
			bool lit = hot || sh->cascade_open;
			if (lit)
				fill(cr, b.x, b.y, b.width, b.height, 0x3070d0);
			const char *label = sh->modern && list_mode(sh) ? "Back" : m->label;
			int tw = text_width("Tahoma Bold 8", label);
			int tx = b.x + b.width - tw - 32;
			draw_text(cr, "Tahoma Bold 8", label, tx, b.y + 4, 0, lit ? 0xffffff : 0x373738, 0, 0, 0, true);
			cairo_surface_t *arrow = lit ? t->more_arrow_hot : t->more_arrow;
			if (sh->modern && list_mode(sh)) {
				/* the same green arrow, pointing back */
				cairo_save(cr);
				cairo_translate(cr, b.x + 14 + 21, b.y + 2);
				cairo_scale(cr, -1, 1);
				draw_surface(cr, arrow, 0, 0);
				cairo_restore(cr);
			} else {
				/* the bitmap carries padding; at 3/4 size it sits inside the row like XP's */
				cairo_save(cr);
				cairo_translate(cr, tx + tw + 6, b.y + (b.height - 18) / 2.0);
				cairo_scale(cr, 0.75, 0.75);
				draw_surface(cr, arrow, 0, 0);
				cairo_restore(cr);
			}
			break;
		}
		case ITEM_LOGOFF: case ITEM_TURNOFF:
			if (hot) {
				cairo_set_source_rgba(cr, 1, 1, 1, 0.18);
				cairo_rectangle(cr, b.x, b.y, b.width, b.height);
				cairo_fill(cr);
			}
			draw_logoff_glyph(cr, hot ? t->logoff_buttons_hot : t->logoff_buttons, m->kind == ITEM_LOGOFF ? 1 : 2, b.x + 1, b.y + 1);
			draw_text(cr, "Tahoma 8", m->label, b.x + 29, b.y + 6, 0, 0xffffff, 0, 0, 0, true);
			break;
		}
	}
	cairo_destroy(cr);
	show(&sh->menu, sh->server->layer_menu, s, 0, sh->h - bar_h(sh) - mh);
	wlr_scene_node_set_enabled(&sh->menu->node, sh->start_open);
}

/* ---- classic All Programs cascade, XP's Luna menu look ---- */

static void draw_cascade(struct shell *sh) {
	if (!sh->cascade_open) {
		if (sh->cascade)
			wlr_scene_node_set_enabled(&sh->cascade->node, false);
		return;
	}
	struct theme *t = &sh->server->theme;
	int n;
	struct app *apps = apps_all(&n);
	struct wlr_box cb = sh->cascade_box;
	cairo_surface_t *s = canvas_new(cb.width, cb.height);
	cairo_t *cr = cairo_create(s);
	fill(cr, 0, 0, cb.width, cb.height, 0xaca899);
	fill(cr, 1, 1, cb.width - 2, cb.height - 2, 0xffffff);
	if (!n)
		draw_text(cr, "Tahoma, Italic 8", "(Empty)", 28, 4, 0, 0x808080, 0, 0, 0, true);
	for (int i = 0; i < n; i++) {
		int col = i / sh->cascade_rows, row = i % sh->cascade_rows;
		int x = 1 + col * CASCADE_COL, y = 1 + row * CASCADE_ROW;
		bool hot = i == sh->cascade_hot;
		if (hot)
			fill(cr, x, y, CASCADE_COL, CASCADE_ROW, 0x316ac5);
		draw_scaled(cr, app_icon(t, &apps[i]), x + 4, y + 3, 16);
		draw_text(cr, "Tahoma 8", apps[i].name, x + 28, y + 4, CASCADE_COL - 34, hot ? 0xffffff : 0x000000, 0, 0, 0, true);
	}
	cairo_destroy(cr);
	show(&sh->cascade, sh->server->layer_menu, s, cb.x, cb.y);
	wlr_scene_node_set_enabled(&sh->cascade->node, true);
}

static void open_cascade(struct shell *sh, struct wlr_box anchor /* layout coords */) {
	int n;
	apps_all(&n);
	int max_rows = (anchor.y + anchor.height - 2) / CASCADE_ROW;
	if (max_rows < 1)
		max_rows = 1;
	int count = n ? n : 1;
	int cols = (count + max_rows - 1) / max_rows;
	sh->cascade_rows = (count + cols - 1) / cols;
	int w = cols * CASCADE_COL + 2, h = sh->cascade_rows * CASCADE_ROW + 2;
	int x = anchor.x + anchor.width - 2;
	if (x + w > sh->w)
		x = sh->w - w;
	sh->cascade_box = (struct wlr_box){x, anchor.y + anchor.height - h, w, h};
	sh->cascade_open = true;
	sh->cascade_hot = -1;
	draw_cascade(sh);
}

static void close_cascade(struct shell *sh) {
	if (!sh->cascade_open)
		return;
	sh->cascade_open = false;
	draw_cascade(sh);
}

bool shell_start_open(struct shell *sh) {
	return sh->start_open;
}

void shell_toggle_start(struct shell *sh) {
	sh->start_open = !sh->start_open;
	sh->menu_hot = -1;
	sh->query[0] = 0;
	sh->all_mode = false;
	if (sh->start_open)
		apps_load(); /* pick up anything installed since last time */
	update_results(sh);
	close_cascade(sh);
	draw_menu(sh);
	draw_taskbar(sh);
}

/* ---------------------------------------------------------------- events */

static struct wlr_box menu_box(struct shell *sh) {
	return (struct wlr_box){0, sh->h - bar_h(sh) - sh->menu_h, MENU_W, sh->menu_h};
}

static int list_index_at(struct shell *sh, double mx, double my) {
	struct wlr_box lb = list_box(sh);
	if (!list_mode(sh) || !in_box(&lb, mx, my))
		return -1;
	int idx = sh->scroll + (int)(my - lb.y) / ROW_H;
	return idx < sh->n_results ? idx : -1;
}

static int cascade_index_at(struct shell *sh, double x, double y) {
	if (!sh->cascade_open || !in_box(&sh->cascade_box, x, y))
		return -1;
	int n;
	apps_all(&n);
	int col = (int)(x - sh->cascade_box.x - 1) / CASCADE_COL, row = (int)(y - sh->cascade_box.y - 1) / CASCADE_ROW;
	if (row >= sh->cascade_rows)
		return -1;
	int i = col * sh->cascade_rows + row;
	return i < n ? i : -1;
}

static void launch_app(struct shell *sh, int app_index) {
	int n;
	struct app *apps = apps_all(&n);
	if (app_index < 0 || app_index >= n)
		return;
	char *cmd = strdup(apps[app_index].exec);
	shell_toggle_start(sh);
	spawn(cmd);
	free(cmd);
}

bool shell_motion(struct shell *sh, double x, double y, bool over_view) {
	(void)over_view;
	if (sh->vol_drag) {
		vol_drag_to(sh, y);
		return true;
	}
	if (sh->vol_open && in_box(&sh->vol_box, x, y))
		return true;
	if (sh->start_open) {
		struct wlr_box mb = menu_box(sh);
		if (sh->cascade_open && in_box(&sh->cascade_box, x, y)) {
			int h = cascade_index_at(sh, x, y);
			if (h != sh->cascade_hot) {
				sh->cascade_hot = h;
				draw_cascade(sh);
			}
			return true;
		}
		int hot = -1;
		if (in_box(&mb, x, y))
			for (int i = 0; i < sh->n_items; i++)
				if (item_shown(sh, &sh->items[i]) && in_box(&sh->items[i].box, x - mb.x, y - mb.y))
					hot = i;
		int lh = list_index_at(sh, x - mb.x, y - mb.y);
		if (hot != sh->menu_hot || lh != sh->list_hot) {
			sh->menu_hot = hot;
			sh->list_hot = lh;
			draw_menu(sh);
		}
		/* classic: hovering All Programs opens the cascade; other items close it */
		if (!sh->modern && in_box(&mb, x, y)) {
			bool on_all = hot >= 0 && sh->items[hot].kind == ITEM_ALL_PROGRAMS;
			if (on_all && !sh->cascade_open) {
				struct wlr_box a = sh->items[hot].box;
				a.x += mb.x;
				a.y += mb.y;
				open_cascade(sh, a);
				draw_menu(sh);
			} else if (!on_all && hot >= 0 && sh->cascade_open) {
				close_cascade(sh);
				draw_menu(sh);
			}
		}
		if (in_box(&mb, x, y))
			return true;
	}
	bool on_bar = y >= sh->h - bar_h(sh);
	int task_hot = -1;
	bool start_hot = false;
	if (on_bar) {
		double by = y - (sh->h - bar_h(sh));
		start_hot = x < start_w(sh);
		for (int i = 0; i < sh->n_tasks; i++)
			if (in_box(&sh->tasks[i].box, x, by))
				task_hot = i;
	}
	if (task_hot != sh->task_hot || start_hot != sh->start_hot) {
		sh->task_hot = task_hot;
		sh->start_hot = start_hot;
		draw_taskbar(sh);
	}
	return on_bar;
}

static void activate_item(struct shell *sh, struct menu_item *m) {
	if (m->kind == ITEM_ALL_PROGRAMS) {
		if (sh->modern) {
			bool back = list_mode(sh);
			sh->all_mode = !back;
			sh->query[0] = 0;
			update_results(sh);
			draw_menu(sh);
		}
		return;
	}
	shell_toggle_start(sh);
	if (m->kind == ITEM_LOGOFF) {
		session_logoff(sh->server->session);
		return;
	}
	if (m->kind == ITEM_TURNOFF) {
		session_turnoff_dialog(sh->server->session);
		return;
	}
	if (m->cmd)
		spawn(m->cmd);
}

bool shell_button(struct shell *sh, double x, double y, bool pressed, bool dbl, bool over_view) {
	struct wlr_box mb = menu_box(sh);
	bool on_bar = y >= sh->h - bar_h(sh);
	if (sh->vol_open || sh->vol_drag) {
		if (vol_button(sh, x, y, pressed))
			return true;
		/* a click anywhere else closes the pop-up and still does what it would */
		if (pressed)
			vol_set_open(sh, false);
		if (on_bar && in_box(&sh->vol_icon_box, x, y - (sh->h - bar_h(sh))))
			return true;
	} else if (pressed && on_bar && sh->vol_ok && in_box(&sh->vol_icon_box, x, y - (sh->h - bar_h(sh)))) {
		if (sh->start_open)
			shell_toggle_start(sh);
		vol_set_open(sh, true);
		return true;
	}
	if (sh->start_open) {
		if (sh->cascade_open && in_box(&sh->cascade_box, x, y)) {
			if (!pressed)
				launch_app(sh, cascade_index_at(sh, x, y));
			return true;
		}
		if (in_box(&mb, x, y)) {
			if (!pressed) {
				int li = list_index_at(sh, x - mb.x, y - mb.y);
				if (li >= 0) {
					launch_app(sh, sh->results[li]);
					return true;
				}
				for (int i = 0; i < sh->n_items; i++)
					if (item_shown(sh, &sh->items[i]) && in_box(&sh->items[i].box, x - mb.x, y - mb.y)) {
						activate_item(sh, &sh->items[i]);
						break;
					}
			}
			return true;
		}
		/* any click outside closes the menu; the start button's own click toggles it below */
		if (pressed && !(on_bar && x < start_w(sh))) {
			shell_toggle_start(sh);
			return true;
		}
	}
	if (on_bar) {
		if (!pressed)
			return true;
		if (x < start_w(sh)) {
			shell_toggle_start(sh);
			return true;
		}
		double by = y - (sh->h - bar_h(sh));
		for (int i = 0; i < sh->n_tasks; i++)
			if (in_box(&sh->tasks[i].box, x, by)) {
				struct view *v = sh->tasks[i].view;
				if (v->active && !v->minimized)
					view_set_minimized(v, true);
				else
					view_focus(v);
			}
		return true;
	}
	if (over_view)
		return false;
	/* the desktop */
	if (pressed) {
		int sel = -1;
		for (int i = 0; i < sh->n_icons; i++)
			if (in_box(&sh->icons[i].box, x, y))
				sel = i;
		if (sel != sh->icon_sel) {
			sh->icon_sel = sel;
			draw_desktop(sh);
		}
		if (dbl && sel >= 0 && sh->icons[sel].cmd)
			spawn(sh->icons[sel].cmd);
		view_focus(NULL);
	}
	return true;
}

static void keep_sel_visible(struct shell *sh) {
	int rows = list_box(sh).height / ROW_H;
	if (sh->list_sel < sh->scroll)
		sh->scroll = sh->list_sel;
	else if (sh->list_sel >= sh->scroll + rows)
		sh->scroll = sh->list_sel - rows + 1;
}

bool shell_key(struct shell *sh, uint32_t sym, const char *utf8) {
	if (!sh->start_open)
		return false;
	if (sym == XKB_KEY_Escape) {
		if (sh->modern && list_mode(sh)) {
			sh->query[0] = 0;
			sh->all_mode = false;
			update_results(sh);
			draw_menu(sh);
		} else {
			shell_toggle_start(sh);
		}
		return true;
	}
	if (!sh->modern)
		return true; /* the menu has the keyboard while it's open */
	size_t len = strlen(sh->query);
	switch (sym) {
	case XKB_KEY_BackSpace:
		while (len && (sh->query[len - 1] & 0xc0) == 0x80)
			len--; /* UTF-8 continuation bytes */
		if (len)
			sh->query[len - 1] = 0;
		update_results(sh);
		break;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		if (list_mode(sh) && sh->n_results)
			launch_app(sh, sh->results[sh->list_sel >= 0 ? sh->list_sel : 0]);
		return true;
	case XKB_KEY_Down:
		if (list_mode(sh) && sh->list_sel + 1 < sh->n_results)
			sh->list_sel++;
		keep_sel_visible(sh);
		sh->list_hot = -1;
		break;
	case XKB_KEY_Up:
		if (list_mode(sh) && sh->list_sel > 0)
			sh->list_sel--;
		keep_sel_visible(sh);
		sh->list_hot = -1;
		break;
	default:
		if (utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f && len + strlen(utf8) < sizeof(sh->query) - 1) {
			strcat(sh->query, utf8);
			update_results(sh);
		} else {
			return true;
		}
	}
	draw_menu(sh);
	return true;
}

bool shell_axis(struct shell *sh, double x, double y, double delta) {
	if (!sh->start_open)
		return false;
	struct wlr_box mb = menu_box(sh);
	if (!in_box(&mb, x, y) && !(sh->cascade_open && in_box(&sh->cascade_box, x, y)))
		return false;
	if (list_mode(sh)) {
		int rows = list_box(sh).height / ROW_H;
		int max = sh->n_results > rows ? sh->n_results - rows : 0;
		sh->scroll += delta > 0 ? 3 : -3;
		if (sh->scroll > max) sh->scroll = max;
		if (sh->scroll < 0) sh->scroll = 0;
		sh->list_hot = list_index_at(sh, x - mb.x, y - mb.y);
		draw_menu(sh);
	}
	return true;
}

/* ---------------------------------------------------------------- setup */

void shell_reconfigure(struct shell *sh) {
	if (sh->start_open)
		shell_toggle_start(sh);
	sh->modern = sh->server->opt_modern_start;
	sh->menu_h = MENU_H + (sh->modern ? SEARCH_H : 0);
	layout_menu(sh);
	tick(sh);
	draw_desktop(sh);
	draw_taskbar(sh);
	draw_menu(sh);
}

void shell_layout(struct shell *sh) {
	server_screen(sh->server, &sh->w, &sh->h);
	draw_desktop(sh);
	draw_taskbar(sh);
	draw_menu(sh);
}

struct shell *shell_create(struct server *s) {
	struct shell *sh = calloc(1, sizeof(*sh));
	sh->server = s;
	sh->menu_hot = sh->task_hot = sh->icon_sel = sh->list_hot = sh->list_sel = sh->cascade_hot = -1;
	sh->modern = s->opt_modern_start;
	sh->menu_h = MENU_H + (sh->modern ? SEARCH_H : 0);
	tick(sh);
	apps_load();
	update_results(sh);
	sh->vol_icon = theme_icon(&s->theme, "volume");
	sh->vol_icon_muted = theme_icon(&s->theme, "volume-muted");
	sh->vol_ok = volume_available() && sh->vol_icon.s16;
	if (sh->vol_ok)
		vol_refresh(sh);

	add_icon(sh, "My Documents", getenv("USER"), "my-documents", "expede-explorer ~");
	add_icon(sh, "My Computer", "Computer", "computer", "expede-explorer computer:");
	add_icon(sh, "My Network Places", "Network", "network", "pcmanfm-qt network:///");
	add_icon(sh, "Recycle Bin", NULL, "recycle-empty", "expede-explorer trash:");

	add_item(sh, ITEM_PINNED, "Command Prompt", "cmd", "foot");
	add_item(sh, ITEM_PINNED, "Windows Explorer", "explorer", "expede-explorer ~");
	add_item(sh, ITEM_SEP_LEFT, NULL, NULL, NULL);
	add_item(sh, ITEM_MFU, "Notepad", "notepad", "mousepad");
	add_item(sh, ITEM_ALL_PROGRAMS, "All Programs", NULL, NULL);
	add_item(sh, ITEM_PLACE_BOLD, "My Documents", "my-documents", "expede-explorer ~");
	add_item(sh, ITEM_PLACE_BOLD, "My Pictures", "my-pictures", "expede-explorer ~/Pictures");
	add_item(sh, ITEM_PLACE_BOLD, "My Music", "my-music", "expede-explorer ~/Music");
	add_item(sh, ITEM_PLACE_BOLD, "My Computer", "computer", "expede-explorer computer:");
	add_item(sh, ITEM_SEP_RIGHT, NULL, NULL, NULL);
	add_item(sh, ITEM_PLACE, "Control Panel", "control-panel", "expede-settings");
	add_item(sh, ITEM_PLACE, "Help and Support", "help", NULL);
	add_item(sh, ITEM_PLACE, "Search", "search", "expede-explorer ~");
	add_item(sh, ITEM_PLACE, "Run...", "run", "foot");
	add_item(sh, ITEM_LOGOFF, "Log Off", NULL, NULL);
	add_item(sh, ITEM_TURNOFF, "Turn Off Computer", NULL, NULL);
	layout_menu(sh);


	sh->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display), clock_timer, sh);
	wl_event_source_timer_update(sh->timer, 1000);
	return sh;
}
