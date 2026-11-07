/* expede-settings: Expede's Control Panel. Category view (or Classic view) in the
 * main window, and each applet as its own XP property sheet with tabs and
 * OK / Cancel / Apply, all drawn with the Luna bitmaps from the user's XP files.
 *
 * Saving writes ~/.config/expede/expede.conf and sends SIGHUP to the running
 * compositor, which applies everything at once. The volume slider talks to the
 * sound system directly, as XP's did.
 *
 *   expede-settings                 the Control Panel
 *   expede-settings display|taskbar|sounds|mouse|datetime|users   one applet */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "theme.h"
#include "volume.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

/* ------------------------------------------------------------------ the options */

struct config {
	bool snap, modern, tap, natural, sounds, clock24;
	int scale_pct, speed;
	char wallpaper[256], wall_style[16], picture[64], keyboard[64];
};

static struct config cfg, saved;

static void conf_path(char *out, size_t len) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(out, len, "%s/expede/expede.conf", xdg);
	else
		snprintf(out, len, "%s/.config/expede/expede.conf", getenv("HOME"));
}

static bool truthy(const char *v) {
	return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1");
}

static void config_load(void) {
	memset(&cfg, 0, sizeof(cfg)); /* zeroed padding keeps the memcmp() "changed?" test honest */
	cfg.tap = cfg.natural = cfg.sounds = true;
	snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "Bliss.jpg");
	snprintf(cfg.wall_style, sizeof(cfg.wall_style), "auto");
	char path[512], line[512], key[64], val[256];
	conf_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char *eq = strchr(line, '=');
			if (line[0] == '#' || !eq || sscanf(line, " %63[^= ]", key) != 1)
				continue;
			char *v = eq + 1;
			while (*v == ' ' || *v == '\t')
				v++;
			size_t n = strcspn(v, "\r\n");
			while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
				n--;
			snprintf(val, sizeof(val), "%.*s", (int)n, v);
			if (!strcmp(key, "snap")) cfg.snap = truthy(val);
			else if (!strcmp(key, "tap_to_click")) cfg.tap = truthy(val);
			else if (!strcmp(key, "natural_scroll")) cfg.natural = truthy(val);
			else if (!strcmp(key, "sounds")) cfg.sounds = truthy(val);
			else if (!strcmp(key, "clock")) cfg.clock24 = !strcmp(val, "24");
			else if (!strcmp(key, "scale")) cfg.scale_pct = !strcasecmp(val, "auto") ? 0 : atoi(val);
			else if (!strcmp(key, "pointer_speed")) cfg.speed = atoi(val);
			else if (!strcmp(key, "wallpaper")) snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", !strcasecmp(val, "none") ? "" : val);
			else if (!strcmp(key, "wallpaper_style")) snprintf(cfg.wall_style, sizeof(cfg.wall_style), "%s", val);
			else if (!strcmp(key, "user_picture")) snprintf(cfg.picture, sizeof(cfg.picture), "%s", val);
			else if (!strcmp(key, "keyboard_layout")) snprintf(cfg.keyboard, sizeof(cfg.keyboard), "%s", val);
		}
		fclose(f);
	}
	saved = cfg;
}

static void config_save(void) {
	char path[512];
	conf_path(path, sizeof(path));
	char *dir = strdup(path);
	*strrchr(dir, '/') = 0;
	mkdir(dir, 0755);
	free(dir);
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "expede-settings: cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	fprintf(f, "# Expede settings (written by the Control Panel)\n");
	fprintf(f, "snap = %s\n", cfg.snap ? "true" : "false");
	fprintf(f, "tap_to_click = %s\n", cfg.tap ? "true" : "false");
	fprintf(f, "natural_scroll = %s\n", cfg.natural ? "true" : "false");
	fprintf(f, "pointer_speed = %d\n", cfg.speed);
	fprintf(f, "sounds = %s\n", cfg.sounds ? "true" : "false");
	fprintf(f, "clock = %s\n", cfg.clock24 ? "24" : "12");
	if (cfg.scale_pct)
		fprintf(f, "scale = %d\n", cfg.scale_pct);
	else
		fprintf(f, "scale = auto\n");
	fprintf(f, "wallpaper = %s\n", cfg.wallpaper[0] ? cfg.wallpaper : "none");
	fprintf(f, "wallpaper_style = %s\n", cfg.wall_style);
	if (cfg.picture[0])
		fprintf(f, "user_picture = %s\n", cfg.picture);
	if (cfg.keyboard[0])
		fprintf(f, "keyboard_layout = %s\n", cfg.keyboard);
	fclose(f);
	saved = cfg;
	/* tell the running desktop to reread it */
	spawn("pkill -HUP -x expede");
}

void spawn(const char *cmd) {
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
}

/* ------------------------------------------------------------------ lists of things */

struct choice { char *value, *label; cairo_surface_t *img; };

static struct choice *wallpapers; /* [0] is (None) */
static int n_wallpapers;
static struct choice *pictures;
static int n_pictures;

static const struct { const char *value, *label; } layouts[] = {
	{"", "System default"}, {"us", "English (US)"}, {"gb", "English (UK)"}, {"us(intl)", "English (US, international)"},
	{"ie", "Irish"}, {"de", "German"}, {"fr", "French"}, {"es", "Spanish"}, {"it", "Italian"}, {"pt", "Portuguese"},
	{"br", "Portuguese (Brazil)"}, {"nl", "Dutch"}, {"be", "Belgian"}, {"ch", "Swiss"}, {"se", "Swedish"},
	{"no", "Norwegian"}, {"dk", "Danish"}, {"fi", "Finnish"}, {"pl", "Polish"}, {"cz", "Czech"}, {"hu", "Hungarian"},
	{"ru", "Russian"}, {"ua", "Ukrainian"}, {"gr", "Greek"}, {"tr", "Turkish"}, {"jp", "Japanese"}, {"kr", "Korean"},
	{"us(dvorak)", "English (Dvorak)"}, {"us(colemak)", "English (Colemak)"},
};
#define N_LAYOUTS ((int)(sizeof(layouts) / sizeof(*layouts)))

static int by_label(const void *a, const void *b) {
	return strcasecmp(((const struct choice *)a)->label, ((const struct choice *)b)->label);
}

static void list_dir(const char *dir, struct choice **out, int *n, bool wallpaper) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		*out = realloc(*out, (*n + 1) * sizeof(**out));
		char *label = strdup(e->d_name);
		char *dot = strstr(label, wallpaper ? ".tile." : ".png");
		if (!dot)
			dot = strrchr(label, '.');
		if (dot)
			*dot = 0;
		(*out)[(*n)++] = (struct choice){strdup(e->d_name), label, NULL};
	}
	closedir(d);
	qsort(*out, *n, sizeof(**out), by_label);
}

/* ------------------------------------------------------------------ windows */

enum kind { K_CP, K_DISPLAY, K_TASKBAR, K_SOUNDS, K_MOUSE, K_DATETIME, K_USERS, K_COUNT };

static const struct applet {
	const char *arg, *category, *classic, *title, *icon, *tabs[2];
} applets[K_COUNT] = {
	[K_CP] = {NULL, NULL, NULL, "Control Panel", "control-panel", {NULL, NULL}},
	[K_DISPLAY] = {"display", "Appearance and Themes", "Display", "Display Properties", "cp-appearance", {"Desktop", "Appearance"}},
	[K_TASKBAR] = {"taskbar", "Taskbar and Start Menu", "Taskbar and Start Menu", "Taskbar and Start Menu Properties", "cp-taskbar", {"Taskbar", "Start Menu"}},
	[K_SOUNDS] = {"sounds", "Sounds, Speech, and Audio\nDevices", "Sounds and Audio Devices", "Sounds and Audio Devices Properties", "cp-sounds", {"Volume", "Sounds"}},
	[K_MOUSE] = {"mouse", "Printers and Other\nHardware", "Mouse", "Mouse Properties", "cp-hardware", {"Pointer Options", "Touchpad"}},
	[K_DATETIME] = {"datetime", "Date, Time, Language, and\nRegional Options", "Date, Time and Keyboard", "Date and Time Properties", "cp-datetime", {"Time", "Keyboard"}},
	[K_USERS] = {"users", "User Accounts", "User Accounts", "User Accounts", "cp-users", {"Picture", NULL}},
};

#define SHEET_W 410
#define SHEET_H 450
#define CP_W 660
#define CP_H 470

/* widget ids */
enum {
	ID_NONE, ID_OK, ID_CANCEL, ID_APPLY, ID_TAB0, ID_TAB1, ID_VIEW_TOGGLE,
	ID_SNAP, ID_CLASSIC, ID_MODERN, ID_TAP, ID_NATURAL, ID_SPEED, ID_VOLUME, ID_MUTE,
	ID_SCHEME_DEFAULT, ID_SCHEME_NONE, ID_TEST_SOUND, ID_CLOCK12, ID_CLOCK24,
	ID_STRETCH, ID_CENTER, ID_TILE,
	ID_SCALE0 = 100,      /* + 0..5 */
	ID_CATEGORY = 200,    /* + kind */
	ID_LIST = 1000,       /* + row */
};

struct rect { int id, x, y, w, h; };

struct win {
	enum kind kind;
	bool alive, configured;
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	struct zxdg_toplevel_decoration_v1 *deco;
	int w, h, scale, tab, scroll;
	struct rect rects[160];
	int n_rects, hot, pressed;
	bool dragging;
	struct wlr_box_ { int x, y, w, h; } list_box;
};

static struct win wins[K_COUNT];

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct xdg_wm_base *wm_base;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct theme theme;
	struct win *focus;
	double px, py;
	bool classic_view;
	int volume;
	bool muted, have_volume;
	char device[160];
	cairo_surface_t *preview; /* the wallpaper in Display's little monitor */
	char preview_for[256];
	struct icon_set icons[K_COUNT];
} app;

static void redraw(struct win *w);
static void open_window(enum kind k);

/* ------------------------------------------------------------------ drawing helpers */

static void rgb(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

static void fill(cairo_t *cr, double x, double y, double w, double h, uint32_t c) {
	rgb(cr, c);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void reg(struct win *w, int id, int x, int y, int ww, int hh) {
	if (w->n_rects < (int)(sizeof(w->rects) / sizeof(*w->rects)))
		w->rects[w->n_rects++] = (struct rect){id, x, y, ww, hh};
}

static int state(struct win *w, int id) {
	return w->pressed == id && w->hot == id ? 2 : w->hot == id ? 1 : 0;
}

static void text(cairo_t *cr, const char *font, const char *s, double x, double y, double maxw, uint32_t c) {
	draw_text(cr, font, s, x, y, maxw, c, 0, 0, 0, true);
}

static void scaled(cairo_t *cr, cairo_surface_t *s, double x, double y, double size) {
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

/* checkbox frames: unchecked n/h/p/d, checked n/h/p/d; radio: unchecked x4, checked x4 */
static void check(struct win *w, cairo_t *cr, int id, int x, int y, bool on, bool radio, const char *label) {
	draw_frame_true(cr, radio ? app.theme.radio : app.theme.checkbox, radio ? 8 : 12, (on ? 4 : 0) + state(w, id), x, y + 1);
	text(cr, "Tahoma 8", label, x + 19, y + 1, 0, 0x000000);
	reg(w, id, x, y, 19 + text_width("Tahoma 8", label) + 4, 16);
}

static void button(struct win *w, cairo_t *cr, int id, int x, int y, int bw, const char *label, bool disabled, bool def) {
	/* push button frames: normal hot pressed disabled default */
	int f = disabled ? 3 : state(w, id) ? state(w, id) : def ? 4 : 0;
	draw_image(cr, &app.theme.push_button, f, x, y, bw, 23);
	int tw = text_width("Tahoma 8", label);
	text(cr, "Tahoma 8", label, x + (bw - tw) / 2, y + 5, 0, disabled ? 0xaca899 : 0x000000);
	if (!disabled)
		reg(w, id, x, y, bw, 23);
}

static void group(cairo_t *cr, int x, int y, int gw, int gh, const char *title) {
	draw_image(cr, &app.theme.groupbox, 0, x, y + 6, gw, gh - 6);
	int tw = text_width("Tahoma 8", title);
	fill(cr, x + 6, y, tw + 6, 14, 0xfcfcfe);
	text(cr, "Tahoma 8", title, x + 9, y, 0, 0x0046d5);
}

static void hint(cairo_t *cr, int x, int y, const char *s) {
	text(cr, "Tahoma 8", s, x, y, 330, 0x5a5a5a);
}

/* horizontal Luna trackbar, value 0..100 */
static void slider(struct win *w, cairo_t *cr, int id, int x, int y, int sw, int value) {
	draw_image(cr, &app.theme.slider_track, 0, x, y + 8, sw, 4);
	for (int i = 0; i <= 10; i++)
		fill(cr, x + 5 + i * (sw - 11) / 10, y + 22, 1, 3, 0x000000);
	int tx = x + value * (sw - 11) / 100;
	draw_image(cr, &app.theme.slider_thumb_h, w->dragging && w->pressed == id ? 2 : state(w, id) == 1 ? 1 : 0, tx, y, 11, 21);
	reg(w, id, x - 4, y - 2, sw + 8, 26);
}

static int slider_value(struct win *w, int id, double px) {
	for (int i = 0; i < w->n_rects; i++)
		if (w->rects[i].id == id) {
			struct rect r = w->rects[i];
			int v = (int)((px - (r.x + 4) - 5) * 100 / (r.w - 8 - 11) + 0.5);
			return v < 0 ? 0 : v > 100 ? 100 : v;
		}
	return 0;
}

/* white list box with the XP field outline; rows are registered as ID_LIST + index */
static void listbox(struct win *w, cairo_t *cr, int x, int y, int lw, int lh, int n, int sel,
		const char *(*label)(int i), cairo_surface_t *(*icon)(int i)) {
	fill(cr, x, y, lw, lh, 0x7f9db9);
	fill(cr, x + 1, y + 1, lw - 2, lh - 2, 0xffffff);
	w->list_box = (struct wlr_box_){x, y, lw, lh};
	int rows = (lh - 4) / 17;
	if (w->scroll > n - rows)
		w->scroll = n - rows;
	if (w->scroll < 0)
		w->scroll = 0;
	cairo_save(cr);
	cairo_rectangle(cr, x + 2, y + 2, lw - 4, lh - 4);
	cairo_clip(cr);
	for (int r = 0; r < rows && w->scroll + r < n; r++) {
		int i = w->scroll + r, ry = y + 2 + r * 17;
		if (i == sel)
			fill(cr, x + 2, ry, lw - 4 - (n > rows ? 6 : 0), 17, 0x316ac5);
		int tx = x + 5;
		if (icon && icon(i)) {
			scaled(cr, icon(i), x + 4, ry + 1, 16);
			tx += 20;
		}
		text(cr, "Tahoma 8", label(i), tx, ry + 2, lw - (tx - x) - 10, i == sel ? 0xffffff : 0x000000);
		reg(w, ID_LIST + i, x + 2, ry, lw - 4, 17);
	}
	cairo_restore(cr);
	if (n > rows) { /* a slim scroll indicator; the wheel scrolls */
		double frac = (double)rows / n, pos = (double)w->scroll / n;
		fill(cr, x + lw - 7, y + 2, 5, lh - 4, 0xeef2fb);
		fill(cr, x + lw - 7, y + 2 + pos * (lh - 4), 5, frac * (lh - 4), 0xa8bce8);
	}
}

static void tabs_and_buttons(struct win *w, cairo_t *cr) {
	const struct applet *a = &applets[w->kind];
	draw_image(cr, &app.theme.tab_pane, 0, 6, 26, SHEET_W - 12, SHEET_H - 72);
	int x = 8;
	for (int i = 0; i < 2 && a->tabs[i]; i++) {
		int tw = text_width("Tahoma 8", a->tabs[i]) + 20;
		bool sel = w->tab == i;
		/* tab frames: normal hot selected disabled focused; the selected one stands 2px taller */
		draw_image(cr, &app.theme.tab_item, sel ? 2 : state(w, ID_TAB0 + i) == 1 ? 1 : 0, x, sel ? 6 : 8, tw, sel ? 22 : 20);
		text(cr, "Tahoma 8", a->tabs[i], x + 10, sel ? 10 : 12, 0, 0x000000);
		reg(w, ID_TAB0 + i, x, 6, tw, 22);
		x += tw;
	}
	bool dirty = memcmp(&cfg, &saved, sizeof(cfg)) != 0;
	button(w, cr, ID_OK, SHEET_W - 249, SHEET_H - 36, 75, "OK", false, true);
	button(w, cr, ID_CANCEL, SHEET_W - 168, SHEET_H - 36, 75, "Cancel", false, false);
	button(w, cr, ID_APPLY, SHEET_W - 87, SHEET_H - 36, 75, "Apply", !dirty, false);
}

/* ------------------------------------------------------------------ Control Panel */

static void paint_cp(struct win *w, cairo_t *cr) {
	fill(cr, 0, 0, w->w, w->h, 0xffffff);
	/* XP's blue task pane */
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, w->h);
	cairo_pattern_add_color_stop_rgb(g, 0, 0x7b / 255.0, 0xa2 / 255.0, 0xe7 / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0x63 / 255.0, 0x75 / 255.0, 0xd6 / 255.0);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, 0, 0, 200, w->h);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	int bx = 12, by = 12, bw = 176;
	g = cairo_pattern_create_linear(bx, 0, bx + bw, 0);
	cairo_pattern_add_color_stop_rgb(g, 0, 1, 1, 1);
	cairo_pattern_add_color_stop_rgb(g, 1, 0xc6 / 255.0, 0xd3 / 255.0, 0xf7 / 255.0);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, bx, by, bw, 24);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	scaled(cr, app.icons[K_CP].s32, bx + 4, by + 2, 20);
	text(cr, "Tahoma Bold 8", "Control Panel", bx + 30, by + 6, 0, 0x215dc6);
	fill(cr, bx, by + 24, bw, 34, 0xd6dff7);
	const char *toggle = app.classic_view ? "Switch to Category View" : "Switch to Classic View";
	bool hot = w->hot == ID_VIEW_TOGGLE;
	text(cr, "Tahoma 8", toggle, bx + 28, by + 34, 0, hot ? 0x428eff : 0x215dc6);
	if (hot)
		fill(cr, bx + 28, by + 47, text_width("Tahoma 8", toggle), 1, 0x428eff);
	scaled(cr, app.icons[K_DISPLAY].s16, bx + 8, by + 32, 16);
	reg(w, ID_VIEW_TOGGLE, bx + 4, by + 28, bw - 8, 24);

	int cx = 220;
	if (!app.classic_view) {
		text(cr, "Trebuchet MS, Bold 14", "Pick a category", cx, 24, 0, 0x4d6185);
		fill(cr, cx, 52, w->w - cx - 20, 1, 0xd6dff7);
		for (int k = K_DISPLAY; k < K_COUNT; k++) {
			int i = k - K_DISPLAY, col = i % 2, row = i / 2;
			int x = cx + col * 215, y = 74 + row * 70;
			bool h = w->hot == ID_CATEGORY + k;
			draw_surface(cr, app.icons[k].s32 ? app.icons[k].s32 : NULL, x, y);
			/* XP wraps long category names onto a second line */
			const char *name = applets[k].category;
			text(cr, "Tahoma 8", name, x + 42, y + (strchr(name, '\n') ? 2 : 8), 0, h ? 0x428eff : 0x215dc6);
			reg(w, ID_CATEGORY + k, x, y, 205, 40);
		}
	} else {
		/* Classic view: every applet as a big icon, like a folder */
		for (int k = K_DISPLAY; k < K_COUNT; k++) {
			int i = k - K_DISPLAY, col = i % 4, row = i / 4;
			int x = cx + col * 105, y = 24 + row * 100;
			bool h = w->hot == ID_CATEGORY + k;
			if (h)
				fill(cr, x, y, 96, 84, 0xe8eefa);
			draw_surface(cr, app.icons[k].s32, x + 32, y + 6);
			draw_label(cr, "Tahoma 8", applets[k].classic, x + 48, y + 44, 92, 0x000000, 0xffffff, -1);
			reg(w, ID_CATEGORY + k, x, y, 96, 84);
		}
	}
}

/* ------------------------------------------------------------------ the applets */

static const char *wallpaper_label(int i) { return wallpapers[i].label; }
static const char *layout_label(int i) { return layouts[i].label; }

static int wallpaper_index(void) {
	for (int i = 0; i < n_wallpapers; i++)
		if (!strcmp(wallpapers[i].value, cfg.wallpaper))
			return i;
	return 0;
}

static void paint_monitor(cairo_t *cr, int x, int y) {
	/* a little beige XP monitor showing the chosen wallpaper */
	fill(cr, x, y, 190, 140, 0x8c8a82);
	fill(cr, x + 1, y + 1, 188, 138, 0xe9e7df);
	fill(cr, x + 14, y + 12, 162, 114, 0x404040);
	int sx = x + 16, sy = y + 14, sw = 158, sh = 110;
	fill(cr, sx, sy, sw, sh, 0x004e98);
	if (strcmp(app.preview_for, cfg.wallpaper)) {
		if (app.preview)
			cairo_surface_destroy(app.preview);
		app.preview = NULL;
		snprintf(app.preview_for, sizeof(app.preview_for), "%s", cfg.wallpaper);
		if (cfg.wallpaper[0]) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/wallpapers/%s", app.theme.dir, cfg.wallpaper);
			app.preview = theme_load_image(path);
		}
	}
	if (app.preview) {
		double iw = cairo_image_surface_get_width(app.preview), ih = cairo_image_surface_get_height(app.preview);
		bool tiled = strstr(cfg.wallpaper, ".tile.") != NULL;
		const char *st = cfg.wall_style;
		int style = !strcmp(st, "center") ? WALL_CENTER : !strcmp(st, "tile") ? WALL_TILE :
			!strcmp(st, "stretch") ? WALL_STRETCH : tiled ? WALL_TILE : WALL_STRETCH;
		double k = sw / 1280.0; /* preview as if on a 1280-wide screen */
		cairo_save(cr);
		cairo_rectangle(cr, sx, sy, sw, sh);
		cairo_clip(cr);
		cairo_translate(cr, sx, sy);
		if (style == WALL_STRETCH) {
			cairo_scale(cr, sw / iw, sh / ih);
			cairo_set_source_surface(cr, app.preview, 0, 0);
		} else {
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, app.preview, style == WALL_CENTER ? (sw / k - iw) / 2 : 0,
				style == WALL_CENTER ? (sh / k - ih) / 2 : 0);
			if (style == WALL_TILE)
				cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
		}
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	fill(cr, x + 80, y + 140, 30, 8, 0xc9c6bb);
	fill(cr, x + 60, y + 148, 70, 6, 0xd9d6cc);
}

static void paint_display(struct win *w, cairo_t *cr) {
	if (w->tab == 0) {
		paint_monitor(cr, (SHEET_W - 190) / 2, 40);
		text(cr, "Tahoma 8", "Background:", 18, 206, 0, 0x000000);
		listbox(w, cr, 18, 222, 240, 150, n_wallpapers, wallpaper_index(), wallpaper_label, NULL);
		text(cr, "Tahoma 8", "Position:", 274, 222, 0, 0x000000);
		bool tiled = strstr(cfg.wallpaper, ".tile.") != NULL;
		const char *st = cfg.wall_style;
		bool auto_ = !strcmp(st, "auto");
		check(w, cr, ID_STRETCH, 274, 240, !strcmp(st, "stretch") || (auto_ && !tiled), true, "Stretch");
		check(w, cr, ID_CENTER, 274, 262, !strcmp(st, "center"), true, "Center");
		check(w, cr, ID_TILE, 274, 284, !strcmp(st, "tile") || (auto_ && tiled), true, "Tile");
	} else {
		group(cr, 16, 40, SHEET_W - 32, 108, "Size of text and items");
		static const int pcts[6] = {0, 100, 125, 150, 175, 200};
		static const char *labels[6] = {"Automatic", "100%", "125%", "150%", "175%", "200%"};
		for (int i = 0; i < 6; i++)
			check(w, cr, ID_SCALE0 + i, 30 + (i % 3) * 115, 64 + (i / 3) * 24, cfg.scale_pct == pcts[i], true, labels[i]);
		hint(cr, 30, 116, "Automatic picks a size from your screen's sharpness,\nlike Windows does.");
		group(cr, 16, 160, SHEET_W - 32, 64, "Windows and buttons");
		text(cr, "Tahoma 8", "Windows XP style (Luna, Default blue)", 30, 184, 0, 0x000000);
		hint(cr, 30, 202, "From your own Windows XP files.");
	}
}

static void paint_taskbar(struct win *w, cairo_t *cr) {
	if (w->tab == 0) {
		group(cr, 16, 40, SHEET_W - 32, 84, "Window behavior");
		check(w, cr, ID_SNAP, 28, 64, cfg.snap, false, "Snap windows to the edges of the screen");
		hint(cr, 47, 86, "Drag a window to the top to maximise it, or to a side\nto fill half the screen. Not part of Windows XP.");
	} else {
		group(cr, 16, 40, SHEET_W - 32, 84, "Start menu style");
		check(w, cr, ID_CLASSIC, 28, 64, true, true, "Start menu (Windows XP)");
		hint(cr, 47, 84, "All Programs opens XP's cascading list of every app.");
	}
}

static void paint_sounds(struct win *w, cairo_t *cr) {
	if (w->tab == 0) {
		draw_surface(cr, app.icons[K_SOUNDS].s32, 24, 44);
		text(cr, "Tahoma 8", app.device, 66, 54, SHEET_W - 90, 0x000000);
		group(cr, 16, 92, SHEET_W - 32, 118, "Device volume");
		if (app.have_volume) {
			text(cr, "Tahoma 8", "Low", 30, 122, 0, 0x000000);
			text(cr, "Tahoma 8", "High", SHEET_W - 60, 122, 0, 0x000000);
			slider(w, cr, ID_VOLUME, 60, 118, SHEET_W - 130, app.volume);
			check(w, cr, ID_MUTE, 30, 168, app.muted, false, "Mute");
		} else {
			hint(cr, 30, 122, "No volume control found (wpctl, pactl or amixer).");
		}
	} else {
		group(cr, 16, 40, SHEET_W - 32, 100, "Sound scheme");
		check(w, cr, ID_SCHEME_DEFAULT, 28, 64, cfg.sounds, true, "Windows Default");
		check(w, cr, ID_SCHEME_NONE, 28, 88, !cfg.sounds, true, "No Sounds");
		button(w, cr, ID_TEST_SOUND, SHEET_W - 112, 62, 80, "Test", false, false);
		hint(cr, 47, 112, "Startup, log on and off, shutdown, minimize and restore.");
	}
}

static void paint_mouse(struct win *w, cairo_t *cr) {
	if (w->tab == 0) {
		group(cr, 16, 40, SHEET_W - 32, 90, "Motion");
		text(cr, "Tahoma 8", "Select a pointer speed:", 30, 62, 0, 0x000000);
		text(cr, "Tahoma 8", "Slow", 30, 92, 0, 0x000000);
		text(cr, "Tahoma 8", "Fast", SHEET_W - 60, 92, 0, 0x000000);
		slider(w, cr, ID_SPEED, 64, 88, SHEET_W - 136, (cfg.speed + 100) / 2);
	} else {
		group(cr, 16, 40, SHEET_W - 32, 132, "Touchpad");
		check(w, cr, ID_TAP, 28, 64, cfg.tap, false, "Tap the touchpad to click");
		check(w, cr, ID_NATURAL, 28, 100, cfg.natural, false, "Natural scrolling");
		hint(cr, 47, 120, "Content follows your fingers, like a phone. Mouse wheels\nalways scroll the usual way.");
	}
}

static int layout_index(void) {
	for (int i = 0; i < N_LAYOUTS; i++)
		if (!strcmp(layouts[i].value, cfg.keyboard))
			return i;
	return 0;
}

static void paint_datetime(struct win *w, cairo_t *cr) {
	if (w->tab == 0) {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		char big[32], date[64];
		strftime(big, sizeof(big), cfg.clock24 ? "%H:%M" : "%-I:%M %p", &tm);
		strftime(date, sizeof(date), "%A, %B %-d, %Y", &tm);
		text(cr, "Trebuchet MS, Bold 20", big, 30, 46, 0, 0x0a246a);
		text(cr, "Tahoma 8", date, 32, 84, 0, 0x000000);
		group(cr, 16, 112, SHEET_W - 32, 80, "Clock in the notification area");
		check(w, cr, ID_CLOCK12, 28, 136, !cfg.clock24, true, "12-hour (3:05 PM)");
		check(w, cr, ID_CLOCK24, 28, 160, cfg.clock24, true, "24-hour (15:05)");
	} else {
		text(cr, "Tahoma 8", "Keyboard layout:", 18, 44, 0, 0x000000);
		listbox(w, cr, 18, 62, SHEET_W - 36, 250, N_LAYOUTS, layout_index(), layout_label, NULL);
		hint(cr, 18, 322, "Takes effect as soon as you click Apply.");
	}
}

static void paint_users(struct win *w, cairo_t *cr) {
	const char *user = getenv("USER") ? getenv("USER") : "User";
	cairo_surface_t *cur = NULL;
	for (int i = 0; i < n_pictures; i++)
		if (!strncmp(pictures[i].value, cfg.picture, strlen(cfg.picture)) && cfg.picture[0] &&
				pictures[i].value[strlen(cfg.picture)] == '.')
			cur = pictures[i].img;
	draw_image(cr, &app.theme.user_tile, 0, 22, 40, 56, 56);
	scaled(cr, cur ? cur : app.theme.user_pic, 26, 44, 48);
	text(cr, "Trebuchet MS, Bold 12", user, 90, 50, 0, 0x0a246a);
	hint(cr, 90, 74, "Pick a new picture for your account.");
	for (int i = 0; i < n_pictures; i++) {
		int col = i % 6, row = i / 6, x = 24 + col * 62, y = 112 + row * 62;
		bool sel = cur == pictures[i].img, h = w->hot == ID_LIST + i;
		if (sel || h)
			fill(cr, x - 3, y - 3, 54, 54, sel ? 0x316ac5 : 0x9bb4e5);
		scaled(cr, pictures[i].img, x, y, 48);
		reg(w, ID_LIST + i, x - 3, y - 3, 54, 54);
	}
}

static void paint(struct win *w, cairo_t *cr) {
	w->n_rects = 0;
	if (w->kind == K_CP) {
		paint_cp(w, cr);
		return;
	}
	fill(cr, 0, 0, w->w, w->h, 0xece9d8); /* XP's dialog face colour */
	tabs_and_buttons(w, cr);
	switch (w->kind) {
	case K_DISPLAY: paint_display(w, cr); break;
	case K_TASKBAR: paint_taskbar(w, cr); break;
	case K_SOUNDS: paint_sounds(w, cr); break;
	case K_MOUSE: paint_mouse(w, cr); break;
	case K_DATETIME: paint_datetime(w, cr); break;
	case K_USERS: paint_users(w, cr); break;
	default: break;
	}
}

/* ------------------------------------------------------------------ actions */

static void redraw_all(void) {
	for (int k = 0; k < K_COUNT; k++)
		if (wins[k].alive)
			redraw(&wins[k]);
}

static void close_window(struct win *w) {
	if (!w->alive)
		return;
	/* the protocol wants the decoration gone before its toplevel */
	if (w->deco)
		zxdg_toplevel_decoration_v1_destroy(w->deco);
	xdg_toplevel_destroy(w->tl);
	xdg_surface_destroy(w->xs);
	wl_surface_destroy(w->surface);
	w->alive = w->configured = false;
	if (app.focus == w)
		app.focus = NULL;
}

static void activate(struct win *w, int id) {
	if (id >= ID_CATEGORY && id < ID_CATEGORY + K_COUNT) {
		open_window(id - ID_CATEGORY);
		return;
	}
	if (id >= ID_LIST) {
		int i = id - ID_LIST;
		if (w->kind == K_DISPLAY && i < n_wallpapers)
			snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", wallpapers[i].value);
		else if (w->kind == K_DATETIME && i < N_LAYOUTS)
			snprintf(cfg.keyboard, sizeof(cfg.keyboard), "%s", layouts[i].value);
		else if (w->kind == K_USERS && i < n_pictures) {
			snprintf(cfg.picture, sizeof(cfg.picture), "%s", pictures[i].value);
			*strrchr(cfg.picture, '.') = 0;
		}
		redraw_all();
		return;
	}
	if (id >= ID_SCALE0 && id < ID_SCALE0 + 6) {
		static const int pcts[6] = {0, 100, 125, 150, 175, 200};
		cfg.scale_pct = pcts[id - ID_SCALE0];
		redraw_all();
		return;
	}
	switch (id) {
	case ID_VIEW_TOGGLE: app.classic_view = !app.classic_view; break;
	case ID_TAB0: w->tab = 0; w->scroll = 0; break;
	case ID_TAB1: w->tab = 1; w->scroll = 0; break;
	case ID_SNAP: cfg.snap = !cfg.snap; break;
	case ID_TAP: cfg.tap = !cfg.tap; break;
	case ID_NATURAL: cfg.natural = !cfg.natural; break;
	case ID_SCHEME_DEFAULT: cfg.sounds = true; break;
	case ID_SCHEME_NONE: cfg.sounds = false; break;
	case ID_CLOCK12: cfg.clock24 = false; break;
	case ID_CLOCK24: cfg.clock24 = true; break;
	case ID_STRETCH: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "stretch"); break;
	case ID_CENTER: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "center"); break;
	case ID_TILE: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "tile"); break;
	case ID_MUTE:
		app.muted = !app.muted;
		volume_mute(app.muted);
		break;
	case ID_TEST_SOUND: {
		char cmd[1200];
		snprintf(cmd, sizeof(cmd), "pw-play '%1$s/sounds/ding.wav' 2>/dev/null || paplay '%1$s/sounds/ding.wav' 2>/dev/null || aplay -q '%1$s/sounds/ding.wav'", app.theme.dir);
		spawn(cmd);
		break;
	}
	case ID_APPLY:
		config_save();
		break;
	case ID_OK:
		if (memcmp(&cfg, &saved, sizeof(cfg)))
			config_save();
		close_window(w);
		break;
	case ID_CANCEL:
		cfg = saved;
		close_window(w);
		break;
	}
	redraw_all();
}

static void drag(struct win *w, double x) {
	if (w->pressed == ID_VOLUME) {
		int v = slider_value(w, ID_VOLUME, x);
		if (v != app.volume) {
			app.volume = v;
			volume_write(v);
		}
	} else if (w->pressed == ID_SPEED) {
		cfg.speed = slider_value(w, ID_SPEED, x) * 2 - 100;
	}
	redraw_all();
}

/* ------------------------------------------------------------------ wl_shm buffers */

static void buffer_release(void *data, struct wl_buffer *b) {
	wl_buffer_destroy(b);
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void redraw(struct win *w) {
	if (!w->alive || !w->configured)
		return;
	int k = w->scale > 0 ? w->scale : 1;
	ui_scale = k;
	int bw = w->w * k, bh = w->h * k, stride = bw * 4, size = stride * bh;
	int fd = memfd_create("expede-settings", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0)
		return;
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	cairo_surface_t *s = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, bw, bh, stride);
	cairo_surface_set_device_scale(s, k, k);
	cairo_t *cr = cairo_create(s);
	paint(w, cr);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	munmap(data, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, bw, bh, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b, &buffer_listener, NULL);
	wl_surface_attach(w->surface, b, 0, 0);
	wl_surface_set_buffer_scale(w->surface, k);
	wl_surface_damage_buffer(w->surface, 0, 0, bw, bh);
	wl_surface_commit(w->surface);
}

/* ------------------------------------------------------------------ input */

static int hit(struct win *w, double x, double y) {
	for (int i = w->n_rects - 1; i >= 0; i--) {
		struct rect r = w->rects[i];
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
			return r.id;
	}
	return ID_NONE;
}

static struct win *win_for(struct wl_surface *s) {
	for (int k = 0; k < K_COUNT; k++)
		if (wins[k].alive && wins[k].surface == s)
			return &wins[k];
	return NULL;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	app.focus = win_for(s);
	app.px = wl_fixed_to_double(x);
	app.py = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	struct win *w = win_for(s);
	if (w) {
		w->hot = ID_NONE;
		redraw(w);
	}
	app.focus = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	app.px = wl_fixed_to_double(x);
	app.py = wl_fixed_to_double(y);
	struct win *w = app.focus;
	if (!w)
		return;
	if (w->dragging) {
		drag(w, app.px);
		return;
	}
	int h = hit(w, app.px, app.py);
	if (h != w->hot) {
		w->hot = h;
		redraw(w);
	}
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t st) {
	struct win *w = app.focus;
	if (!w || button != BTN_LEFT)
		return;
	int id = hit(w, app.px, app.py);
	if (st == WL_POINTER_BUTTON_STATE_PRESSED) {
		w->pressed = id;
		if (id == ID_VOLUME || id == ID_SPEED) {
			w->dragging = true;
			drag(w, app.px);
		}
		redraw(w);
		return;
	}
	int pressed = w->pressed;
	w->pressed = ID_NONE;
	if (w->dragging) {
		w->dragging = false;
		redraw(w);
		return;
	}
	if (id == pressed && id != ID_NONE)
		activate(w, id);
	else
		redraw(w);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
	struct win *w = app.focus;
	if (!w || axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	struct wlr_box_ b = w->list_box;
	if (app.px >= b.x && app.px < b.x + b.w && app.py >= b.y && app.py < b.y + b.h) {
		w->scroll += wl_fixed_to_double(v) > 0 ? 3 : -3;
		w->hot = ID_NONE;
		redraw(w);
	}
}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
	.frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.pointer) {
		app.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(app.pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

/* ------------------------------------------------------------------ surfaces */

static void s_enter(void *d, struct wl_surface *s, struct wl_output *o) {}
static void s_leave(void *d, struct wl_surface *s, struct wl_output *o) {}
static void s_preferred_scale(void *d, struct wl_surface *s, int32_t factor) {
	struct win *w = d;
	if (factor != w->scale) {
		w->scale = factor;
		redraw(w);
	}
}
static void s_preferred_transform(void *d, struct wl_surface *s, uint32_t t) {}
static const struct wl_surface_listener surface_listener = {
	.enter = s_enter, .leave = s_leave,
	.preferred_buffer_scale = s_preferred_scale, .preferred_buffer_transform = s_preferred_transform,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_ping};

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) {
	struct win *w = d;
	xdg_surface_ack_configure(s, serial);
	w->configured = true;
	redraw(w);
}
static const struct xdg_surface_listener xdg_surface_listener = {.configure = xs_configure};

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {}
static void tl_close(void *d, struct xdg_toplevel *t) {
	struct win *w = d;
	if (w->kind != K_CP)
		cfg = saved; /* closing a sheet with its X is Cancel */
	close_window(w);
}
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *caps) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = tl_configure, .close = tl_close, .configure_bounds = tl_bounds, .wm_capabilities = tl_caps,
};

static void open_window(enum kind k) {
	struct win *w = &wins[k];
	if (w->alive)
		return; /* XP just brings it forward; here it is already open */
	*w = (struct win){.kind = k, .alive = true, .scale = 1, .hot = ID_NONE, .pressed = ID_NONE};
	w->w = k == K_CP ? CP_W : SHEET_W;
	w->h = k == K_CP ? CP_H : SHEET_H;
	if (k == K_SOUNDS) {
		app.have_volume = volume_available() && volume_read(&app.volume, &app.muted);
		volume_device_name(app.device, sizeof(app.device));
	}
	w->surface = wl_compositor_create_surface(app.compositor);
	wl_surface_add_listener(w->surface, &surface_listener, w);
	w->xs = xdg_wm_base_get_xdg_surface(app.wm_base, w->surface);
	xdg_surface_add_listener(w->xs, &xdg_surface_listener, w);
	w->tl = xdg_surface_get_toplevel(w->xs);
	xdg_toplevel_add_listener(w->tl, &toplevel_listener, w);
	xdg_toplevel_set_title(w->tl, applets[k].title);
	xdg_toplevel_set_app_id(w->tl, "expede-settings");
	if (k != K_CP) {
		xdg_toplevel_set_min_size(w->tl, w->w, w->h);
		xdg_toplevel_set_max_size(w->tl, w->w, w->h);
	}
	if (app.deco_mgr) {
		w->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(app.deco_mgr, w->tl);
		zxdg_toplevel_decoration_v1_set_mode(w->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(w->surface);
}

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
	if (!strcmp(iface, wl_compositor_interface.name))
		app.compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6);
	else if (!strcmp(iface, wl_shm_interface.name))
		app.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) {
		app.seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(app.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		app.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(app.wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
		app.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

static const char *assets_dir(void) {
	static char buf[512];
	const char *env = getenv("EXPEDE_ASSETS");
	if (env)
		return env;
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg && *xdg)
		snprintf(buf, sizeof(buf), "%s/expede/xp", xdg);
	else
		snprintf(buf, sizeof(buf), "%s/.local/share/expede/xp", getenv("HOME"));
	return buf;
}

static bool any_open(void) {
	for (int k = 0; k < K_COUNT; k++)
		if (wins[k].alive)
			return true;
	return false;
}

int main(int argc, char **argv) {
	if (!theme_load(&app.theme, assets_dir()))
		return 1;
	config_load();
	theme_set_user_picture(&app.theme, cfg.picture);

	char dir[700];
	wallpapers = calloc(1, sizeof(*wallpapers));
	wallpapers[0] = (struct choice){strdup(""), strdup("(None)"), NULL};
	n_wallpapers = 1;
	struct choice *found = NULL;
	int n_found = 0;
	snprintf(dir, sizeof(dir), "%s/wallpapers", app.theme.dir);
	list_dir(dir, &found, &n_found, true);
	wallpapers = realloc(wallpapers, (1 + n_found) * sizeof(*wallpapers));
	memcpy(wallpapers + 1, found, n_found * sizeof(*found));
	n_wallpapers += n_found;
	free(found);
	snprintf(dir, sizeof(dir), "%s/pictures", app.theme.dir);
	list_dir(dir, &pictures, &n_pictures, false);
	for (int i = 0; i < n_pictures; i++) {
		char rel[300];
		snprintf(rel, sizeof(rel), "pictures/%s", pictures[i].value);
		pictures[i].img = theme_png(&app.theme, rel);
	}
	for (int k = 0; k < K_COUNT; k++)
		app.icons[k] = theme_icon(&app.theme, applets[k].icon);

	app.display = wl_display_connect(NULL);
	if (!app.display) {
		fprintf(stderr, "expede-settings: no Wayland display\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(app.display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(app.display);
	if (!app.compositor || !app.shm || !app.wm_base) {
		fprintf(stderr, "expede-settings: compositor lacks xdg-shell\n");
		return 1;
	}

	enum kind start = K_CP;
	for (int k = 1; argc > 1 && k < K_COUNT; k++)
		if (!strcmp(argv[1], applets[k].arg))
			start = k;
	open_window(start);
	while (any_open() && wl_display_dispatch(app.display) != -1)
		;
	wl_display_disconnect(app.display);
	return 0;
}
