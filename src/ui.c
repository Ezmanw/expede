/* Expede's app toolkit: see ui.h. One Wayland connection, any number of
 * windows, each repainted whole into a wl_shm buffer at the screen's scale. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "ui.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"

struct theme ui_theme;

struct ui_priv {
	struct wl_surface *surface;
	struct xdg_surface *xs;
	struct xdg_toplevel *tl;
	struct zxdg_toplevel_decoration_v1 *deco;
	int pending_w, pending_h;
};

#define MAX_WINDOWS 32

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct xdg_wm_base *wm_base;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct ui_win *windows[MAX_WINDOWS];
	struct ui_win *pointer_focus, *key_focus;
	double px, py;
	bool held, quit;
	/* double clicks */
	uint32_t last_press_ms;
	int last_press_id;
	struct ui_win *last_press_win;
	/* keyboard */
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	int32_t repeat_rate, repeat_delay;
	uint32_t repeat_key;
	uint64_t repeat_next_ms;
	bool repeating;
} u;

static uint64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void spawn(const char *cmd) {
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
}

const char *ui_shell_quote(const char *s, char *out, size_t cap) {
	size_t o = 0;
	if (o + 1 < cap) out[o++] = '\'';
	for (; *s && o + 5 < cap; s++) {
		if (*s == '\'') {
			memcpy(out + o, "'\\''", 4);
			o += 4;
		} else {
			out[o++] = *s;
		}
	}
	if (o + 1 < cap) out[o++] = '\'';
	out[o] = 0;
	return out;
}

/* "xp", "7" or "10": the Windows version the user picked */
static const char *want_style(void) {
	char path[512], line[512], key[64], val[256];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/expede/expede.conf", xdg);
	else
		snprintf(path, sizeof(path), "%s/.config/expede/expede.conf", getenv("HOME"));
	FILE *f = fopen(path, "r");
	const char *st = "xp";
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq || sscanf(line, " %63[^= ]", key) != 1 || strcmp(key, "style"))
			continue;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		size_t n = strcspn(v, "\r\n");
		while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
			n--;
		snprintf(val, sizeof(val), "%.*s", (int)n, v);
		if (!strcasecmp(val, "7") || !strcasecmp(val, "win7") || !strcasecmp(val, "windows7"))
			st = "7";
		else if (!strcasecmp(val, "10") || !strcasecmp(val, "win10") || !strcasecmp(val, "windows10"))
			st = "10";
		else
			st = "xp";
	}
	if (f)
		fclose(f);
	return st;
}

const char *ui_assets_dir(void) {
	static char buf[512];
	const char *env = getenv("EXPEDE_ASSETS");
	if (env)
		return env;
	char base[480], ini[540];
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg && *xdg)
		snprintf(base, sizeof(base), "%s/expede", xdg);
	else
		snprintf(base, sizeof(base), "%s/.local/share/expede", getenv("HOME"));
	/* the chosen version's rip, else whichever one there is */
	const char *order[4] = {want_style(), "xp", "7", "10"};
	for (int i = 0; i < 4; i++) {
		snprintf(ini, sizeof(ini), "%s/%s/luna/normalblue.ini", base, order[i]);
		if (access(ini, R_OK) == 0) {
			snprintf(buf, sizeof(buf), "%s/%s", base, order[i]);
			return buf;
		}
	}
	snprintf(buf, sizeof(buf), "%s/xp", base);
	return buf;
}

/* ------------------------------------------------------------------ drawing */

/* Windows 10 Settings draws the classic pages inside its own layout */
static int org_x, org_y;
bool ui_w10_look;
uint32_t ui_accent = 0x0078d7;

void ui_set_origin(int x, int y) {
	org_x = x;
	org_y = y;
}

void ui_reg(struct ui_win *w, int id, int x, int y, int ww, int hh) {
	if (w->n_rects < (int)(sizeof(w->rects) / sizeof(*w->rects)))
		w->rects[w->n_rects++] = (struct ui_rect){id, x + org_x, y + org_y, ww, hh};
}

int ui_state(struct ui_win *w, int id) {
	return w->pressed == id && w->hot == id ? 2 : w->hot == id ? 1 : 0;
}

void ui_fill(cairo_t *cr, double x, double y, double w, double h, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

void ui_text(cairo_t *cr, const char *font, const char *s, double x, double y, double maxw, uint32_t c) {
	draw_text(cr, font, s, x, y, maxw, c, 0, 0, 0, true);
}

void ui_icon(cairo_t *cr, cairo_surface_t *s, double x, double y, double size) {
	if (!s)
		return;
	double k = size / cairo_image_surface_get_width(s);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), k == 1 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

void ui_button(struct ui_win *w, cairo_t *cr, int id, int x, int y, int bw, const char *label, bool disabled, bool def) {
	/* push button frames: normal hot pressed disabled default */
	int f = disabled ? 3 : ui_state(w, id) ? ui_state(w, id) : def ? 4 : 0;
	draw_image(cr, &ui_theme.push_button, f, x, y, bw, 23);
	int tw = text_width("Tahoma 8", label);
	ui_text(cr, "Tahoma 8", label, x + (bw - tw) / 2, y + (ui_theme.version >= 7 ? 4 : 5), 0,
		disabled ? (ui_theme.version >= 7 ? 0x838383 : 0xaca899) : 0x000000);
	if (!disabled)
		ui_reg(w, id, x, y, bw, 23);
}

/* Windows 10's on/off switch, with its On / Off and the setting's name beside it */
static void w10_toggle(struct ui_win *w, cairo_t *cr, int id, int x, int y, bool on, const char *label) {
	double tx = x + 0.5, ty = y - 1.5, tw = 40, th = 20, r = th / 2;
	bool hot = ui_state(w, id) > 0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, tx + r, ty + r, r, M_PI / 2, 3 * M_PI / 2);
	cairo_arc(cr, tx + tw - r, ty + r, r, -M_PI / 2, M_PI / 2);
	cairo_close_path(cr);
	if (on) {
		cairo_set_source_rgb(cr, ((ui_accent >> 16) & 255) / 255.0, ((ui_accent >> 8) & 255) / 255.0,
			(ui_accent & 255) / 255.0);
		cairo_fill(cr);
	} else {
		cairo_set_source_rgb(cr, 1, 1, 1);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 2);
		cairo_set_source_rgb(cr, hot ? 0 : 0.2, hot ? 0 : 0.2, hot ? 0 : 0.2);
		cairo_stroke(cr);
	}
	cairo_arc(cr, on ? tx + tw - r : tx + r, ty + r, 5, 0, 2 * M_PI);
	cairo_set_source_rgb(cr, on ? 1 : 0.2, on ? 1 : 0.2, on ? 1 : 0.2);
	cairo_fill(cr);
	ui_text(cr, "Segoe UI 10", on ? "On" : "Off", x + 50, y, 0, 0x000000);
	ui_text(cr, "Segoe UI 10", label, x + 84, y, 0, 0x000000);
	ui_reg(w, id, x, y - 2, 84 + text_width("Segoe UI 10", label) + 4, 22);
}

void ui_check(struct ui_win *w, cairo_t *cr, int id, int x, int y, bool on, bool radio, const char *label) {
	if (ui_w10_look && !radio) {
		w10_toggle(w, cr, id, x, y, on, label);
		return;
	}
	/* checkbox: unchecked n/h/p/d, checked n/h/p/d...; radio: unchecked x4, checked x4 */
	draw_frame_true(cr, radio ? ui_theme.radio : ui_theme.checkbox, radio ? 8 : ui_theme.version >= 7 ? 20 : 12,
		(on ? 4 : 0) + ui_state(w, id), x, y + 1);
	ui_text(cr, "Tahoma 8", label, x + 19, y + (ui_theme.version >= 7 ? 0 : 1), 0, 0x000000);
	ui_reg(w, id, x, y, 19 + text_width("Tahoma 8", label) + 4, 16);
}

void ui_group(cairo_t *cr, int x, int y, int gw, int gh, const char *title) {
	if (ui_w10_look) {
		/* Windows 10 Settings: a section heading, no box */
		ui_text(cr, "Segoe UI Semibold 13", title, x, y - 4, gw, 0x000000);
		return;
	}
	draw_image(cr, &ui_theme.groupbox, 0, x, y + 6, gw, gh - 6);
	int tw = text_width("Tahoma 8", title);
	ui_fill(cr, x + 6, y, tw + 6, 14, ui_theme.version >= 7 ? 0xffffff : 0xfcfcfe);
	ui_text(cr, "Tahoma 8", title, x + 9, y, 0, ui_theme.version >= 7 ? 0x000000 : 0x0046d5);
}

void ui_field(struct ui_win *w, cairo_t *cr, int id, int x, int y, int fw, int fh, const char *text, bool focused, int caret) {
	if (ui_theme.version >= 7 && ui_theme.w7_edit.s) {
		ui_fill(cr, x + 1, y + 1, fw - 2, fh - 2, 0xffffff);
		draw_image(cr, &ui_theme.w7_edit, focused ? 2 : ui_state(w, id) ? 1 : 0, x, y, fw, fh);
	} else {
		ui_fill(cr, x, y, fw, fh, 0x7f9db9);
		ui_fill(cr, x + 1, y + 1, fw - 2, fh - 2, 0xffffff);
	}
	int ty = y + (fh - (ui_theme.version >= 7 ? 15 : 13)) / 2;
	cairo_save(cr);
	cairo_rectangle(cr, x + 2, y + 1, fw - 4, fh - 2);
	cairo_clip(cr);
	ui_text(cr, "Tahoma 8", text, x + 4, ty, 0, 0x000000);
	if (focused) {
		char before[1024];
		snprintf(before, sizeof(before), "%.*s", caret, text);
		int cx = x + 4 + (caret ? text_width("Tahoma 8", before) : 0);
		ui_fill(cr, cx, ty, 1, ui_theme.version >= 7 ? 15 : 13, 0x000000);
	}
	cairo_restore(cr);
	ui_reg(w, id, x, y, fw, fh);
}

bool ui_edit(char *text, size_t cap, int *caret, xkb_keysym_t sym, const char *utf8, int mods) {
	int len = strlen(text);
	if (*caret > len)
		*caret = len;
	switch (sym) {
	case XKB_KEY_BackSpace:
		if (*caret > 0) {
			int p = *caret - 1;
			while (p > 0 && (text[p] & 0xc0) == 0x80)
				p--;
			memmove(text + p, text + *caret, len - *caret + 1);
			*caret = p;
		}
		return true;
	case XKB_KEY_Delete:
		if (*caret < len) {
			int q = *caret + 1;
			while (q < len && (text[q] & 0xc0) == 0x80)
				q++;
			memmove(text + *caret, text + q, len - q + 1);
		}
		return true;
	case XKB_KEY_Left:
		while (*caret > 0 && (text[--*caret] & 0xc0) == 0x80)
			;
		return true;
	case XKB_KEY_Right:
		if (*caret < len)
			while (++*caret < len && (text[*caret] & 0xc0) == 0x80)
				;
		return true;
	case XKB_KEY_Home: *caret = 0; return true;
	case XKB_KEY_End: *caret = len; return true;
	}
	if (!(mods & (UI_CTRL | UI_ALT)) && utf8 && (unsigned char)utf8[0] >= 0x20 && utf8[0] != 0x7f) {
		int n = strlen(utf8);
		if (len + n + 1 < (int)cap) {
			memmove(text + *caret + n, text + *caret, len - *caret + 1);
			memcpy(text + *caret, utf8, n);
			*caret += n;
		}
		return true;
	}
	return false;
}

int ui_menu_height(const struct ui_menu_item *items, int n) {
	int h = 6;
	for (int i = 0; i < n; i++)
		h += items[i].label ? 22 : 9;
	return h;
}

void ui_menu(struct ui_win *w, cairo_t *cr, int base_id, int x, int y, const struct ui_menu_item *items, int n) {
	int mw = 180, mh = ui_menu_height(items, n);
	bool w7 = ui_theme.version >= 7;
	for (int i = 0; i < n; i++)
		if (items[i].label) {
			int tw = text_width(items[i].bold ? "Tahoma Bold 8" : "Tahoma 8", items[i].label) + 48;
			if (tw > mw)
				mw = tw;
		}
	if (x + mw > w->w)
		x = w->w - mw - 2;
	if (y + mh > w->h)
		y = w->h - mh - 2;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
	cairo_rectangle(cr, x + 3, y + 3, mw, mh);
	cairo_fill(cr);
	ui_fill(cr, x, y, mw, mh, w7 ? 0x979797 : 0xaca899);
	ui_fill(cr, x + 1, y + 1, mw - 2, mh - 2, w7 ? 0xf0f0f0 : 0xffffff);
	if (w7) {
		ui_fill(cr, x + 26, y + 2, 1, mh - 4, 0xe2e3e3);
		ui_fill(cr, x + 27, y + 2, 1, mh - 4, 0xffffff);
	}
	ui_reg(w, base_id - 1, x, y, mw, mh); /* the menu body itself swallows clicks */
	int iy = y + 3;
	for (int i = 0; i < n; i++) {
		if (!items[i].label) {
			if (w7) {
				ui_fill(cr, x + 28, iy + 4, mw - 30, 1, 0xe0e0e0);
				ui_fill(cr, x + 28, iy + 5, mw - 30, 1, 0xffffff);
			} else {
				ui_fill(cr, x + 2, iy + 4, mw - 4, 1, 0xaca899);
			}
			iy += 9;
			continue;
		}
		bool hot = w->hot == base_id + i && !items[i].disabled;
		if (hot) {
			if (w7 && ui_theme.w7_menu_item.s)
				draw_image(cr, &ui_theme.w7_menu_item, 1, x + 3, iy, mw - 6, 22);
			else
				ui_fill(cr, x + 3, iy, mw - 6, 22, 0x316ac5);
		}
		ui_text(cr, items[i].bold ? "Tahoma Bold 8" : "Tahoma 8", items[i].label, x + 32, iy + 3, mw - 40,
			items[i].disabled ? (w7 ? 0x838383 : 0xaca899) : (hot && !w7) ? 0xffffff : 0x000000);
		if (!items[i].disabled)
			ui_reg(w, base_id + i, x + 3, iy, mw - 6, 22);
		iy += 22;
	}
}

/* ------------------------------------------------------------------ buffers */

static void buffer_release(void *data, struct wl_buffer *b) {
	wl_buffer_destroy(b);
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

void ui_redraw(struct ui_win *w) {
	if (!w->alive || !w->configured || !w->paint)
		return;
	int k = w->scale > 0 ? w->scale : 1;
	ui_scale = k;
	int bw = w->w * k, bh = w->h * k, stride = bw * 4, size = stride * bh;
	int fd = memfd_create("expede-ui", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		if (fd >= 0)
			close(fd);
		return;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return;
	}
	cairo_surface_t *s = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, bw, bh, stride);
	cairo_surface_set_device_scale(s, k, k);
	cairo_t *cr = cairo_create(s);
	w->n_rects = 0;
	w->paint(w, cr);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	munmap(data, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(u.shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, bw, bh, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b, &buffer_listener, NULL);
	wl_surface_attach(w->priv->surface, b, 0, 0);
	wl_surface_set_buffer_scale(w->priv->surface, k);
	wl_surface_damage_buffer(w->priv->surface, 0, 0, bw, bh);
	wl_surface_commit(w->priv->surface);
}

/* ------------------------------------------------------------------ input */

static int hit(struct ui_win *w, double x, double y) {
	for (int i = w->n_rects - 1; i >= 0; i--) {
		struct ui_rect r = w->rects[i];
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
			return r.id;
	}
	return 0;
}

static struct ui_win *win_for(struct wl_surface *s) {
	for (int i = 0; i < MAX_WINDOWS; i++)
		if (u.windows[i] && u.windows[i]->priv->surface == s)
			return u.windows[i];
	return NULL;
}

static int mods(void) {
	int m = 0;
	if (!u.xkb_state)
		return 0;
	if (xkb_state_mod_name_is_active(u.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) m |= UI_SHIFT;
	if (xkb_state_mod_name_is_active(u.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) m |= UI_CTRL;
	if (xkb_state_mod_name_is_active(u.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) m |= UI_ALT;
	return m;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	u.pointer_focus = win_for(s);
	u.px = wl_fixed_to_double(x);
	u.py = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	struct ui_win *w = win_for(s);
	if (w && !u.held) {
		w->hot = 0;
		ui_redraw(w);
	}
	u.pointer_focus = NULL;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	u.px = wl_fixed_to_double(x);
	u.py = wl_fixed_to_double(y);
	struct ui_win *w = u.pointer_focus;
	if (!w)
		return;
	if (w->motion)
		w->motion(w, u.px, u.py, u.held);
	if (!w->alive)
		return;
	int h = hit(w, u.px, u.py);
	if (h != w->hot) {
		w->hot = h;
		ui_redraw(w);
	}
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t st) {
	struct ui_win *w = u.pointer_focus;
	if (!w)
		return;
	int b = button == BTN_LEFT ? UI_LEFT : button == BTN_RIGHT ? UI_RIGHT : button == BTN_MIDDLE ? UI_MIDDLE : 0;
	if (!b)
		return;
	bool pressed = st == WL_POINTER_BUTTON_STATE_PRESSED;
	int id = hit(w, u.px, u.py);
	int clicks = 1;
	if (pressed) {
		u.key_focus = w;
		uint64_t t = now_ms();
		if (b == UI_LEFT && w == u.last_press_win && id == u.last_press_id && t - u.last_press_ms < 450) {
			clicks = 2;
			u.last_press_ms = 0;
		} else {
			u.last_press_ms = t;
		}
		u.last_press_id = id;
		u.last_press_win = w;
		if (b == UI_LEFT) {
			w->pressed = id;
			u.held = true;
		}
	} else if (b == UI_LEFT) {
		u.held = false;
	}
	if (w->button)
		w->button(w, id, b, pressed, u.px, u.py, clicks, mods());
	if (!w->alive)
		return;
	if (!pressed && b == UI_LEFT)
		w->pressed = 0;
	w->hot = hit(w, u.px, u.py);
	ui_redraw(w);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
	struct ui_win *w = u.pointer_focus;
	if (w && w->scroll && axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		w->scroll(w, wl_fixed_to_double(v), u.px, u.py);
}
static void p_frame(void *d, struct wl_pointer *p) {}
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
	.frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	if (u.xkb_state) xkb_state_unref(u.xkb_state);
	if (u.keymap) xkb_keymap_unref(u.keymap);
	u.keymap = xkb_keymap_new_from_string(u.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	u.xkb_state = u.keymap ? xkb_state_new(u.keymap) : NULL;
}
static void k_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
	u.key_focus = win_for(s);
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
	u.repeating = false;
}
static void deliver_key(uint32_t key) {
	struct ui_win *w = u.key_focus;
	if (!w || !w->key || !u.xkb_state)
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(u.xkb_state, key + 8);
	char utf8[16] = "";
	xkb_state_key_get_utf8(u.xkb_state, key + 8, utf8, sizeof(utf8));
	w->key(w, sym, utf8, mods());
	if (w->alive)
		ui_redraw(w);
}
static void k_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		deliver_key(key);
		if (u.keymap && xkb_keymap_key_repeats(u.keymap, key + 8) && u.repeat_rate > 0) {
			u.repeating = true;
			u.repeat_key = key;
			u.repeat_next_ms = now_ms() + u.repeat_delay;
		}
	} else if (u.repeat_key == key) {
		u.repeating = false;
	}
}
static void k_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t group) {
	if (u.xkb_state)
		xkb_state_update_mask(u.xkb_state, dep, lat, lock, 0, 0, group);
}
static void k_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
	u.repeat_rate = rate;
	u.repeat_delay = delay;
}
static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = k_keymap, .enter = k_enter, .leave = k_leave, .key = k_key, .modifiers = k_modifiers,
	.repeat_info = k_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !u.pointer) {
		u.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(u.pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !u.keyboard) {
		u.keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(u.keyboard, &keyboard_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

/* ------------------------------------------------------------------ windows */

static void s_enter(void *d, struct wl_surface *s, struct wl_output *o) {}
static void s_leave(void *d, struct wl_surface *s, struct wl_output *o) {}
static void s_scale(void *d, struct wl_surface *s, int32_t factor) {
	struct ui_win *w = d;
	if (factor != w->scale) {
		w->scale = factor;
		ui_redraw(w);
	}
}
static void s_transform(void *d, struct wl_surface *s, uint32_t t) {}
static const struct wl_surface_listener surface_listener = {
	.enter = s_enter, .leave = s_leave, .preferred_buffer_scale = s_scale, .preferred_buffer_transform = s_transform,
};

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_ping};

static void xs_configure(void *d, struct xdg_surface *s, uint32_t serial) {
	struct ui_win *w = d;
	xdg_surface_ack_configure(s, serial);
	if (w->resizable && w->priv->pending_w > 0 && w->priv->pending_h > 0) {
		w->w = w->priv->pending_w;
		w->h = w->priv->pending_h;
	}
	w->configured = true;
	ui_redraw(w);
}
static const struct xdg_surface_listener xdg_surface_listener = {.configure = xs_configure};

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
	struct ui_win *win = d;
	win->priv->pending_w = w;
	win->priv->pending_h = h;
}
static void tl_close(void *d, struct xdg_toplevel *t) {
	struct ui_win *w = d;
	if (w->close)
		w->close(w);
	else
		ui_destroy(w);
}
static void tl_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tl_caps(void *d, struct xdg_toplevel *t, struct wl_array *caps) {}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = tl_configure, .close = tl_close, .configure_bounds = tl_bounds, .wm_capabilities = tl_caps,
};

struct ui_win *ui_window(const char *title, const char *app_id, int w, int h, bool resizable) {
	int slot = -1;
	for (int i = 0; i < MAX_WINDOWS; i++)
		if (!u.windows[i]) {
			slot = i;
			break;
		}
	if (slot < 0)
		return NULL;
	struct ui_win *win = calloc(1, sizeof(*win));
	win->priv = calloc(1, sizeof(*win->priv));
	win->w = w;
	win->h = h;
	win->scale = 1;
	win->alive = true;
	win->resizable = resizable;
	struct ui_priv *p = win->priv;
	p->surface = wl_compositor_create_surface(u.compositor);
	wl_surface_add_listener(p->surface, &surface_listener, win);
	p->xs = xdg_wm_base_get_xdg_surface(u.wm_base, p->surface);
	xdg_surface_add_listener(p->xs, &xdg_surface_listener, win);
	p->tl = xdg_surface_get_toplevel(p->xs);
	xdg_toplevel_add_listener(p->tl, &toplevel_listener, win);
	xdg_toplevel_set_title(p->tl, title);
	xdg_toplevel_set_app_id(p->tl, app_id);
	if (!resizable) {
		xdg_toplevel_set_min_size(p->tl, w, h);
		xdg_toplevel_set_max_size(p->tl, w, h);
	} else {
		xdg_toplevel_set_min_size(p->tl, 360, 240);
	}
	if (u.deco_mgr) {
		p->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(u.deco_mgr, p->tl);
		zxdg_toplevel_decoration_v1_set_mode(p->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(p->surface);
	u.windows[slot] = win;
	return win;
}

void ui_set_title(struct ui_win *w, const char *title) {
	xdg_toplevel_set_title(w->priv->tl, title);
}

void ui_destroy(struct ui_win *w) {
	if (!w->alive)
		return;
	w->alive = false;
	struct ui_priv *p = w->priv;
	/* the protocol wants the decoration gone before its toplevel */
	if (p->deco)
		zxdg_toplevel_decoration_v1_destroy(p->deco);
	xdg_toplevel_destroy(p->tl);
	xdg_surface_destroy(p->xs);
	wl_surface_destroy(p->surface);
	for (int i = 0; i < MAX_WINDOWS; i++)
		if (u.windows[i] == w)
			u.windows[i] = NULL;
	if (u.pointer_focus == w) u.pointer_focus = NULL;
	if (u.key_focus == w) u.key_focus = NULL;
	if (u.last_press_win == w) u.last_press_win = NULL;
	/* the struct stays allocated: callers may still hold it this turn */
}

/* ------------------------------------------------------------------ setup and loop */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
	if (!strcmp(iface, wl_compositor_interface.name))
		u.compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 6 ? version : 6);
	else if (!strcmp(iface, wl_shm_interface.name))
		u.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name)) {
		u.seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
		wl_seat_add_listener(u.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		u.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(u.wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name))
		u.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

bool ui_init(const char *who) {
	if (!theme_load(&ui_theme, ui_assets_dir()))
		return false;
	theme_use_fonts(&ui_theme);
	u.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	u.display = wl_display_connect(NULL);
	if (!u.display) {
		fprintf(stderr, "%s: no Wayland display\n", who);
		return false;
	}
	struct wl_registry *reg = wl_display_get_registry(u.display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(u.display);
	if (!u.compositor || !u.shm || !u.wm_base) {
		fprintf(stderr, "%s: the compositor lacks xdg-shell\n", who);
		return false;
	}
	return true;
}

void ui_reload_theme(void) {
	const char *dir = ui_assets_dir();
	if (strcmp(ui_theme.dir, dir)) {
		theme_load(&ui_theme, dir);
		theme_use_fonts(&ui_theme);
	}
}

void ui_quit(void) {
	u.quit = true;
}

static bool any_window(void) {
	for (int i = 0; i < MAX_WINDOWS; i++)
		if (u.windows[i])
			return true;
	return false;
}

/* ---- extra fds and timers for apps that talk to helpers (bluetoothctl...) ---- */

struct watch { int fd; void (*cb)(int, void *); void *data; };
static struct watch watches[8];
static int n_watches;
struct timer { void (*cb)(void *); void *data; uint64_t at; };
static struct timer timers[8];
static int n_timers;

void ui_watch(int fd, void (*cb)(int fd, void *data), void *data) {
	ui_unwatch(fd);
	if (n_watches < 8)
		watches[n_watches++] = (struct watch){fd, cb, data};
}

void ui_unwatch(int fd) {
	for (int i = 0; i < n_watches; i++)
		if (watches[i].fd == fd) {
			watches[i] = watches[--n_watches];
			return;
		}
}

void ui_after(int ms, void (*cb)(void *data), void *data) {
	for (int i = 0; i < n_timers; i++)
		if (timers[i].cb == cb) {
			timers[i].at = now_ms() + ms;
			timers[i].data = data;
			return;
		}
	if (n_timers < 8)
		timers[n_timers++] = (struct timer){cb, data, now_ms() + ms};
}

int ui_run(void) {
	while (!u.quit && any_window()) {
		while (wl_display_prepare_read(u.display) != 0)
			wl_display_dispatch_pending(u.display);
		if (wl_display_flush(u.display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(u.display);
			return 1;
		}
		int timeout = -1;
		uint64_t t = now_ms();
		if (u.repeating)
			timeout = u.repeat_next_ms > t ? (int)(u.repeat_next_ms - t) : 0;
		for (int i = 0; i < n_timers; i++) {
			int left = timers[i].at > t ? (int)(timers[i].at - t) : 0;
			if (timeout < 0 || left < timeout)
				timeout = left;
		}
		struct pollfd pfd[9] = {{wl_display_get_fd(u.display), POLLIN, 0}};
		int nw = n_watches;
		for (int i = 0; i < nw; i++)
			pfd[1 + i] = (struct pollfd){watches[i].fd, POLLIN, 0};
		if (poll(pfd, 1 + nw, timeout) > 0 && (pfd[0].revents & POLLIN)) {
			if (wl_display_read_events(u.display) < 0)
				return 1;
		} else {
			wl_display_cancel_read(u.display);
		}
		wl_display_dispatch_pending(u.display);
		for (int i = 0; i < nw; i++)
			if (pfd[1 + i].revents)
				for (int j = 0; j < n_watches; j++)
					if (watches[j].fd == pfd[1 + i].fd) {
						watches[j].cb(watches[j].fd, watches[j].data);
						break;
					}
		t = now_ms();
		for (int i = 0; i < n_timers; i++)
			if (timers[i].at <= t) {
				void (*cb)(void *) = timers[i].cb;
				void *data = timers[i].data;
				timers[i--] = timers[--n_timers];
				cb(data);
			}
		if (u.repeating && now_ms() >= u.repeat_next_ms) {
			deliver_key(u.repeat_key);
			u.repeat_next_ms = now_ms() + 1000 / (u.repeat_rate ? u.repeat_rate : 25);
		}
	}
	wl_display_flush(u.display);
	return 0;
}
