/* The XP shell: Bliss desktop with icons, the Luna taskbar, and the two-column
 * Start menu. Each is one cairo-drawn buffer in the scene graph, redrawn when
 * its state changes.
 *
 * The Start menu has two styles. Classic is XP's: All Programs opens a cascade
 * of every installed app. Modern (start_menu = modern) adds a search box at the
 * bottom of the left column: typing filters apps, and All Programs swaps the
 * left column for a scrolling list. */
#define _GNU_SOURCE
#include <dirent.h>
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
	struct wlr_scene_buffer *other_desk[4]; /* wallpaper on the other screens */
	struct wlr_scene_buffer *bar_glass, *menu_glass;
	char date[24];
	int bat_pct; /* -1: no battery */
	bool bat_charging;
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
	/* the tray speaker and Volume / Battery pop-ups */
	bool vol_ok, vol_open, vol_drag, vol_muted;
	int vol_pct;
	struct wlr_box vol_icon_box; /* taskbar coords */
	struct wlr_box vol_box;      /* layout coords */
	struct icon_set vol_icon, vol_icon_muted, vol_icon_0, vol_icon_1, vol_icon_2, sndvol_icon;
	struct wlr_scene_buffer *volpop;
	bool bat_open, bright_drag, bright_hw;
	int bright_pct;
	struct wlr_box bat_icon_box; /* taskbar coords */
	/* Windows 10: the search box, Task View and Action Center buttons (taskbar coords) */
	struct wlr_box search_box10, taskview_box, action_box;
	bool search_hot, taskview_hot, action_hot;
	/* Task View */
	bool tv_open;
	struct wlr_scene_buffer *tv_node;
	struct { struct view *view; cairo_surface_t *shot; struct wlr_box box, close; } tv[32];
	int n_tv, tv_hot;
	bool tv_close_hot;
	/* Action Center */
	bool action_open;
	struct wlr_scene_buffer *ac_node, *ac_glass;
	struct wlr_box ac_tiles[8], ac_bright;
	bool ac_on[8];
	int ac_hot, ac_bright_pct;
	bool ac_bright_drag;
	/* Windows 10 Start */
	struct hit10 { struct wlr_box b; int id; } hits10[160];
	int n_hits10, hot10, scroll10;
	bool rail_open, power_menu10, user_menu10;
	struct icon_set tile_img[6];
	struct theme *tile_theme;
	struct wlr_box bat_box;      /* layout coords */
	struct wlr_scene_buffer *batpop;
	struct wlr_scene_rect *bright_veil;
	bool net_open, net_up;
	int net_sig, net_hot, n_net_aps;
	struct { char ssid[64]; int sig; bool active, sec; } net_aps[8];
	struct wlr_box net_icon_box; /* taskbar coords */
	struct wlr_box net_box;      /* layout coords */
	struct wlr_scene_buffer *netpop;
};

#define VOL_W 72
#define VOL_H 160
#define VOL_TRACK_TOP 30
#define VOL_TRACK_H 88
#define W7_VOL_W 86
#define W7_VOL_H 228
#define W7_VOL_TOP 66
#define W7_VOL_TH 96
#define BAT_POP_W 216
#define BAT_SLIDER_X 36
#define BAT_SLIDER_W 164
#define NET_POP_W 236
#define NET_ROW_H 26

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
static bool bar7(struct shell *sh) { return bar_theme(sh)->version >= 7; }
static bool menu7(struct shell *sh) { return menu_theme(sh)->version >= 7; }
static bool desk7(struct shell *sh) { return desk_theme(sh)->version >= 7; }
static int bar_h(struct shell *sh) { return taskbar_h(sh->server); }
static bool bar10(struct shell *sh) { return bar_theme(sh)->version == 10; }
static int start_w(struct shell *sh) { return bar10(sh) ? 48 : bar7(sh) ? W7_ORB + 2 : START_W; }
static bool menu10(struct shell *sh) { return menu_theme(sh)->version == 10; }
#define W10_MENU_W 660
static int menu_w(struct shell *sh) { return menu10(sh) ? W10_MENU_W : menu7(sh) ? W7_MENU_W : MENU_W; }

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
void blur_line(uint32_t *px, int n, int step, int r, uint32_t *tmp) {
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

void blur_surface(cairo_surface_t *g, int r) {
	cairo_surface_flush(g);
	int gw = cairo_image_surface_get_width(g), gh = cairo_image_surface_get_height(g);
	if (gw <= 0 || gh <= 0)
		return;
	uint32_t *px = (uint32_t *)cairo_image_surface_get_data(g);
	int st = cairo_image_surface_get_stride(g) / 4;
	uint32_t *tmp = malloc(sizeof(uint32_t) * (gw > gh ? gw : gh));
	for (int pass = 0; pass < 3; pass++) {
		for (int y = 0; y < gh; y++)
			blur_line(px + y * st, gw, 1, r, tmp);
		for (int x = 0; x < gw; x++)
			blur_line(px + x, gh, st, r, tmp);
	}
	free(tmp);
	cairo_surface_mark_dirty(g);
}

/* Aero glass shows what's behind it blurred; Expede pre-blurs the desktop picture
 * (wallpaper and icons) for fast fallback, and blurs overlapping windows on demand */
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
	blur_surface(g, 3);
	if (s->glass_buf)
		wlr_buffer_drop(s->glass_buf);
	s->glass_buf = buffer_from_cairo(g);
	server_glass_changed(s);
}

void shell_update_glass(struct shell *sh) {
	if (!sh)
		return;
	struct server *srv = sh->server;
	int bh = bar_h(sh);
	if (bar7(sh)) {
		glass_backdrop(srv, &sh->bar_glass, srv->layer_shell, 0, sh->h - bh, 0, sh->h - bh, sh->w, bh);
		if (sh->bar_glass && sh->taskbar)
			wlr_scene_node_place_below(&sh->bar_glass->node, &sh->taskbar->node);
	}
	if (menu7(sh) && sh->start_open) {
		glass_backdrop(srv, &sh->menu_glass, srv->layer_menu, 0, sh->h - bh - W7_MENU_H,
			0, sh->h - bh - W7_MENU_H, W7_MENU_W, W7_MENU_H);
		if (sh->menu_glass && sh->menu)
			wlr_scene_node_place_below(&sh->menu_glass->node, &sh->menu->node);
	}
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

/* first battery under /sys/class/power_supply, like the tray's power meter */
static void read_battery(struct shell *sh) {
	sh->bat_pct = -1;
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "BAT", 3) && strncmp(e->d_name, "CMB", 3))
			continue;
		char p[300];
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (fscanf(f, "%d", &sh->bat_pct) != 1)
			sh->bat_pct = -1;
		fclose(f);
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", e->d_name);
		char st[32] = "";
		if ((f = fopen(p, "r"))) {
			if (!fgets(st, sizeof(st), f))
				st[0] = 0;
			fclose(f);
		}
		sh->bat_charging = !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4);
		break;
	}
	if (d)
		closedir(d);
}

static void read_brightness(struct shell *sh) {
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;
	sh->bright_hw = false;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[300];
		int cur = 0, max = 0;
		snprintf(p, sizeof(p), "/sys/class/backlight/%s/brightness", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (fscanf(f, "%d", &cur) != 1) cur = 0;
			fclose(f);
		}
		snprintf(p, sizeof(p), "/sys/class/backlight/%s/max_brightness", e->d_name);
		if ((f = fopen(p, "r"))) {
			if (fscanf(f, "%d", &max) != 1) max = 0;
			fclose(f);
		}
		if (max > 0) {
			sh->bright_hw = true;
			if (!sh->bright_drag)
				sh->bright_pct = (cur * 100 + max / 2) / max;
			break;
		}
	}
	if (d)
		closedir(d);
	if (sh->bright_pct <= 0)
		sh->bright_pct = 100;
}

static void apply_brightness(struct shell *sh, int pct) {
	if (pct < 10) pct = 10;
	if (pct > 100) pct = 100;
	sh->bright_pct = pct;
	if (sh->bright_hw) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "brightnessctl -q set %d%%", pct);
		spawn(cmd);
	} else {
		float alpha = (100 - pct) * 0.78f / 90.0f;
		float col[4] = {0, 0, 0, alpha};
		if (!sh->bright_veil)
			sh->bright_veil = wlr_scene_rect_create(sh->server->layer_overlay, sh->w, sh->h, col);
		else {
			wlr_scene_rect_set_size(sh->bright_veil, sh->w, sh->h);
			wlr_scene_rect_set_color(sh->bright_veil, col);
		}
		wlr_scene_node_set_enabled(&sh->bright_veil->node, pct < 100);
	}
}

static void draw_sun(cairo_t *cr, double x, double y, uint32_t rgb) {
	static const double rays[8][2] = {
		{1, 0}, {0.7, 0.7}, {0, 1}, {-0.7, 0.7},
		{-1, 0}, {-0.7, -0.7}, {0, -1}, {0.7, -0.7},
	};
	double cx = x + 8, cy = y + 8;
	cairo_save(cr);
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
	cairo_set_line_width(cr, 1.4);
	cairo_arc(cr, cx, cy, 3.5, 0, 2 * M_PI);
	cairo_stroke(cr);
	for (int i = 0; i < 8; i++) {
		cairo_move_to(cr, cx + rays[i][0] * 5.2, cy + rays[i][1] * 5.2);
		cairo_line_to(cr, cx + rays[i][0] * 7.2, cy + rays[i][1] * 7.2);
	}
	cairo_stroke(cr);
	cairo_restore(cr);
}

/* a 16px battery: outline, fill to the charge (red when low), bolt when plugged in */
static void draw_battery(cairo_t *cr, struct shell *sh, double x, double y) {
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, x + 0.5, y + 4.5, 13, 8);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_stroke_preserve(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
	cairo_fill(cr);
	cairo_rectangle(cr, x + 14, y + 6.5, 2, 4);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
	int pct = sh->bat_pct > 100 ? 100 : sh->bat_pct;
	if (pct <= 15)
		cairo_set_source_rgb(cr, 0.9, 0.15, 0.1);
	else
		cairo_set_source_rgb(cr, 0.3, 0.85, 0.25);
	cairo_rectangle(cr, x + 2, y + 6, 10 * pct / 100.0, 5);
	cairo_fill(cr);
	if (sh->bat_charging) {
		cairo_move_to(cr, x + 8, y + 3);
		cairo_line_to(cr, x + 4.5, y + 9);
		cairo_line_to(cr, x + 7.5, y + 9);
		cairo_line_to(cr, x + 6, y + 14);
		cairo_line_to(cr, x + 10.5, y + 8);
		cairo_line_to(cr, x + 7.5, y + 8);
		cairo_close_path(cr);
		cairo_set_source_rgb(cr, 1, 0.85, 0.1);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0, 0, 0);
		cairo_stroke(cr);
	}
}

static void draw_wifi_icon(cairo_t *cr, struct shell *sh, double x, double y) {
	int bars = !sh->net_up ? 0 : sh->net_sig >= 75 ? 5 : sh->net_sig >= 55 ? 4 : sh->net_sig >= 35 ? 3 : sh->net_sig >= 15 ? 2 : 1;
	for (int i = 0; i < 5; i++) {
		int bh = 3 + i * 2;
		double bx = x + 1 + i * 3, by = y + 14 - bh;
		cairo_rectangle(cr, bx, by, 2, bh);
		if (i < bars)
			cairo_set_source_rgb(cr, 1, 1, 1);
		else
			cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
		cairo_fill(cr);
	}
}

static void read_net_fast(struct shell *sh) {
	sh->net_up = false;
	sh->net_sig = 0;
	FILE *f = fopen("/proc/net/wireless", "r");
	char line[256];
	if (f) {
		for (int n = 0; fgets(line, sizeof(line), f); n++) {
			float link = 0;
			if (n >= 2 && sscanf(line, " %*[^:]: %*x %f", &link) == 1 && link > 0) {
				sh->net_up = true;
				sh->net_sig = (int)(link * 100.0f / 70.0f + 0.5f);
				if (sh->net_sig > 100) sh->net_sig = 100;
				break;
			}
		}
		fclose(f);
	}
	if (!sh->net_up) {
		DIR *d = opendir("/sys/class/net");
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
				continue;
			char p[300], st[16] = "";
			snprintf(p, sizeof(p), "/sys/class/net/%s/operstate", e->d_name);
			if ((f = fopen(p, "r"))) {
				if (!fgets(st, sizeof(st), f)) st[0] = 0;
				fclose(f);
			}
			if (!strncmp(st, "up", 2)) {
				sh->net_up = true;
				sh->net_sig = 100;
				break;
			}
		}
		if (d)
			closedir(d);
	}
}

static void tick(struct shell *sh) {
	read_battery(sh);
	read_brightness(sh);
	read_net_fast(sh);
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
	tx -= 24;
	sh->bat_icon_box = (struct wlr_box){tx, 8, 24, 24};
	if (sh->bat_pct >= 0)
		draw_battery(cr, sh, tx + 4, 12);
	else
		draw_sun(cr, tx + 4, 12, 0xffffff);
	tx -= 24;
	sh->net_icon_box = (struct wlr_box){tx, 8, 24, 24};
	draw_wifi_icon(cr, sh, tx + 4, 12);
	if (sh->vol_ok) {
		tx -= 26;
		sh->vol_icon_box = (struct wlr_box){tx, 8, 24, 24};
		cairo_surface_t *vic = sh->vol_muted ? sh->vol_icon_muted.s16 :
			sh->vol_pct == 0 && sh->vol_icon_0.s16 ? sh->vol_icon_0.s16 :
			sh->vol_pct < 33 && sh->vol_icon_1.s16 ? sh->vol_icon_1.s16 :
			sh->vol_pct < 66 && sh->vol_icon_2.s16 ? sh->vol_icon_2.s16 : sh->vol_icon.s16;
		draw_surface(cr, vic, tx + 4, 12);
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

/* Windows 10's flag: four flat panes, the right ones a little taller (it leans in) */
static void draw_win10_logo(cairo_t *cr, double x, double y, double size, uint32_t rgb) {
	double g = size * 0.06, half = (size - g) / 2, lean = size * 0.08;
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
	double xs[2] = {x, x + half + g}, ys[2] = {y, y + half + g};
	for (int c = 0; c < 2; c++)
		for (int r = 0; r < 2; r++) {
			double x0 = xs[c], x1 = xs[c] + half;
			double l = lean * (1 - c * 1.0), rr = lean * (1 - c - 1.0) + lean;
			double t0 = ys[r] + (r ? 0 : lean - l * 0), b0 = ys[r] + half;
			(void)rr;
			/* left column sits lower at the top edge than the right one */
			double top_l = r ? ys[r] : y + lean * (c ? 0.5 : 1), top_r = r ? ys[r] : y + lean * (c ? 0 : 0.5);
			double bot_l = r ? y + size - lean * (c ? 0.5 : 1) : b0, bot_r = r ? y + size - lean * (c ? 0 : 0.5) : b0;
			(void)t0;
			cairo_move_to(cr, x0, top_l);
			cairo_line_to(cr, x1, top_r);
			cairo_line_to(cr, x1, bot_r);
			cairo_line_to(cr, x0, bot_l);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
}

/* Windows 10's taskbar: dark acrylic, the Start flag, the search box, Task View,
 * square icon buttons with a light line under running apps, the tray in MDL2 glyphs */
static void draw_taskbar10(struct shell *sh) {
	struct server *srv = sh->server;
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	int bh = bar_h(sh);
	bool dark = srv->opt_dark;
	uint32_t fg = dark ? 0xffffff : 0x000000;
	double hov = dark ? 1 : 0; /* hover tint: white on dark, black on light */
	cairo_surface_t *s = canvas_new(sh->w, bh);
	cairo_t *cr = cairo_create(s);
	bool acrylic = srv->opt_transparency && srv->glass_buf;
	if (dark)
		cairo_set_source_rgba(cr, 0.06, 0.06, 0.06, acrylic ? 0.85 : 1);
	else
		cairo_set_source_rgba(cr, 0.93, 0.93, 0.93, acrylic ? 0.85 : 1);
	cairo_paint(cr);

	/* Start */
	if (sh->start_open || sh->start_hot) {
		cairo_set_source_rgba(cr, hov, hov, hov, sh->start_open ? 0.15 : 0.1);
		cairo_rectangle(cr, 0, 0, 48, bh);
		cairo_fill(cr);
	}
	draw_win10_logo(cr, 16, (bh - 16) / 2.0, 16, sh->start_hot && !sh->start_open ? srv->accent_rgb : fg);
	/* "Type here to search" */
	int sw = sh->w >= 1100 ? 344 : 0;
	sh->search_box10 = (struct wlr_box){48, 0, sw, bh};
	if (sw) {
		cairo_set_source_rgb(cr, sh->search_hot ? 1 : 0.95, sh->search_hot ? 1 : 0.95, sh->search_hot ? 1 : 0.95);
		cairo_rectangle(cr, 48, 0, sw, bh);
		cairo_fill(cr);
		draw_mdl2(cr, "\uE721", 10, 48 + 22, bh / 2.0, 0x000000);
		if (sh->start_open && sh->query[0]) {
			/* what you type in Start shows here, with the caret */
			int qw = draw_text(cr, "Segoe UI 9", sh->query, 48 + 42, (bh - 16) / 2, sw - 50, 0x000000, 0, 0, 0, false);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, 48 + 43 + (qw < sw - 50 ? qw : sw - 50), 11, 1, bh - 22);
			cairo_fill(cr);
		} else {
			draw_text(cr, "Segoe UI 9", "Type here to search", 48 + 42, (bh - 16) / 2, sw - 50, 0x6b6b6b, 0, 0, 0, false);
		}
	} else {
		sh->search_box10 = (struct wlr_box){48, 0, 48, bh};
		if (sh->search_hot) {
			cairo_set_source_rgba(cr, hov, hov, hov, 0.1);
			cairo_rectangle(cr, 48, 0, 48, bh);
			cairo_fill(cr);
		}
		draw_mdl2(cr, "\uE721", 11, 48 + 24, bh / 2.0, fg);
	}
	int x = 48 + sh->search_box10.width;
	sh->taskview_box = (struct wlr_box){x, 0, 48, bh};
	if (sh->taskview_hot) {
		cairo_set_source_rgba(cr, hov, hov, hov, 0.1);
		cairo_rectangle(cr, x, 0, 48, bh);
		cairo_fill(cr);
	}
	draw_mdl2(cr, "\uE7C4", 11, x + 24, bh / 2.0, fg);
	x += 48;

	/* tray, right to left: Show desktop sliver, Action Center, clock, battery, network, volume, ^ */
	sh->showdesk_box = (struct wlr_box){sh->w - 5, 0, 5, bh};
	cairo_set_source_rgba(cr, hov, hov, hov, sh->showdesk_hot ? 0.25 : 0.12);
	cairo_rectangle(cr, sh->w - 5, 0, 1, bh);
	cairo_fill(cr);
	int tx = sh->w - 5 - 48;
	sh->action_box = (struct wlr_box){tx, 0, 48, bh};
	if (sh->action_hot) {
		cairo_set_source_rgba(cr, hov, hov, hov, 0.1);
		cairo_rectangle(cr, tx, 0, 48, bh);
		cairo_fill(cr);
	}
	draw_mdl2(cr, "\uE91C", 11, tx + 24, bh / 2.0, fg);
	int cw = text_width("Segoe UI 9", sh->clock), dw = text_width("Segoe UI 9", sh->date);
	int clock_w = (cw > dw ? cw : dw) + 16;
	tx -= clock_w;
	draw_text(cr, "Segoe UI 9", sh->clock, tx + (clock_w - cw) / 2, 4, 0, fg, 0, 0, 0, false);
	draw_text(cr, "Segoe UI 9", sh->date, tx + (clock_w - dw) / 2, 20, 0, fg, 0, 0, 0, false);
	if (sh->bat_pct >= 0) {
		tx -= 24;
		sh->bat_icon_box = (struct wlr_box){tx, 0, 24, bh};
		/* battery glyphs E850 (empty) .. E859 (full); E85A.. while charging */
		int lvl = sh->bat_pct >= 100 ? 9 : sh->bat_pct / 10;
		char g[8];
		unsigned cp = (sh->bat_charging ? 0xE85A : 0xE850) + lvl;
		snprintf(g, sizeof(g), "%c%c%c", 0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
		draw_mdl2(cr, g, 10, tx + 12, bh / 2.0, fg);
	} else {
		sh->bat_icon_box = (struct wlr_box){0, 0, 0, 0};
	}
	tx -= 24;
	sh->net_icon_box = (struct wlr_box){tx, 0, 24, bh};
	draw_mdl2(cr, "\uE701", 10, tx + 12, bh / 2.0, fg);
	if (sh->vol_ok) {
		tx -= 24;
		sh->vol_icon_box = (struct wlr_box){tx, 0, 24, bh};
		static const char *vg[4] = {"\uE992", "\uE993", "\uE994", "\uE995"};
		const char *g = sh->vol_muted ? "\uE74F" : vg[sh->vol_pct <= 0 ? 0 : sh->vol_pct < 34 ? 1 : sh->vol_pct < 67 ? 2 : 3];
		draw_mdl2(cr, g, 10, tx + 12, bh / 2.0, fg);
	}
	tx -= 24;
	draw_mdl2(cr, "\uE70E", 8, tx + 12, bh / 2.0, fg);

	/* task buttons: 48 wide, a 2px accent-light line under running apps */
	int avail = tx - x - 8;
	int bw = sh->n_tasks && avail / sh->n_tasks < 48 ? avail / sh->n_tasks : 48;
	uint32_t line = srv->accent_rgb;
	for (int i = 0; i < sh->n_tasks; i++) {
		struct task *k = &sh->tasks[i];
		k->box = (struct wlr_box){x + i * bw, 0, bw, bh};
		bool act = k->view->active && !k->view->minimized, hot = i == sh->task_hot;
		if (act || hot) {
			cairo_set_source_rgba(cr, hov, hov, hov, act && hot ? 0.2 : act ? 0.13 : 0.1);
			cairo_rectangle(cr, k->box.x, 0, bw, bh);
			cairo_fill(cr);
		}
		double inset = act ? 0 : 4;
		cairo_set_source_rgb(cr, ((line >> 16) & 255) / 255.0 * 0.5 + 0.5, ((line >> 8) & 255) / 255.0 * 0.5 + 0.5,
			(line & 255) / 255.0 * 0.5 + 0.5);
		cairo_rectangle(cr, k->box.x + inset, bh - 2, bw - 2 * inset, 2);
		cairo_fill(cr);
		struct icon_set *ic = &k->view->icon;
		cairo_surface_t *img = ic->s32 ? ic->s32 : ic->s16;
		if (img)
			draw_scaled(cr, img, k->box.x + (bw - 24) / 2.0, (bh - 24) / 2.0, 24);
	}
	cairo_destroy(cr);
	show(&sh->taskbar, srv->layer_shell, s, 0, sh->h - bh);
	if (acrylic) {
		glass_backdrop(srv, &sh->bar_glass, srv->layer_shell, 0, sh->h - bh, 0, sh->h - bh, sh->w, bh);
		if (sh->bar_glass)
			wlr_scene_node_place_below(&sh->bar_glass->node, &sh->taskbar->node);
	} else if (sh->bar_glass) {
		wlr_scene_node_set_enabled(&sh->bar_glass->node, false);
	}
}

static void draw_taskbar(struct shell *sh) {
	if (bar10(sh)) {
		draw_taskbar10(sh);
		return;
	}
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

	/* tray: the speaker, wifi, brightness/battery, then the clock */
	int cw = text_width("Tahoma 8", sh->clock);
	int icons_w = (sh->vol_ok ? 22 : 0) + 44;
	int trayw = cw + 19 + 14 + icons_w;
	if (trayw < 62)
		trayw = 62;
	int tx = sh->w - trayw;
	draw_image(cr, &t->tray, 0, tx, 0, trayw, TASKBAR_H);
	if (sh->vol_ok) {
		sh->vol_icon_box = (struct wlr_box){tx + 17, 5, 20, 20};
		draw_surface(cr, (sh->vol_muted ? sh->vol_icon_muted : sh->vol_icon).s16, tx + 19, 7);
	}
	int nx = tx + 19 + (sh->vol_ok ? 22 : 0);
	sh->net_icon_box = (struct wlr_box){nx - 2, 5, 20, 20};
	draw_wifi_icon(cr, sh, nx, 7);
	int bx = nx + 22;
	sh->bat_icon_box = (struct wlr_box){bx - 2, 5, 20, 20};
	if (sh->bat_pct >= 0)
		draw_battery(cr, sh, bx, 6);
	else
		draw_sun(cr, bx, 7, 0xffffff);
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
	int old_bat = sh->bat_pct;
	bool old_chg = sh->bat_charging;
	tick(sh);
	if (strcmp(old, sh->clock) || old_bat != sh->bat_pct || old_chg != sh->bat_charging)
		draw_taskbar(sh);
	wl_event_source_timer_update(sh->timer, 1000);
	return 0;
}

/* ---------------------------------------------------------------- volume & battery pop-ups */

static void load_tray_icons(struct shell *sh) {
	struct theme *vt = bar_theme(sh);
	sh->vol_icon = theme_icon(vt, "volume");
	if (!sh->vol_icon.s16) {
		vt = sh->server->have_xp ? &sh->server->theme : &sh->server->theme7;
		sh->vol_icon = theme_icon(vt, "volume");
	}
	sh->vol_icon_muted = theme_icon(vt, "volume-muted");
	sh->vol_icon_0 = theme_icon(vt, "volume-0");
	sh->vol_icon_1 = theme_icon(vt, "volume-1");
	sh->vol_icon_2 = theme_icon(vt, "volume-2");
	sh->sndvol_icon = theme_icon(vt, "sndvol");
	if (!sh->sndvol_icon.s32)
		sh->sndvol_icon = theme_icon(vt, "cp-sounds");
}

static int bat_pop_h(struct shell *sh) {
	return sh->bat_pct >= 0 ? 128 : 90;
}

static void draw_batpop(struct shell *sh) {
	if (!sh->bat_open) {
		if (sh->batpop)
			wlr_scene_node_set_enabled(&sh->batpop->node, false);
		return;
	}
	bool w7 = bar7(sh);
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	int ph = bat_pop_h(sh);
	cairo_surface_t *s = canvas_new(BAT_POP_W, ph);
	cairo_t *cr = cairo_create(s);
	if (w7) {
		fill(cr, 0, 0, BAT_POP_W, ph, 0x556677);
		fill(cr, 1, 1, BAT_POP_W - 2, ph - 2, 0xd8e6f6);
		fill(cr, 4, 4, BAT_POP_W - 8, ph - 30, 0xffffff);
		fill(cr, 4, ph - 26, BAT_POP_W - 8, 22, 0xf1f5fb);
		fill(cr, 4, ph - 26, BAT_POP_W - 8, 1, 0xd6e5f5);
	} else {
		fill(cr, 0, 0, BAT_POP_W, ph, 0x716f64);
		fill(cr, 1, 1, BAT_POP_W - 2, ph - 2, 0xffffff);
		fill(cr, 2, 2, BAT_POP_W - 3, ph - 3, 0xaca899);
		fill(cr, 2, 2, BAT_POP_W - 4, ph - 4, 0xece9d8);
	}
	int sy = 12;
	if (sh->bat_pct >= 0) {
		draw_battery(cr, sh, 14, 14);
		char line1[64];
		snprintf(line1, sizeof(line1), "%d%% available", sh->bat_pct);
		draw_text(cr, "Tahoma Bold 8", line1, 38, 12, BAT_POP_W - 46, 0x000000, 0, 0, 0, true);
		const char *line2 = sh->bat_charging ? "(plugged in, charging)" : "On battery power";
		draw_text(cr, "Tahoma 8", line2, 38, 28, BAT_POP_W - 46, 0x5a5a5a, 0, 0, 0, true);
		fill(cr, 12, 48, BAT_POP_W - 24, 1, w7 ? 0xd6e5f5 : 0xaca899);
		sy = 56;
	}
	char bstr[48];
	snprintf(bstr, sizeof(bstr), "Screen brightness: %d%%", sh->bright_pct);
	draw_text(cr, "Tahoma 8", bstr, 14, sy, BAT_POP_W - 24, 0x000000, 0, 0, 0, true);
	int ty = sy + 20;
	draw_sun(cr, 14, ty - 2, w7 ? 0x1e395b : 0x000000);
	draw_image(cr, &t->slider_track, 0, BAT_SLIDER_X, ty + 4, BAT_SLIDER_W, 4);
	int tx = BAT_SLIDER_X + (sh->bright_pct - 10) * (BAT_SLIDER_W - 11) / 90;
	if (tx < BAT_SLIDER_X) tx = BAT_SLIDER_X;
	draw_image(cr, &t->slider_thumb_h, sh->bright_drag ? 2 : 0, tx, ty - 4, 11, 21);

	draw_text(cr, "Tahoma 8", "More power options", 14, ph - 21, BAT_POP_W - 24, 0x0066cc, 0, 0, 0, true);
	cairo_destroy(cr);
	show(&sh->batpop, sh->server->layer_menu, s, sh->bat_box.x, sh->bat_box.y);
	wlr_scene_node_set_enabled(&sh->batpop->node, true);
}

static void bat_set_open(struct shell *sh, bool open) {
	sh->bat_open = open;
	sh->bright_drag = false;
	if (sh->bat_open) {
		read_battery(sh);
		read_brightness(sh);
		int ph = bat_pop_h(sh);
		int x = sh->bat_icon_box.x + sh->bat_icon_box.width / 2 - BAT_POP_W / 2;
		if (x + BAT_POP_W > sh->w - 4)
			x = sh->w - BAT_POP_W - 4;
		sh->bat_box = (struct wlr_box){x, sh->h - bar_h(sh) - ph - 2, BAT_POP_W, ph};
	}
	draw_batpop(sh);
}

static void bright_drag_to(struct shell *sh, double x) {
	int pct = 10 + (int)((x - sh->bat_box.x - BAT_SLIDER_X) * 90 / (BAT_SLIDER_W - 11) + 0.5);
	if (pct < 10) pct = 10;
	if (pct > 100) pct = 100;
	if (pct != sh->bright_pct)
		apply_brightness(sh, pct);
	draw_batpop(sh);
}

void shell_brightness_key(struct shell *sh, int step) {
	if (!sh)
		return;
	read_brightness(sh);
	apply_brightness(sh, sh->bright_pct + step);
	if (sh->bat_open)
		draw_batpop(sh);
}

static void scan_net_aps(struct shell *sh) {
	sh->n_net_aps = 0;
	FILE *p = popen("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan no 2>/dev/null", "r");
	if (!p)
		return;
	char line[256];
	while (sh->n_net_aps < 8 && fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\r\n")] = 0;
		char *f[4] = {line, NULL, NULL, NULL};
		int nf = 1;
		for (char *c = line; *c && nf < 4; c++) {
			if (*c == '\\' && c[1]) {
				memmove(c, c + 1, strlen(c));
				continue;
			}
			if (*c == ':') {
				*c = 0;
				f[nf++] = c + 1;
			}
		}
		if (nf < 4 || !f[1][0])
			continue;
		bool dup = false;
		for (int i = 0; i < sh->n_net_aps; i++)
			dup |= !strcmp(sh->net_aps[i].ssid, f[1]);
		if (dup)
			continue;
		int idx = sh->n_net_aps++;
		snprintf(sh->net_aps[idx].ssid, sizeof(sh->net_aps[idx].ssid), "%s", f[1]);
		sh->net_aps[idx].sig = atoi(f[2]);
		sh->net_aps[idx].active = f[0][0] == '*';
		sh->net_aps[idx].sec = f[3][0] && strcmp(f[3], "--");
	}
	pclose(p);
}

static int net_pop_h(struct shell *sh) {
	int rows = sh->n_net_aps ? sh->n_net_aps : 1;
	return 34 + rows * NET_ROW_H + 30;
}

static void draw_netpop(struct shell *sh) {
	if (!sh->net_open) {
		if (sh->netpop)
			wlr_scene_node_set_enabled(&sh->netpop->node, false);
		return;
	}
	bool w7 = bar7(sh);
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	int ph = net_pop_h(sh);
	cairo_surface_t *s = canvas_new(NET_POP_W, ph);
	cairo_t *cr = cairo_create(s);
	if (w7) {
		fill(cr, 0, 0, NET_POP_W, ph, 0x556677);
		fill(cr, 1, 1, NET_POP_W - 2, ph - 2, 0xd8e6f6);
		fill(cr, 4, 4, NET_POP_W - 8, ph - 30, 0xffffff);
		fill(cr, 4, ph - 26, NET_POP_W - 8, 22, 0xf1f5fb);
		fill(cr, 4, ph - 26, NET_POP_W - 8, 1, 0xd6e5f5);
	} else {
		fill(cr, 0, 0, NET_POP_W, ph, 0x716f64);
		fill(cr, 1, 1, NET_POP_W - 2, ph - 2, 0xffffff);
		fill(cr, 2, 2, NET_POP_W - 3, ph - 3, 0xaca899);
		fill(cr, 2, 2, NET_POP_W - 4, ph - 4, 0xece9d8);
	}
	draw_text(cr, "Tahoma Bold 8", sh->net_up ? "Wireless Network Connection" : "Not connected",
		12, 10, NET_POP_W - 24, w7 ? 0x1e395b : 0x000000, 0, 0, 0, true);
	if (!sh->n_net_aps) {
		draw_text(cr, "Tahoma 8", sh->net_up ? "Connected" : "No wireless networks found",
			14, 38, NET_POP_W - 28, 0x5a5a5a, 0, 0, 0, true);
	}
	for (int i = 0; i < sh->n_net_aps; i++) {
		int ry = 32 + i * NET_ROW_H;
		if (i == sh->net_hot || sh->net_aps[i].active) {
			if (w7)
				hl7(cr, (struct wlr_box){8, ry, NET_POP_W - 16, NET_ROW_H - 2});
			else
				fill(cr, 8, ry, NET_POP_W - 16, NET_ROW_H - 2, i == sh->net_hot ? 0x316ac5 : 0xd4d0c8);
		}
		uint32_t fg = (!w7 && i == sh->net_hot) ? 0xffffff : 0x000000;
		draw_text(cr, sh->net_aps[i].active ? "Tahoma Bold 8" : "Tahoma 8",
			sh->net_aps[i].ssid, 14, ry + 5, NET_POP_W - 92, fg, 0, 0, 0, true);
		char rstr[32];
		if (sh->net_aps[i].active)
			snprintf(rstr, sizeof(rstr), "Connected");
		else
			snprintf(rstr, sizeof(rstr), "%d%%%s", sh->net_aps[i].sig, sh->net_aps[i].sec ? " *" : "");
		int rw = text_width("Tahoma 8", rstr);
		draw_text(cr, "Tahoma 8", rstr, NET_POP_W - 14 - rw, ry + 5, 0,
			(!w7 && i == sh->net_hot) ? 0xffffff : 0x5a5a5a, 0, 0, 0, true);
	}
	draw_text(cr, "Tahoma 8", "Open Network and Sharing Center", 14, ph - 21, NET_POP_W - 24, 0x0066cc, 0, 0, 0, true);
	cairo_destroy(cr);
	show(&sh->netpop, sh->server->layer_menu, s, sh->net_box.x, sh->net_box.y);
	wlr_scene_node_set_enabled(&sh->netpop->node, true);
}

static void net_set_open(struct shell *sh, bool open) {
	sh->net_open = open;
	sh->net_hot = -1;
	if (sh->net_open) {
		read_net_fast(sh);
		scan_net_aps(sh);
		int ph = net_pop_h(sh);
		int x = sh->net_icon_box.x + sh->net_icon_box.width / 2 - NET_POP_W / 2;
		if (x + NET_POP_W > sh->w - 4)
			x = sh->w - NET_POP_W - 4;
		sh->net_box = (struct wlr_box){x, sh->h - bar_h(sh) - ph - 2, NET_POP_W, ph};
	}
	draw_netpop(sh);
}

static void draw_volpop(struct shell *sh) {
	if (!sh->vol_open) {
		if (sh->volpop)
			wlr_scene_node_set_enabled(&sh->volpop->node, false);
		return;
	}
	bool w7 = bar7(sh);
	struct theme *t = bar_theme(sh);
	theme_use_fonts(t);
	int vw = w7 ? W7_VOL_W : VOL_W, vh = w7 ? W7_VOL_H : VOL_H;
	int ttop = w7 ? W7_VOL_TOP : VOL_TRACK_TOP, th = w7 ? W7_VOL_TH : VOL_TRACK_H;
	int pct = sh->vol_pct > 100 ? 100 : sh->vol_pct;
	char pct_str[16];
	snprintf(pct_str, sizeof(pct_str), "%d%%", pct);

	cairo_surface_t *s = canvas_new(vw, vh);
	cairo_t *cr = cairo_create(s);
	int cx = vw / 2;
	if (w7) {
		/* Windows 7 Aero volume flyout: outer frame, white body, footer strip */
		fill(cr, 0, 0, vw, vh, 0x556677);
		fill(cr, 1, 1, vw - 2, vh - 2, 0xd8e6f6);
		fill(cr, 4, 4, vw - 8, vh - 32, 0xffffff);
		fill(cr, 4, vh - 28, vw - 8, 24, 0xf1f5fb);
		fill(cr, 4, vh - 28, vw - 8, 1, 0xd6e5f5);
		if (sh->sndvol_icon.s32)
			draw_scaled(cr, sh->sndvol_icon.s32, cx - 16, 10, 32);
		int pw = text_width("Tahoma 8", pct_str);
		draw_text(cr, "Tahoma 8", pct_str, (vw - pw) / 2, 44, 0, 0x1e395b, 0, 0, 0, true);
		draw_image(cr, &t->slider_track, 0, cx - 2, ttop, 4, th);
		for (int i = 0; i <= 5; i++) {
			int y = ttop + i * th / 5;
			fill(cr, cx + 10, y, 4, 1, 0xa0a0a0);
		}
		int ty = ttop + (100 - pct) * th / 100 - 5;
		draw_image(cr, &t->slider_thumb, sh->vol_drag ? 2 : 0, cx - 10, ty, 21, 11);

		/* Mute button with speaker icon */
		int my = ttop + th + 10;
		if (sh->vol_muted) {
			fill(cr, cx - 13, my, 26, 24, 0x7da2ce);
			fill(cr, cx - 12, my + 1, 24, 22, 0xd9ebf9);
		}
		cairo_surface_t *mic = (sh->vol_muted ? sh->vol_icon_muted : sh->vol_icon).s16;
		draw_surface(cr, mic, cx - 8, my + 4);
		int mw = text_width("Tahoma 8", "Mixer");
		draw_text(cr, "Tahoma 8", "Mixer", (vw - mw) / 2, vh - 23, 0, 0x0066cc, 0, 0, 0, true);
	} else {
		/* dialog face with a thin raised frame, like XP's tray volume pop-up */
		fill(cr, 0, 0, vw, vh, 0x716f64);
		fill(cr, 1, 1, vw - 2, vh - 2, 0xffffff);
		fill(cr, 2, 2, vw - 3, vh - 3, 0xaca899);
		fill(cr, 2, 2, vw - 4, vh - 4, 0xece9d8);
		char hdr[24];
		snprintf(hdr, sizeof(hdr), "Vol %d%%", pct);
		int tw = text_width("Tahoma 8", hdr);
		draw_text(cr, "Tahoma 8", hdr, (vw - tw) / 2, 9, 0, 0x000000, 0, 0, 0, true);

		draw_image(cr, &t->slider_track, 0, cx - 2, ttop, 5, th);
		for (int i = 0; i <= 5; i++) {
			int y = ttop + i * th / 5;
			fill(cr, cx - 16, y, 4, 1, 0x000000);
			fill(cr, cx + 13, y, 4, 1, 0x000000);
		}
		int ty = ttop + (100 - pct) * th / 100 - 5;
		/* thumb frames: normal hot pressed focused disabled */
		draw_image(cr, &t->slider_thumb, sh->vol_drag ? 2 : 0, cx - 10, ty, 21, 11);

		/* Mute checkbox: unchecked frames 0-3, checked 4-7 */
		draw_frame_true(cr, t->checkbox, t->version >= 7 ? 20 : 12, sh->vol_muted ? 4 : 0, 12, vh - 28);
		draw_text(cr, "Tahoma 8", "Mute", 31, vh - 28, 0, 0x000000, 0, 0, 0, true);
	}
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
		bat_set_open(sh, false);
		net_set_open(sh, false);
		vol_refresh(sh);
		int vw = bar7(sh) ? W7_VOL_W : VOL_W, vh = bar7(sh) ? W7_VOL_H : VOL_H;
		int x = sh->vol_icon_box.x + sh->vol_icon_box.width / 2 - vw / 2;
		if (x + vw > sh->w - 4)
			x = sh->w - vw - 4;
		sh->vol_box = (struct wlr_box){x, sh->h - bar_h(sh) - vh - 2, vw, vh};
	}
	draw_volpop(sh);
	draw_taskbar(sh);
}

static void vol_drag_to(struct shell *sh, double y) {
	int ttop = bar7(sh) ? W7_VOL_TOP : VOL_TRACK_TOP, th = bar7(sh) ? W7_VOL_TH : VOL_TRACK_H;
	int pct = 100 - (int)((y - sh->vol_box.y - ttop) * 100 / th + 0.5);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	if (pct != sh->vol_pct) {
		sh->vol_pct = pct;
		volume_write(pct);
	}
	draw_volpop(sh);
	if (bar7(sh))
		draw_taskbar(sh);
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
	bool w7 = bar7(sh);
	int vw = w7 ? W7_VOL_W : VOL_W, vh = w7 ? W7_VOL_H : VOL_H;
	int ttop = w7 ? W7_VOL_TOP : VOL_TRACK_TOP, th = w7 ? W7_VOL_TH : VOL_TRACK_H;
	double lx = x - sh->vol_box.x, ly = y - sh->vol_box.y;
	if (w7 && ly >= vh - 28) {
		vol_set_open(sh, false);
		spawn("expede-settings sounds");
	} else if (ly >= (w7 ? ttop + th + 6 : vh - 32) && ly < (w7 ? vh - 28 : vh - 10)) {
		sh->vol_muted = !sh->vol_muted;
		volume_mute(sh->vol_muted);
		draw_volpop(sh);
		draw_taskbar(sh);
	} else if (lx >= vw / 2 - 18 && lx <= vw / 2 + 18 && ly >= ttop - 8 && ly <= ttop + th + 8) {
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

static void add_item(struct shell *sh, enum item_kind kind, const char *label, const char *label7, const char *icon, const char *cmd) {
	struct menu_item *m = &sh->items[sh->n_items++];
	m->kind = kind;
	m->label = label;
	m->label7 = label7;
	m->icon = icon;
	m->cmd = cmd;
	if (icon)
		m->img = theme_icon(menu_theme(sh), icon);
}

static int bottom_band(struct shell *sh) {
	return MORE_H + (sh->modern ? SEARCH_H : 0);
}

/* menu-relative area the modern app list fills */
static struct wlr_box list_box(struct shell *sh) {
	if (menu7(sh))
		return (struct wlr_box){11, 12, W7_LEFT_W - 22, W7_MENU_H - 96};
	int top = USER_H + 6;
	return (struct wlr_box){6, top, COL_W - 15, sh->menu_h - LOGOFF_H - bottom_band(sh) - top - 2};
}

static struct wlr_box search_box(struct shell *sh) {
	if (menu7(sh))
		return (struct wlr_box){14, W7_MENU_H - 42, W7_LEFT_W - 28, 26};
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
	if (menu7(sh)) {
		int ly = 12, ry = 54, rw = W7_MENU_W - W7_LEFT_W - 14;
		for (int i = 0; i < sh->n_items; i++) {
			struct menu_item *m = &sh->items[i];
			if (!m->label7) {
				m->box = (struct wlr_box){0};
				continue;
			}
			switch (m->kind) {
			case ITEM_PINNED: case ITEM_MFU:
				m->box = (struct wlr_box){11, ly, W7_LEFT_W - 22, 36};
				ly += 36;
				break;
			case ITEM_SEP_LEFT:
				m->box = (struct wlr_box){14, ly, W7_LEFT_W - 28, 9};
				ly += 9;
				break;
			case ITEM_PLACE_BOLD: case ITEM_PLACE:
				m->box = (struct wlr_box){W7_LEFT_W + 6, ry, rw, 28};
				ry += 28;
				break;
			case ITEM_SEP_RIGHT:
				m->box = (struct wlr_box){W7_LEFT_W + 10, ry, rw - 8, 9};
				ry += 9;
				break;
			case ITEM_ALL_PROGRAMS:
				m->box = (struct wlr_box){11, W7_MENU_H - 78, W7_LEFT_W - 22, 24};
				break;
			case ITEM_TURNOFF:
				m->box = (struct wlr_box){W7_LEFT_W + 10, W7_MENU_H - 38, 86, 24};
				break;
			case ITEM_LOGOFF:
				m->box = (struct wlr_box){W7_LEFT_W + 96, W7_MENU_H - 38, 23, 24};
				break;
			}
		}
		return;
	}
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
	if (menu7(sh) && !m->label7)
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
	struct theme *t = menu_theme(sh);
	bool w7 = menu7(sh);
	struct wlr_box lb = list_box(sh);
	int n;
	struct app *apps = apps_all(&n);
	cairo_save(cr);
	cairo_rectangle(cr, lb.x, lb.y, lb.width, lb.height);
	cairo_clip(cr);
	if (!sh->n_results)
		draw_text(cr, "Tahoma 8", "No items match your search.", lb.x + 8, lb.y + 8, lb.width - 12,
			0x808080, 0, 0, 0, !w7);
	int rows = lb.height / ROW_H;
	for (int r = 0; r < rows + 1 && sh->scroll + r < sh->n_results; r++) {
		int idx = sh->scroll + r;
		struct app *a = &apps[sh->results[idx]];
		int y = lb.y + r * ROW_H;
		bool hl = idx == sh->list_hot || (sh->list_hot < 0 && idx == sh->list_sel);
		if (hl) {
			if (w7)
				hl7(cr, (struct wlr_box){lb.x, y, lb.width - (sh->n_results > rows ? 6 : 0), ROW_H});
			else
				fill(cr, lb.x, y, lb.width, ROW_H, 0x3070d0);
		}
		draw_scaled(cr, app_icon(t, a), lb.x + 4, y + 2, 20);
		draw_text(cr, "Tahoma 8", a->name, lb.x + 30, y + 4, lb.width - 34,
			hl && !w7 ? 0xffffff : 0x282828, 0, 0, 0, !w7);
	}
	cairo_restore(cr);
	/* scroll hint: a thin bar on the right edge */
	if (sh->n_results > rows && rows > 0) {
		double frac = (double)rows / sh->n_results, pos = (double)sh->scroll / sh->n_results;
		fill(cr, lb.x + lb.width - 4, lb.y + pos * lb.height, 3, frac * lb.height, w7 ? 0xa5b9d1 : 0x7ba2e7);
	}
}

static void draw_search(struct shell *sh, cairo_t *cr) {
	bool w7 = menu7(sh);
	struct wlr_box b = search_box(sh);
	fill(cr, b.x, b.y, b.width, b.height, w7 ? 0x5794bf : 0x7f9db9);
	fill(cr, b.x + 1, b.y + 1, b.width - 2, b.height - 2, 0xffffff);
	int ty = b.y + (b.height - 15) / 2;
	if (sh->query[0]) {
		int w = draw_text(cr, "Tahoma 8", sh->query, b.x + 6, ty, b.width - (w7 ? 28 : 12), 0x000000, 0, 0, 0, !w7);
		int maxw = b.width - (w7 ? 28 : 12);
		fill(cr, b.x + 7 + (w < maxw ? w : maxw), ty + 1, 1, 13, 0x000000);
	} else {
		const char *ph = w7 ? "Search programs and files" : "Start Search";
		draw_text(cr, "Tahoma, Italic 8", ph, b.x + 6, ty, 0, 0x6d6d6d, 0, 0, 0, !w7);
		fill(cr, b.x + 5, ty + 1, 1, 13, 0x000000);
	}
	if (w7) {
		struct icon_set ic = theme_icon(menu_theme(sh), "search");
		if (ic.s16) {
			draw_surface(cr, ic.s16, b.x + b.width - 20, b.y + (b.height - 16) / 2);
			cairo_surface_destroy(ic.s16);
		}
		if (ic.s32)
			cairo_surface_destroy(ic.s32);
	}
}

/* Windows 7's Aero Start menu: white program list and search box on the left,
 * glass places column on the right with the framed user picture rising above */
static void draw_menu7(struct shell *sh) {
	struct server *srv = sh->server;
	struct theme *t = menu_theme(sh);
	theme_use_fonts(t);
	int bh = bar_h(sh), mh = W7_MENU_H;
	cairo_surface_t *s = canvas_new(W7_MENU_W, mh + W7_PIC_OVER);
	cairo_t *cr = cairo_create(s);
	cairo_translate(cr, 0, W7_PIC_OVER);

	/* outer Aero glass frame + right column tint */
	draw_glass(srv, cr, 0, 0, W7_MENU_W, mh, 6, 0, true);
	cairo_save(cr);
	cairo_rectangle(cr, W7_LEFT_W, 2, W7_MENU_W - W7_LEFT_W - 2, mh - 4);
	cairo_clip(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.25);
	cairo_paint(cr);
	cairo_restore(cr);

	/* white left pane: program list, All Programs row, and search tray */
	int lx = 7, ly = 7, lw = W7_LEFT_W - 7, lh = mh - 14;
	fill(cr, lx, ly, lw, lh, 0x7f9db9);
	fill(cr, lx + 1, ly + 1, lw - 2, lh - 2, 0xffffff);
	draw_image(cr, &t->w7_sp_progs, 0, lx + 1, ly + 1, lw - 2, lh - 46);
	fill(cr, lx + 6, mh - 81, lw - 12, 1, 0xd6e0ec);
	fill(cr, lx + 1, mh - 50, lw - 2, 1, 0xc3d2e4);
	fill(cr, lx + 1, mh - 49, lw - 2, 41, 0xf1f5fb);

	if (list_mode(sh))
		draw_app_list(sh, cr);
	draw_search(sh, cr);

	/* user picture rising above the right column in its glass frame */
	int ux = W7_LEFT_W + (W7_MENU_W - W7_LEFT_W - 64) / 2, uy = -W7_PIC_OVER;
	if (t->user_pic)
		draw_scaled(cr, t->user_pic, ux + 8, uy + 8, 48);
	draw_surface(cr, t->w7_userframe, ux, uy);

	for (int i = 0; i < sh->n_items; i++) {
		struct menu_item *m = &sh->items[i];
		if (!m->label7)
			continue;
		struct wlr_box b = m->box;
		bool hot = i == sh->menu_hot;
		if (list_mode(sh) && (m->kind == ITEM_PINNED || m->kind == ITEM_MFU || m->kind == ITEM_SEP_LEFT))
			continue;
		switch (m->kind) {
		case ITEM_PINNED: case ITEM_MFU:
			if (hot)
				hl7(cr, b);
			draw_surface(cr, m->img.s32, b.x + 4, b.y + 2);
			draw_text(cr, "Segoe UI 9", m->label7, b.x + 42, b.y + 10, b.width - 44, 0x282828, 0, 0, 0, false);
			break;
		case ITEM_SEP_LEFT:
			fill(cr, b.x, b.y + 4, b.width, 1, 0xdbe3ed);
			break;
		case ITEM_SEP_RIGHT:
			cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
			cairo_rectangle(cr, b.x, b.y + 3, b.width, 1);
			cairo_fill(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.2);
			cairo_rectangle(cr, b.x, b.y + 4, b.width, 1);
			cairo_fill(cr);
			break;
		case ITEM_PLACE_BOLD: case ITEM_PLACE:
			if (hot)
				glass_hl(cr, b);
			draw_text(cr, "Segoe UI 9", m->label7, b.x + 10, b.y + 6, b.width - 14, 0xffffff, 1, 1, 0x000000, false);
			break;
		case ITEM_ALL_PROGRAMS: {
			if (hot)
				hl7(cr, b);
			bool back = list_mode(sh);
			const char *label = back ? "Back" : m->label7;
			/* small triangle pointing right for All Programs, left for Back */
			double ax = b.x + 8, ay = b.y + b.height / 2.0;
			cairo_save(cr);
			cairo_set_source_rgb(cr, 0.16, 0.24, 0.36);
			if (back) {
				cairo_move_to(cr, ax + 5, ay - 4);
				cairo_line_to(cr, ax, ay);
				cairo_line_to(cr, ax + 5, ay + 4);
			} else {
				cairo_move_to(cr, ax, ay - 4);
				cairo_line_to(cr, ax + 5, ay);
				cairo_line_to(cr, ax, ay + 4);
			}
			cairo_close_path(cr);
			cairo_fill(cr);
			cairo_restore(cr);
			draw_text(cr, "Segoe UI 9", label, b.x + 24, b.y + 4, b.width - 28, 0x282828, 0, 0, 0, false);
			break;
		}
		case ITEM_TURNOFF: {
			draw_image(cr, &t->w7_sp_shutdown, hot ? 1 : 0, b.x, b.y, b.width, b.height);
			int tw = text_width("Segoe UI 9", m->label7);
			draw_text(cr, "Segoe UI 9", m->label7, b.x + (b.width - tw) / 2, b.y + 4, 0, 0xffffff, 1, 1, 0x000000, false);
			break;
		}
		case ITEM_LOGOFF: {
			draw_image(cr, &t->w7_sp_shutdown_arrow, hot ? 1 : 0, b.x, b.y, b.width, b.height);
			double ax = b.x + 9, ay = b.y + b.height / 2.0;
			cairo_save(cr);
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_move_to(cr, ax, ay - 3.5);
			cairo_line_to(cr, ax + 4, ay);
			cairo_line_to(cr, ax, ay + 3.5);
			cairo_close_path(cr);
			cairo_fill(cr);
			cairo_restore(cr);
			break;
		}
		}
	}
	cairo_destroy(cr);
	show(&sh->menu, srv->layer_menu, s, 0, sh->h - bh - mh - W7_PIC_OVER);
	wlr_scene_node_set_enabled(&sh->menu->node, sh->start_open);
	if (sh->start_open) {
		glass_backdrop(srv, &sh->menu_glass, srv->layer_menu, 0, sh->h - bh - mh, 0, sh->h - bh - mh, W7_MENU_W, mh);
		if (sh->menu_glass)
			wlr_scene_node_place_below(&sh->menu_glass->node, &sh->menu->node);
	} else if (sh->menu_glass) {
		wlr_scene_node_set_enabled(&sh->menu_glass->node, false);
	}
}

/* ---------------------------------------------------------------- Windows 10 Start */

enum { S10_MENU = 1, S10_USER, S10_DOCS, S10_PICS, S10_SETTINGS, S10_POWER, S10_SIGNOUT, S10_LOCK,
	S10_SLEEP = 3000, S10_SHUTDOWN, S10_RESTART, S10_APP = 10000, S10_TILE = 20000, S10_RESULT = 30000 };

static const struct { const char *label, *icon, *cmd; } tiles10[6] = {
	{"File Explorer", "explorer", "expede-explorer"}, {"Settings", "control-panel", "expede-settings"},
	{"This PC", "computer", "expede-explorer computer:"}, {"Command Prompt", "cmd", "foot"},
	{"Notepad", "notepad", "mousepad"}, {"Snipping Tool", "file-image", "expede-snip"},
};

static void hit10(struct shell *sh, int x, int y, int w, int h, int id) {
	if (sh->n_hits10 < (int)(sizeof(sh->hits10) / sizeof(*sh->hits10)))
		sh->hits10[sh->n_hits10++] = (struct hit10){{x, y, w, h}, id};
}

static void fill_a(cairo_t *cr, double x, double y, double w, double h, double v, double a) {
	cairo_set_source_rgba(cr, v, v, v, a);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void draw_menu10(struct shell *sh) {
	struct server *srv = sh->server;
	struct theme *t = menu_theme(sh);
	if (sh->menu_glass && !sh->start_open)
		wlr_scene_node_set_enabled(&sh->menu_glass->node, false);
	if (!sh->start_open) {
		if (sh->menu)
			wlr_scene_node_set_enabled(&sh->menu->node, false);
		return;
	}
	theme_use_fonts(t);
	if (sh->tile_theme != desk_theme(sh)) {
		sh->tile_theme = desk_theme(sh);
		for (int i = 0; i < 6; i++)
			sh->tile_img[i] = theme_icon(sh->tile_theme, tiles10[i].icon);
	}
	/* as tall as Windows 10's (640), less on small screens */
	sh->menu_h = sh->h - bar_h(sh) > 680 ? 640 : sh->h - bar_h(sh) - 40;
	int mw = W10_MENU_W, mh = sh->menu_h;
	sh->n_hits10 = 0;
	cairo_surface_t *c = canvas_new(mw, mh);
	cairo_t *cr = cairo_create(c);
	bool acrylic = srv->opt_transparency && srv->glass_buf;
	cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, acrylic ? 0.88 : 1);
	cairo_paint(cr);
	uint32_t acc = srv->accent_rgb;
	int n;
	struct app *apps = apps_all(&n);

	if (sh->query[0]) {
		/* typing in Start searches: "Best match", then the other apps */
		int x = 48, y = 12;
		draw_text(cr, "Segoe UI Semibold 9", "Best match", x + 16, y, 0, 0xffffff, 0, 0, 0, false);
		y += 28;
		for (int r = 0; r < sh->n_results && y < mh - 40; r++) {
			struct app *a = &apps[sh->results[r]];
			int rh = r == 0 ? 72 : 36, id = S10_RESULT + r;
			if (r == 1) {
				draw_text(cr, "Segoe UI Semibold 9", "Apps", x + 16, y + 8, 0, 0xffffff, 0, 0, 0, false);
				y += 32;
			}
			bool hl = sh->hot10 == id || (sh->hot10 == 0 && r == sh->list_sel);
			if (hl)
				fill_a(cr, x, y, mw - x - 8, rh, 1, r == 0 ? 0.2 : 0.12);
			int isz = r == 0 ? 48 : 24;
			draw_scaled(cr, app_icon(t, a), x + 16, y + (rh - isz) / 2.0, isz);
			draw_text(cr, r == 0 ? "Segoe UI 12" : "Segoe UI 9", a->name, x + 16 + isz + 12, y + (r == 0 ? 14 : 9),
				mw - x - isz - 60, 0xffffff, 0, 0, 0, false);
			if (r == 0)
				draw_text(cr, "Segoe UI 9", "App", x + 16 + isz + 12, y + 40, 0, 0xbbbbbb, 0, 0, 0, false);
			hit10(sh, x, y, mw - x - 8, rh, id);
			y += rh;
		}
		if (!sh->n_results)
			draw_text(cr, "Segoe UI 9", "No results", x + 16, y, 0, 0xbbbbbb, 0, 0, 0, false);
		/* no search box on the taskbar (a narrow screen): Start shows one at its foot */
		if (!bar10(sh) || sh->search_box10.width < 100) {
			int by = mh - 48;
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_rectangle(cr, x + 8, by, mw - x - 16, 40);
			cairo_fill(cr);
			draw_mdl2(cr, "\uE721", 10, x + 28, by + 20, 0x000000);
			int qw = draw_text(cr, "Segoe UI 10", sh->query, x + 48, by + 10, mw - x - 70, 0x000000, 0, 0, 0, false);
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_rectangle(cr, x + 49 + qw, by + 10, 1, 20);
			cairo_fill(cr);
		}
	} else {
		/* the A-Z list, with a letter above each group */
		int lx = 48, lw = 270, y = 8 - sh->scroll10;
		cairo_save(cr);
		cairo_rectangle(cr, lx, 0, lw, mh);
		cairo_clip(cr);
		char last = 0;
		for (int i = 0; i < n; i++) {
			char L = apps[i].name[0];
			if (L >= 'a' && L <= 'z')
				L -= 32;
			if (!(L >= 'A' && L <= 'Z'))
				L = '#';
			if (L != last) {
				char hdr[2] = {L, 0};
				if (y > -40 && y < mh)
					draw_text(cr, "Segoe UI 9", hdr, lx + 16, y + 9, 0, 0xffffff, 0, 0, 0, false);
				y += 36;
				last = L;
			}
			if (y > -40 && y < mh) {
				int id = S10_APP + i;
				if (sh->hot10 == id)
					fill_a(cr, lx, y, lw, 36, 1, 0.12);
				/* desktop apps sit on an accent square, like Windows 10 */
				cairo_set_source_rgb(cr, ((acc >> 16) & 255) / 255.0, ((acc >> 8) & 255) / 255.0, (acc & 255) / 255.0);
				cairo_rectangle(cr, lx + 8, y + 2, 32, 32);
				cairo_fill(cr);
				draw_scaled(cr, app_icon(t, &apps[i]), lx + 12, y + 6, 24);
				draw_text(cr, "Segoe UI 9", apps[i].name, lx + 52, y + 9, lw - 60, 0xffffff, 0, 0, 0, false);
				if (y >= 0 && y + 36 <= mh)
					hit10(sh, lx, y, lw, 36, id);
			}
			y += 36;
		}
		cairo_restore(cr);
		/* tiles */
		int tx = 48 + 270 + 24, ty = 44, ts = 100, gap = 4;
		draw_text(cr, "Segoe UI 9", "Expede", tx, 14, 0, 0xffffff, 0, 0, 0, false);
		for (int i = 0; i < 6; i++) {
			int x = tx + (i % 3) * (ts + gap), y2 = ty + (i / 3) * (ts + gap), id = S10_TILE + i;
			cairo_set_source_rgba(cr, ((acc >> 16) & 255) / 255.0, ((acc >> 8) & 255) / 255.0, (acc & 255) / 255.0,
				acrylic ? 0.85 : 1);
			cairo_rectangle(cr, x, y2, ts, ts);
			cairo_fill(cr);
			if (sh->hot10 == id) {
				cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
				cairo_set_line_width(cr, 2);
				cairo_rectangle(cr, x + 1, y2 + 1, ts - 2, ts - 2);
				cairo_stroke(cr);
			}
			cairo_surface_t *img = sh->tile_img[i].s32;
			if (!img) {
				/* no Windows icon for it: the app's own (foot, Mousepad...) */
				const char *exe = tiles10[i].cmd;
				for (int k = 0; k < n && !img; k++)
					if (apps[k].exec && !strncmp(apps[k].exec, exe, strlen(exe)))
						img = app_icon(t, &apps[k]);
			}
			if (img)
				draw_scaled(cr, img, x + (ts - 32) / 2.0, y2 + 26, 32);
			draw_text(cr, "Segoe UI 8", tiles10[i].label, x + 8, y2 + ts - 22, ts - 12, 0xffffff, 0, 0, 0, false);
			hit10(sh, x, y2, ts, ts, id);
		}
	}

	/* the rail: menu at the top; account, Documents, Pictures, Settings and Power at the bottom */
	int rw = sh->rail_open ? 256 : 48;
	if (sh->rail_open) {
		cairo_set_source_rgba(cr, 0.17, 0.17, 0.17, 0.98);
		cairo_rectangle(cr, 0, 0, rw, mh);
		cairo_fill(cr);
	}
	struct { int id; const char *glyph, *label; } rail[6] = {
		{S10_MENU, "\uE700", "START"}, {S10_USER, "\uE77B", getenv("USER") ? getenv("USER") : "User"},
		{S10_DOCS, "\uE8A5", "Documents"}, {S10_PICS, "\uE91B", "Pictures"}, {S10_SETTINGS, "\uE713", "Settings"},
		{S10_POWER, "\uE7E8", "Power"},
	};
	for (int i = 0; i < 6; i++) {
		int y = i == 0 ? 0 : mh - (6 - i) * 48;
		if (sh->hot10 == rail[i].id)
			fill_a(cr, 0, y, rw, 48, 1, 0.12);
		draw_mdl2(cr, rail[i].glyph, 11, 24, y + 24, 0xffffff);
		if (sh->rail_open)
			draw_text(cr, i == 0 ? "Segoe UI Semibold 9" : "Segoe UI 9", rail[i].label, 56, y + 15, rw - 64, 0xffffff,
				0, 0, 0, false);
		hit10(sh, 0, y, rw, 48, rail[i].id);
	}
	/* the power and account menus pop up beside the rail */
	if (sh->power_menu10 || sh->user_menu10) {
		bool pw = sh->power_menu10;
		const char *labels[3] = {pw ? "Sleep" : "Sign out", pw ? "Shut down" : "Change account settings", pw ? "Restart" : NULL};
		int ids[3] = {pw ? S10_SLEEP : S10_SIGNOUT, pw ? S10_SHUTDOWN : S10_LOCK, S10_RESTART};
		int cnt = pw ? 3 : 2, ph = cnt * 40 + 8, px = 48, py = pw ? mh - ph - 4 : mh - 5 * 48;
		fill_a(cr, px, py, 220, ph, 0.17, 1);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
		cairo_rectangle(cr, px + 0.5, py + 0.5, 219, ph - 1);
		cairo_stroke(cr);
		static const char *pglyph[3] = {"\uE708", "\uE7E8", "\uE777"};
		for (int i = 0; i < cnt; i++) {
			int y = py + 4 + i * 40;
			if (sh->hot10 == ids[i])
				fill_a(cr, px, y, 220, 40, 1, 0.12);
			if (pw)
				draw_mdl2(cr, pglyph[i], 10, px + 22, y + 20, 0xffffff);
			draw_text(cr, "Segoe UI 9", labels[i], px + (pw ? 44 : 16), y + 11, 0, 0xffffff, 0, 0, 0, false);
			hit10(sh, px, y, 220, 40, ids[i]);
		}
	}
	cairo_destroy(cr);
	if (bar10(sh))
		draw_taskbar10(sh); /* the search box there shows what's being typed */
	int my = sh->h - bar_h(sh) - mh;
	show(&sh->menu, srv->layer_menu, c, 0, my);
	wlr_scene_node_set_enabled(&sh->menu->node, true);
	if (acrylic) {
		glass_backdrop(srv, &sh->menu_glass, srv->layer_menu, 0, my, 0, my, mw, mh);
		if (sh->menu_glass)
			wlr_scene_node_place_below(&sh->menu_glass->node, &sh->menu->node);
	} else if (sh->menu_glass) {
		wlr_scene_node_set_enabled(&sh->menu_glass->node, false);
	}
}

static int hit10_at(struct shell *sh, double x, double y) {
	struct wlr_box mb = {0, sh->h - bar_h(sh) - sh->menu_h, W10_MENU_W, sh->menu_h};
	int id = 0;
	for (int i = 0; i < sh->n_hits10; i++)
		if (in_box(&sh->hits10[i].b, x - mb.x, y - mb.y))
			id = sh->hits10[i].id; /* later ones (pop-ups) win */
	return id;
}

static void launch_app(struct shell *sh, int app_index);

static bool start10_motion(struct shell *sh, double x, double y) {
	int id = hit10_at(sh, x, y);
	if (id != sh->hot10) {
		sh->hot10 = id;
		draw_menu10(sh);
	}
	return x < W10_MENU_W && y >= sh->h - bar_h(sh) - sh->menu_h && y < sh->h - bar_h(sh);
}

static void start10_click(struct shell *sh, int id) {
	bool keep_menus = id == S10_POWER || id == S10_USER;
	if (!keep_menus && id < S10_SLEEP)
		sh->power_menu10 = sh->user_menu10 = false;
	switch (id) {
	case S10_MENU: sh->rail_open = !sh->rail_open; break;
	case S10_USER: sh->user_menu10 = !sh->user_menu10; sh->power_menu10 = false; break;
	case S10_POWER: sh->power_menu10 = !sh->power_menu10; sh->user_menu10 = false; break;
	case S10_DOCS: shell_toggle_start(sh); spawn("expede-explorer ~/Documents"); return;
	case S10_PICS: shell_toggle_start(sh); spawn("expede-explorer ~/Pictures"); return;
	case S10_SETTINGS: shell_toggle_start(sh); spawn("expede-settings"); return;
	case S10_SIGNOUT: shell_toggle_start(sh); session_logoff(sh->server->session); return;
	case S10_LOCK: shell_toggle_start(sh); spawn("expede-settings users"); return;
	case S10_SLEEP: shell_toggle_start(sh); spawn("systemctl suspend"); return;
	case S10_SHUTDOWN: shell_toggle_start(sh); session_turnoff_dialog(sh->server->session); return;
	case S10_RESTART: shell_toggle_start(sh); spawn("systemctl reboot"); return;
	default:
		if (id >= S10_RESULT) {
			int r = id - S10_RESULT;
			if (r < sh->n_results)
				launch_app(sh, sh->results[r]);
			return;
		}
		if (id >= S10_TILE) {
			const char *cmd = tiles10[id - S10_TILE].cmd;
			shell_toggle_start(sh);
			spawn(cmd);
			return;
		}
		if (id >= S10_APP) {
			launch_app(sh, id - S10_APP);
			return;
		}
	}
	draw_menu10(sh);
}

static void draw_menu(struct shell *sh) {
	if (menu10(sh)) {
		draw_menu10(sh);
		return;
	}
	if (menu7(sh)) {
		draw_menu7(sh);
		return;
	}
	if (sh->menu_glass)
		wlr_scene_node_set_enabled(&sh->menu_glass->node, false);
	struct theme *t = menu_theme(sh);
	theme_use_fonts(t);
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
	sh->hot10 = 0;
	sh->scroll10 = 0;
	sh->rail_open = sh->power_menu10 = sh->user_menu10 = false;
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
	return (struct wlr_box){0, sh->h - bar_h(sh) - sh->menu_h, menu_w(sh), sh->menu_h};
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

static bool tv_motion(struct shell *sh, double x, double y);
static bool tv_button(struct shell *sh, double x, double y, bool pressed);
static bool ac_motion(struct shell *sh, double x, double y);
static bool ac_button(struct shell *sh, double x, double y, bool pressed);

bool shell_motion(struct shell *sh, double x, double y, bool over_view) {
	(void)over_view;
	if (sh->tv_open)
		return tv_motion(sh, x, y);
	if (sh->action_open && ac_motion(sh, x, y))
		return true;
	if (sh->start_open && menu10(sh) && start10_motion(sh, x, y))
		return true;
	if (sh->vol_drag) {
		vol_drag_to(sh, y);
		return true;
	}
	if (sh->bright_drag) {
		bright_drag_to(sh, x);
		return true;
	}
	if (sh->vol_open && in_box(&sh->vol_box, x, y))
		return true;
	if (sh->bat_open && in_box(&sh->bat_box, x, y))
		return true;
	if (sh->net_open && in_box(&sh->net_box, x, y)) {
		double ly = y - sh->net_box.y;
		int hot = (ly >= 32 && ly < 32 + sh->n_net_aps * NET_ROW_H) ? (int)(ly - 32) / NET_ROW_H : -1;
		if (hot != sh->net_hot) {
			sh->net_hot = hot;
			draw_netpop(sh);
		}
		return true;
	}
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
	bool start_hot = false, showdesk_hot = false, search_hot = false, taskview_hot = false, action_hot = false;
	if (on_bar) {
		double by = y - (sh->h - bar_h(sh));
		start_hot = x < start_w(sh);
		showdesk_hot = bar7(sh) && in_box(&sh->showdesk_box, x, by);
		if (bar10(sh)) {
			search_hot = in_box(&sh->search_box10, x, by);
			taskview_hot = in_box(&sh->taskview_box, x, by);
			action_hot = in_box(&sh->action_box, x, by);
		}
		for (int i = 0; i < sh->n_tasks; i++)
			if (in_box(&sh->tasks[i].box, x, by))
				task_hot = i;
	}
	if (task_hot != sh->task_hot || start_hot != sh->start_hot || showdesk_hot != sh->showdesk_hot ||
			search_hot != sh->search_hot || taskview_hot != sh->taskview_hot || action_hot != sh->action_hot) {
		sh->task_hot = task_hot;
		sh->start_hot = start_hot;
		sh->showdesk_hot = showdesk_hot;
		sh->search_hot = search_hot;
		sh->taskview_hot = taskview_hot;
		sh->action_hot = action_hot;
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
	if (sh->tv_open && !on_bar)
		return tv_button(sh, x, y, pressed);
	if (sh->start_open && menu10(sh) && x < W10_MENU_W && !on_bar && y >= sh->h - bar_h(sh) - sh->menu_h) {
		if (!pressed) {
			int id = hit10_at(sh, x, y);
			if (id)
				start10_click(sh, id);
		}
		return true;
	}
	if (sh->action_open) {
		if (ac_button(sh, x, y, pressed))
			return true;
		/* a click anywhere else closes it (the Action Center button toggles it below) */
		double by = y - (sh->h - bar_h(sh));
		if (pressed && !(on_bar && in_box(&sh->action_box, x, by)))
			shell_action_center(sh, false);
	}
	if (sh->bat_open || sh->bright_drag) {
		if (sh->bright_drag && !pressed) {
			sh->bright_drag = false;
			draw_batpop(sh);
			return true;
		}
		if (in_box(&sh->bat_box, x, y)) {
			if (pressed) {
				int ph = bat_pop_h(sh);
				int sy = sh->bat_pct >= 0 ? 56 : 12;
				double lx = x - sh->bat_box.x, ly = y - sh->bat_box.y;
				if (ly >= ph - 26) {
					bat_set_open(sh, false);
					spawn("expede-settings power");
				} else if (lx >= BAT_SLIDER_X - 6 && lx <= BAT_SLIDER_X + BAT_SLIDER_W + 6 &&
						ly >= sy + 12 && ly <= sy + 42) {
					sh->bright_drag = true;
					bright_drag_to(sh, x);
				}
			}
			return true;
		}
		if (pressed)
			bat_set_open(sh, false);
		if (on_bar && in_box(&sh->bat_icon_box, x, y - (sh->h - bar_h(sh))))
			return true;
	} else if (pressed && on_bar && in_box(&sh->bat_icon_box, x, y - (sh->h - bar_h(sh)))) {
		if (sh->start_open)
			shell_toggle_start(sh);
		if (sh->vol_open)
			vol_set_open(sh, false);
		if (sh->net_open)
			net_set_open(sh, false);
		bat_set_open(sh, true);
		return true;
	}
	if (sh->net_open) {
		if (in_box(&sh->net_box, x, y)) {
			if (pressed) {
				int ph = net_pop_h(sh);
				double ly = y - sh->net_box.y;
				if (ly >= ph - 26) {
					net_set_open(sh, false);
					spawn("expede-settings network");
				} else if (ly >= 32 && ly < 32 + sh->n_net_aps * NET_ROW_H) {
					int idx = (int)(ly - 32) / NET_ROW_H;
					net_set_open(sh, false);
					if (sh->net_aps[idx].sec && !sh->net_aps[idx].active) {
						spawn("expede-settings network");
					} else {
						char cmd[256], q[160];
						size_t o = 0;
						q[o++] = '\'';
						for (const char *s = sh->net_aps[idx].ssid; *s && o + 5 < sizeof(q); s++)
							if (*s == '\'') { memcpy(q + o, "'\\''", 4); o += 4; }
							else q[o++] = *s;
						q[o++] = '\'';
						q[o] = 0;
						snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s >/dev/null 2>&1 || expede-settings network", q);
						spawn(cmd);
					}
				}
			}
			return true;
		}
		if (pressed)
			net_set_open(sh, false);
		if (on_bar && in_box(&sh->net_icon_box, x, y - (sh->h - bar_h(sh))))
			return true;
	} else if (pressed && on_bar && in_box(&sh->net_icon_box, x, y - (sh->h - bar_h(sh)))) {
		if (sh->start_open)
			shell_toggle_start(sh);
		if (sh->vol_open)
			vol_set_open(sh, false);
		if (sh->bat_open)
			bat_set_open(sh, false);
		net_set_open(sh, true);
		return true;
	}
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
		if (bar10(sh) && in_box(&sh->search_box10, x, by)) {
			/* Windows 10 searches from Start: open it, ready to type */
			shell_toggle_start(sh);
			return true;
		}
		if (bar10(sh) && in_box(&sh->taskview_box, x, by)) {
			shell_task_view(sh);
			return true;
		}
		if (bar10(sh) && in_box(&sh->action_box, x, by)) {
			shell_action_center(sh, !sh->action_open);
			return true;
		}
		if (bar7(sh) && in_box(&sh->showdesk_box, x, by)) {
			if (sh->start_open)
				shell_toggle_start(sh);
			bool any_vis = false;
			for (int i = 0; i < sh->n_tasks; i++)
				if (!sh->tasks[i].view->minimized)
					any_vis = true;
			if (any_vis) {
				sh->n_peeked = 0;
				for (int i = 0; i < sh->n_tasks && sh->n_peeked < 64; i++)
					if (!sh->tasks[i].view->minimized) {
						sh->peeked[sh->n_peeked++] = sh->tasks[i].view;
						view_set_minimized(sh->tasks[i].view, true);
					}
				view_focus(NULL);
			} else {
				for (int i = 0; i < sh->n_peeked; i++)
					for (int j = 0; j < sh->n_tasks; j++)
						if (sh->tasks[j].view == sh->peeked[i])
							view_set_minimized(sh->peeked[i], false);
				if (sh->n_peeked > 0)
					view_focus(sh->peeked[sh->n_peeked - 1]);
				sh->n_peeked = 0;
			}
			return true;
		}
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
	bool on_bar = y >= sh->h - bar_h(sh);
	if (sh->start_open && menu10(sh) && x < W10_MENU_W && !on_bar) {
		int n;
		apps_all(&n);
		int max = n * 36 + 27 * 36 - sh->menu_h;
		sh->scroll10 += delta > 0 ? 108 : -108;
		if (sh->scroll10 > max) sh->scroll10 = max;
		if (sh->scroll10 < 0) sh->scroll10 = 0;
		sh->hot10 = 0;
		draw_menu10(sh);
		return true;
	}
	if ((sh->bat_open && in_box(&sh->bat_box, x, y)) ||
			(on_bar && in_box(&sh->bat_icon_box, x, y - (sh->h - bar_h(sh))))) {
		shell_brightness_key(sh, delta < 0 ? 5 : -5);
		return true;
	}
	if ((sh->vol_open && in_box(&sh->vol_box, x, y)) ||
			(on_bar && sh->vol_ok && in_box(&sh->vol_icon_box, x, y - (sh->h - bar_h(sh))))) {
		shell_volume_key(sh, delta < 0 ? 5 : -5, false);
		return true;
	}
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

/* ---------------------------------------------------------------- Windows 10: Task View */

static void tv_free(struct shell *sh) {
	for (int i = 0; i < sh->n_tv; i++)
		if (sh->tv[i].shot)
			cairo_surface_destroy(sh->tv[i].shot);
	sh->n_tv = 0;
}

static void draw_task_view(struct shell *sh) {
	if (!sh->tv_open) {
		if (sh->tv_node)
			wlr_scene_node_set_enabled(&sh->tv_node->node, false);
		return;
	}
	struct server *srv = sh->server;
	theme_use_fonts(bar_theme(sh));
	int h = sh->h - bar_h(sh);
	cairo_surface_t *c = canvas_new(sh->w, h);
	cairo_t *cr = cairo_create(c);
	/* the desktop, dimmed and blurred, behind the windows */
	cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, srv->opt_transparency ? 0.75 : 0.97);
	cairo_paint(cr);
	if (!sh->n_tv)
		draw_text(cr, "Segoe UI Light 20", "No apps open", sh->w / 2 - 70, h / 2 - 20, 0, 0xffffff, 0, 0, 0, false);
	for (int i = 0; i < sh->n_tv; i++) {
		struct wlr_box b = sh->tv[i].box;
		bool hot = i == sh->tv_hot;
		const char *title = sh->tv[i].view->xt->title ? sh->tv[i].view->xt->title : "";
		if (sh->tv[i].view->icon.s16)
			draw_surface(cr, sh->tv[i].view->icon.s16, b.x, b.y - 24);
		draw_text(cr, "Segoe UI 9", title, b.x + 22, b.y - 24, b.width - 52, 0xffffff, 0, 0, 0, false);
		if (hot) {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.6);
			cairo_rectangle(cr, b.x - 3, b.y - 3, b.width + 6, b.height + 6);
			cairo_fill(cr);
			/* the close button Task View shows on the hovered window */
			struct wlr_box x = sh->tv[i].close;
			cairo_set_source_rgb(cr, sh->tv_close_hot ? 0.91 : 0.2, sh->tv_close_hot ? 0.07 : 0.2, sh->tv_close_hot ? 0.14 : 0.2);
			cairo_rectangle(cr, x.x, x.y, x.width, x.height);
			cairo_fill(cr);
			draw_mdl2(cr, "\uE8BB", 7.5, x.x + x.width / 2.0, x.y + x.height / 2.0, 0xffffff);
		}
		cairo_surface_t *shot = sh->tv[i].shot;
		if (shot) {
			double sw = cairo_image_surface_get_width(shot), shh = cairo_image_surface_get_height(shot);
			cairo_save(cr);
			cairo_translate(cr, b.x, b.y);
			cairo_scale(cr, b.width / sw, b.height / shh);
			cairo_set_source_surface(cr, shot, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	cairo_destroy(cr);
	show(&sh->tv_node, srv->layer_menu, c, 0, 0);
	wlr_scene_node_set_enabled(&sh->tv_node->node, true);
}

/* lay the windows out in rows like Windows 10, each as big as fits */
static void tv_layout(struct shell *sh) {
	int n = sh->n_tv;
	if (!n)
		return;
	int area_w = sh->w - 80, area_h = sh->h - bar_h(sh) - 100;
	int cols = 1;
	while (cols * cols < n)
		cols++;
	int rows = (n + cols - 1) / cols;
	int cw = area_w / cols, ch = area_h / rows;
	for (int i = 0; i < n; i++) {
		cairo_surface_t *shot = sh->tv[i].shot;
		double sw = shot ? cairo_image_surface_get_width(shot) : 4, shh = shot ? cairo_image_surface_get_height(shot) : 3;
		double k = fmin((cw - 40) / sw, (ch - 60) / shh);
		if (k > 0.6 * ui_scale)
			k = 0.6 * ui_scale;
		int w = (int)(sw * k), h = (int)(shh * k);
		int r = i / cols, c = i % cols, in_row = r == rows - 1 ? n - r * cols : cols;
		int x0 = (sh->w - in_row * cw) / 2;
		int x = x0 + c * cw + (cw - w) / 2, y = 60 + r * ch + (ch - h) / 2 + 12;
		sh->tv[i].box = (struct wlr_box){x, y, w, h};
		sh->tv[i].close = (struct wlr_box){x + w - 32, y - 32, 32, 30};
	}
}

void shell_task_view(struct shell *sh) {
	if (sh->tv_open) {
		sh->tv_open = false;
		tv_free(sh);
		draw_task_view(sh);
		return;
	}
	if (sh->start_open)
		shell_toggle_start(sh);
	shell_action_center(sh, false);
	tv_free(sh);
	for (int i = 0; i < sh->n_tasks && sh->n_tv < 32; i++) {
		sh->tv[sh->n_tv].view = sh->tasks[i].view;
		sh->tv[sh->n_tv].shot = view_snapshot(sh->tasks[i].view);
		sh->n_tv++;
	}
	sh->tv_open = true;
	sh->tv_hot = -1;
	tv_layout(sh);
	draw_task_view(sh);
}

static bool tv_motion(struct shell *sh, double x, double y) {
	int hot = -1;
	bool close_hot = false;
	for (int i = 0; i < sh->n_tv; i++) {
		struct wlr_box b = sh->tv[i].box;
		b.y -= 32;
		b.height += 32;
		if (in_box(&b, x, y)) {
			hot = i;
			close_hot = in_box(&sh->tv[i].close, x, y);
		}
	}
	if (hot != sh->tv_hot || close_hot != sh->tv_close_hot) {
		sh->tv_hot = hot;
		sh->tv_close_hot = close_hot;
		draw_task_view(sh);
	}
	return true;
}

static bool tv_button(struct shell *sh, double x, double y, bool pressed) {
	if (pressed)
		return true;
	tv_motion(sh, x, y);
	int i = sh->tv_hot;
	if (i >= 0 && sh->tv_close_hot) {
		view_close(sh->tv[i].view);
		return true;
	}
	struct view *v = i >= 0 ? sh->tv[i].view : NULL;
	shell_task_view(sh); /* closes it */
	if (v) {
		if (v->minimized)
			view_set_minimized(v, false);
		view_focus(v);
	}
	return true;
}

/* ---------------------------------------------------------------- Windows 10: Action Center */

#define AC_W 396
enum { AC_SAVER, AC_NETWORK, AC_SETTINGS, AC_BLUETOOTH, AC_WIFI, AC_AIRPLANE, AC_SNIP, AC_FOCUS };
static const struct { const char *label, *glyph; } ac_tiles[8] = {
	{"Battery saver", "\uE8BE"}, {"Network", "\uE839"}, {"All settings", "\uE713"}, {"Bluetooth", "\uE702"},
	{"Wi-Fi", "\uE701"}, {"Airplane mode", "\uE709"}, {"Screen snip", "\uE7F8"}, {"Focus assist", "\uE708"},
};

static bool cmd_says(const char *cmd, const char *want) {
	FILE *p = popen(cmd, "r");
	char buf[256] = "";
	if (p) {
		size_t n = fread(buf, 1, sizeof(buf) - 1, p);
		buf[n] = 0;
		pclose(p);
	}
	return strstr(buf, want) != NULL;
}

static void ac_read_state(struct shell *sh) {
	sh->ac_on[AC_SAVER] = cmd_says("powerprofilesctl get 2>/dev/null", "power-saver");
	sh->ac_on[AC_BLUETOOTH] = cmd_says("bluetoothctl show 2>/dev/null", "Powered: yes");
	sh->ac_on[AC_WIFI] = cmd_says("nmcli radio wifi 2>/dev/null", "enabled");
	sh->ac_on[AC_AIRPLANE] = cmd_says("nmcli radio all 2>/dev/null", "disabled") && !sh->ac_on[AC_WIFI];
	sh->ac_on[AC_FOCUS] = !sh->server->opt_sounds;
	sh->ac_bright_pct = sh->bright_pct;
}

static void draw_action_center(struct shell *sh) {
	struct server *srv = sh->server;
	if (!sh->action_open) {
		if (sh->ac_node)
			wlr_scene_node_set_enabled(&sh->ac_node->node, false);
		if (sh->ac_glass)
			wlr_scene_node_set_enabled(&sh->ac_glass->node, false);
		return;
	}
	theme_use_fonts(bar_theme(sh));
	int h = sh->h - bar_h(sh), x0 = sh->w - AC_W;
	cairo_surface_t *c = canvas_new(AC_W, h);
	cairo_t *cr = cairo_create(c);
	bool acrylic = srv->opt_transparency && srv->glass_buf;
	cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, acrylic ? 0.88 : 1);
	cairo_paint(cr);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
	cairo_rectangle(cr, 0, 0, 1, h);
	cairo_fill(cr);
	uint32_t link = 0x99ebff;
	draw_text(cr, "Segoe UI 9", "Manage notifications", AC_W - 16 - text_width("Segoe UI 9", "Manage notifications"), 12,
		0, link, 0, 0, 0, false);
	int nw = text_width("Segoe UI 11", "No new notifications");
	draw_text(cr, "Segoe UI 11", "No new notifications", (AC_W - nw) / 2, 44, 0, 0xc8c8c8, 0, 0, 0, false);
	/* quick actions: 4 across, accent when on */
	int tw = (AC_W - 16 - 3 * 2) / 4, th = 58, ty = h - 16 - 40 - 2 * th - 2 - 24;
	draw_text(cr, "Segoe UI 9", "Collapse", 16, ty - 22, 0, link, 0, 0, 0, false);
	for (int i = 0; i < 8; i++) {
		int x = 8 + (i % 4) * (tw + 2), y = ty + (i / 4) * (th + 2);
		sh->ac_tiles[i] = (struct wlr_box){x0 + x, y, tw, th};
		bool on = sh->ac_on[i], hot = i == sh->ac_hot;
		uint32_t bg = on ? srv->accent_rgb : 0x3b3b3b;
		cairo_set_source_rgb(cr, ((bg >> 16) & 255) / 255.0, ((bg >> 8) & 255) / 255.0, (bg & 255) / 255.0);
		cairo_rectangle(cr, x, y, tw, th);
		cairo_fill(cr);
		if (hot) {
			cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, x + 1, y + 1, tw - 2, th - 2);
			cairo_stroke(cr);
		}
		draw_mdl2(cr, ac_tiles[i].glyph, 11, x + 18, y + 18, 0xffffff);
		draw_text(cr, "Segoe UI 8", ac_tiles[i].label, x + 8, y + th - 22, tw - 12, 0xffffff, 0, 0, 0, false);
	}
	/* brightness */
	int by = h - 40;
	draw_mdl2(cr, "\uE706", 11, 26, by + 14, 0xffffff);
	int sx = 48, sw = AC_W - 48 - 24;
	sh->ac_bright = (struct wlr_box){x0 + sx, by, sw, 28};
	cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
	cairo_rectangle(cr, sx, by + 13, sw, 2);
	cairo_fill(cr);
	int pos = sx + sw * sh->ac_bright_pct / 100;
	cairo_set_source_rgb(cr, ((srv->accent_rgb >> 16) & 255) / 255.0, ((srv->accent_rgb >> 8) & 255) / 255.0,
		(srv->accent_rgb & 255) / 255.0);
	cairo_rectangle(cr, sx, by + 13, pos - sx, 2);
	cairo_fill(cr);
	cairo_rectangle(cr, pos - 4, by + 4, 8, 20);
	cairo_fill(cr);
	cairo_destroy(cr);
	show(&sh->ac_node, srv->layer_menu, c, x0, 0);
	wlr_scene_node_set_enabled(&sh->ac_node->node, true);
	if (acrylic) {
		glass_backdrop(srv, &sh->ac_glass, srv->layer_menu, x0, 0, x0, 0, AC_W, h);
		if (sh->ac_glass)
			wlr_scene_node_place_below(&sh->ac_glass->node, &sh->ac_node->node);
	}
}

void shell_action_center(struct shell *sh, bool open) {
	if (open == sh->action_open)
		return;
	if (open && sh->start_open)
		shell_toggle_start(sh);
	sh->action_open = open;
	sh->ac_hot = -1;
	if (open)
		ac_read_state(sh);
	draw_action_center(sh);
}

bool shell_overlay_open(struct shell *sh) {
	return sh->tv_open || sh->action_open;
}

void shell_close_overlays(struct shell *sh) {
	if (sh->tv_open)
		shell_task_view(sh);
	shell_action_center(sh, false);
}

static void ac_set_bright(struct shell *sh, double x) {
	int pct = (int)((x - sh->ac_bright.x) * 100 / sh->ac_bright.width);
	pct = pct < 1 ? 1 : pct > 100 ? 100 : pct;
	if (pct == sh->ac_bright_pct)
		return;
	sh->ac_bright_pct = pct;
	char cmd[64];
	snprintf(cmd, sizeof(cmd), "brightnessctl -q set %d%%", pct);
	spawn(cmd);
	draw_action_center(sh);
}

static void ac_toggle(struct shell *sh, int i) {
	bool on = sh->ac_on[i];
	switch (i) {
	case AC_SAVER: spawn(on ? "powerprofilesctl set balanced" : "powerprofilesctl set power-saver"); break;
	case AC_NETWORK: shell_action_center(sh, false); spawn("expede-settings network"); return;
	case AC_SETTINGS: shell_action_center(sh, false); spawn("expede-settings"); return;
	case AC_BLUETOOTH: spawn(on ? "bluetoothctl power off" : "bluetoothctl power on"); break;
	case AC_WIFI: spawn(on ? "nmcli radio wifi off" : "nmcli radio wifi on"); break;
	case AC_AIRPLANE: spawn(on ? "nmcli radio all on" : "nmcli radio all off"); break;
	case AC_SNIP: shell_action_center(sh, false); spawn("expede-snip"); return;
	case AC_FOCUS: sh->server->opt_sounds = on; break; /* focus assist keeps things quiet */
	}
	sh->ac_on[i] = !on;
	draw_action_center(sh);
}

static bool ac_motion(struct shell *sh, double x, double y) {
	if (sh->ac_bright_drag) {
		ac_set_bright(sh, x);
		return true;
	}
	if (x < sh->w - AC_W || y >= sh->h - bar_h(sh))
		return false;
	int hot = -1;
	for (int i = 0; i < 8; i++)
		if (in_box(&sh->ac_tiles[i], x, y))
			hot = i;
	if (hot != sh->ac_hot) {
		sh->ac_hot = hot;
		draw_action_center(sh);
	}
	return true;
}

/* true when the panel used the click */
static bool ac_button(struct shell *sh, double x, double y, bool pressed) {
	if (sh->ac_bright_drag && !pressed) {
		sh->ac_bright_drag = false;
		return true;
	}
	if (x < sh->w - AC_W || y >= sh->h - bar_h(sh))
		return false;
	if (pressed && in_box(&sh->ac_bright, x, y)) {
		sh->ac_bright_drag = true;
		ac_set_bright(sh, x);
	} else if (!pressed) {
		for (int i = 0; i < 8; i++)
			if (in_box(&sh->ac_tiles[i], x, y))
				ac_toggle(sh, i);
	}
	return true;
}

/* ---------------------------------------------------------------- setup */

void shell_reconfigure(struct shell *sh) {
	if (sh->start_open)
		shell_toggle_start(sh);
	if (sh->vol_open)
		vol_set_open(sh, false);
	if (sh->bat_open)
		bat_set_open(sh, false);
	load_tray_icons(sh);
	for (int i = 0; i < sh->n_icons; i++)
		sh->icons[i].img = theme_icon(desk_theme(sh), sh->icons[i].icon);
	for (int i = 0; i < sh->n_items; i++)
		if (sh->items[i].icon)
			sh->items[i].img = theme_icon(menu_theme(sh), sh->items[i].icon);
	sh->modern = menu7(sh) || sh->server->opt_modern_start;
	sh->menu_h = menu10(sh) ? (sh->h - bar_h(sh) > 680 ? 640 : sh->h - bar_h(sh) - 40) :
		menu7(sh) ? W7_MENU_H : MENU_H + (sh->modern ? SEARCH_H : 0);
	layout_menu(sh);
	tick(sh);
	draw_desktop(sh);
	draw_taskbar(sh);
	draw_menu(sh);
}

/* Windows shows the wallpaper (no icons) on every extended screen */
static void draw_other_desktops(struct shell *sh) {
	struct wlr_box boxes[4];
	int n = server_other_outputs(sh->server, boxes, 4);
	struct theme *t = desk_theme(sh);
	for (int i = 0; i < 4; i++) {
		if (i >= n) {
			if (sh->other_desk[i])
				wlr_scene_node_set_enabled(&sh->other_desk[i]->node, false);
			continue;
		}
		struct wlr_box b = boxes[i];
		cairo_surface_t *c = canvas_new(b.width, b.height);
		cairo_t *cr = cairo_create(c);
		cairo_set_source_rgb(cr, t->desktop_bg[0], t->desktop_bg[1], t->desktop_bg[2]);
		cairo_paint(cr);
		if (t->wallpaper) {
			double ww = cairo_image_surface_get_width(t->wallpaper), wh = cairo_image_surface_get_height(t->wallpaper);
			if (t->wallpaper_style == WALL_TILE) {
				cairo_set_source_surface(cr, t->wallpaper, 0, 0);
				cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
			} else if (t->wallpaper_style == WALL_CENTER) {
				cairo_set_source_surface(cr, t->wallpaper, (int)((b.width - ww) / 2), (int)((b.height - wh) / 2));
			} else {
				cairo_scale(cr, b.width / ww, b.height / wh);
				cairo_set_source_surface(cr, t->wallpaper, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			}
			cairo_paint(cr);
		}
		cairo_destroy(cr);
		show(&sh->other_desk[i], sh->server->layer_desktop, c, b.x, b.y);
	}
}

void shell_layout(struct shell *sh) {
	server_screen(sh->server, &sh->w, &sh->h);
	draw_other_desktops(sh);
	if (sh->bright_veil)
		wlr_scene_rect_set_size(sh->bright_veil, sh->w, sh->h);
	draw_desktop(sh);
	draw_taskbar(sh);
	draw_menu(sh);
}

struct shell *shell_create(struct server *s) {
	struct shell *sh = calloc(1, sizeof(*sh));
	sh->server = s;
	sh->menu_hot = sh->task_hot = sh->icon_sel = sh->list_hot = sh->list_sel = sh->cascade_hot = -1;
	sh->modern = menu7(sh) || s->opt_modern_start;
	sh->menu_h = menu10(sh) ? (sh->h - bar_h(sh) > 680 ? 640 : sh->h - bar_h(sh) - 40) :
		menu7(sh) ? W7_MENU_H : MENU_H + (sh->modern ? SEARCH_H : 0);
	tick(sh);
	apps_load();
	update_results(sh);
	load_tray_icons(sh);
	sh->vol_ok = volume_available() && sh->vol_icon.s16;
	if (sh->vol_ok)
		vol_refresh(sh);

	add_icon(sh, "My Documents", getenv("USER"), "my-documents", "expede-explorer ~");
	add_icon(sh, "My Computer", "Computer", "computer", "expede-explorer computer:");
	add_icon(sh, "My Network Places", "Network", "network", "pcmanfm-qt network:///");
	add_icon(sh, "Recycle Bin", "Recycle Bin", "recycle-empty", "expede-explorer trash:");

	add_item(sh, ITEM_PINNED, "Command Prompt", "Command Prompt", "cmd", "foot");
	add_item(sh, ITEM_PINNED, "Windows Explorer", "Windows Explorer", "explorer", "expede-explorer ~");
	add_item(sh, ITEM_SEP_LEFT, NULL, "", NULL, NULL);
	add_item(sh, ITEM_MFU, "Notepad", "Notepad", "notepad", "mousepad");
	add_item(sh, ITEM_MFU, "Snipping Tool", "Snipping Tool", "my-pictures", "expede-snip");
	add_item(sh, ITEM_ALL_PROGRAMS, "All Programs", "All Programs", NULL, NULL);
	add_item(sh, ITEM_PLACE_BOLD, "My Documents", "Documents", "my-documents", "expede-explorer ~");
	add_item(sh, ITEM_PLACE_BOLD, "My Pictures", "Pictures", "my-pictures", "expede-explorer ~/Pictures");
	add_item(sh, ITEM_PLACE_BOLD, "My Music", "Music", "my-music", "expede-explorer ~/Music");
	add_item(sh, ITEM_PLACE_BOLD, "My Computer", "Computer", "computer", "expede-explorer computer:");
	add_item(sh, ITEM_SEP_RIGHT, NULL, "", NULL, NULL);
	add_item(sh, ITEM_PLACE, "Control Panel", "Control Panel", "control-panel", "expede-settings");
	add_item(sh, ITEM_PLACE, "Help and Support", "Help and Support", "help", NULL);
	add_item(sh, ITEM_PLACE, "Search", NULL, "search", "expede-explorer ~");
	add_item(sh, ITEM_PLACE, "Run...", NULL, "run", "foot");
	add_item(sh, ITEM_LOGOFF, "Log Off", "", NULL, NULL);
	add_item(sh, ITEM_TURNOFF, "Turn Off Computer", "Shut down", NULL, NULL);
	layout_menu(sh);


	sh->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display), clock_timer, sh);
	wl_event_source_timer_update(sh->timer, 1000);
	return sh;
}
