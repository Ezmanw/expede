/* The session around the desktop: XP's Welcome screen, "welcome" while logging
 * on, the Logging off / shutting down screens, the Turn Off Computer dialog,
 * and the system sounds that go with them. Layout and colours follow the
 * DirectUI markup inside logonui.exe. */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "expede.h"

enum state { ST_WELCOME, ST_LOGGING_ON, ST_DESKTOP, ST_LOGGING_OFF, ST_SHUTTING_DOWN, ST_TURNOFF };
enum hot { HOT_NONE, HOT_ACCOUNT, HOT_TURNOFF, HOT_STANDBY, HOT_OFF, HOT_RESTART, HOT_CANCEL, HOT_W7_ARROW, HOT_W7_MENU_RESTART, HOT_W7_MENU_SLEEP, HOT_W7_MENU_LOGOFF };
enum after { AFTER_NONE, AFTER_POWEROFF, AFTER_REBOOT };

struct session {
	struct server *server;
	enum state state;
	enum hot hot, pressed;
	enum after after;
	bool startup_played;
	bool w7_power_menu;
	enum state dialog_from; /* where Cancel on the Turn Off dialog returns to */
	int w, h, w7_bg_w, w7_bg_h;
	struct wlr_scene_buffer *node;
	struct wl_event_source *timer;
	cairo_surface_t *glow, *logo, *frame, *frame_hot, *account_sel, *div_v, *div_top, *div_bottom;
	cairo_surface_t *turnoff, *turnoff_hot, *dialog, *dialog_flag, *dialog_buttons;
	/* Windows 7 logon assets */
	cairo_surface_t *w7_bg, *w7_branding[2], *w7_tile_frame, *w7_spinner, *w7_ease;
	cairo_surface_t *w7_go[3], *w7_shutdown[3], *w7_shutdown_arrow[3], *w7_button[3];
};

/* ---- sounds ---- */

void sound_play(struct server *s, const char *name) {
	if (getenv("EXPEDE_MUTE") || !s->opt_sounds)
		return;
	char path[640], cmd[2400];
	snprintf(path, sizeof(path), "%s/sounds/%s.wav", theme_of(s, s->st_sounds)->dir, name);
	/* whichever player the system has: PipeWire, PulseAudio, then plain ALSA */
	snprintf(cmd, sizeof(cmd), "pw-play '%s' 2>/dev/null || paplay '%s' 2>/dev/null || aplay -q '%s' 2>/dev/null",
		path, path, path);
	spawn(cmd);
}

/* ---- geometry (1024x768 matches XP's own proportions) ---- */

static bool logon7(struct session *se) {
	return theme_of(se->server, se->server->st_logon)->version == 7;
}

static int top_h(struct session *se) { return se->h * 78 / 768; }
static int bottom_h(struct session *se) { return se->h * 96 / 768; }
static int content_mid(struct session *se) { return (top_h(se) + se->h - bottom_h(se)) / 2; }

static struct wlr_box account_box(struct session *se) {
	if (logon7(se))
		return (struct wlr_box){(se->w - 220) / 2, se->h / 2 - 150, 220, 250};
	return (struct wlr_box){se->w / 2 + 26, content_mid(se) - 36, 308, 72};
}

static struct wlr_box w7_go_box(struct session *se) {
	return (struct wlr_box){(se->w - 38) / 2, se->h / 2 + 64, 38, 38};
}

static struct wlr_box turnoff_box(struct session *se) {
	if (logon7(se))
		return (struct wlr_box){se->w - 32 - 21 - 38, se->h - 32 - 28, 38, 28};
	int y = se->h - bottom_h(se) + 22;
	return (struct wlr_box){25, y, 26 + 8 + text_width("Tahoma 14", "Turn off computer"), 26};
}

static struct wlr_box w7_arrow_box(struct session *se) {
	return (struct wlr_box){se->w - 32 - 21, se->h - 32 - 28, 21, 28};
}

static struct wlr_box w7_power_menu_box(struct session *se) {
	return (struct wlr_box){se->w - 32 - 120, se->h - 32 - 28 - 80, 120, 76};
}

static struct wlr_box dialog_box(struct session *se) {
	return (struct wlr_box){(se->w - 313) / 2, (se->h - 198) / 2, 313, 198};
}

/* dialog-relative */
static struct wlr_box dlg_button(int i) {
	static const int cx[3] = {66, 157, 248};
	return (struct wlr_box){cx[i] - 16, 72, 32, 32};
}
static const struct wlr_box dlg_cancel = {228, 163, 75, 23};

static bool in_box(struct wlr_box b, double x, double y) {
	return x >= b.x && x < b.x + b.width && y >= b.y && y < b.y + b.height;
}

/* ---- drawing ---- */

static void rgb(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

static void stretch(cairo_t *cr, cairo_surface_t *s, double x, double y, double w, double h) {
	if (!s)
		return;
	cairo_save(cr);
	cairo_translate(cr, x, y);
	int iw = cairo_image_surface_get_width(s), ih = cairo_image_surface_get_height(s);
	cairo_scale(cr, w / iw, h / ih);
	cairo_rectangle(cr, 0, 0, iw, ih);
	cairo_clip(cr);
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void text_right(cairo_t *cr, const char *font, const char *text, double right, double y, uint32_t c,
		int sdx, int sdy, uint32_t sc) {
	int w = text_width(font, text);
	draw_text(cr, font, text, right - w, y, 0, c, sdx, sdy, sc, false);
}

static void draw_frame_screen(struct session *se, cairo_t *cr) {
	int w = se->w, h = se->h, th = top_h(se), bh = bottom_h(se);
	rgb(cr, 0x00309c);
	cairo_rectangle(cr, 0, 0, w, th);
	cairo_fill(cr);
	rgb(cr, 0x5a7edc);
	cairo_rectangle(cr, 0, th, w, h - th - bh);
	cairo_fill(cr);
	/* the soft light in the top-left of the content area */
	stretch(cr, se->glow, 0, th, 219 * 2.6, 207 * 2.6);
	stretch(cr, se->div_top, 0, th, w, 2);

	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, w, 0);
	cairo_pattern_add_color_stop_rgb(g, 0, 57 / 255.0, 52 / 255.0, 173 / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0, 48 / 255.0, 156 / 255.0);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, 0, h - bh, w, bh);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	stretch(cr, se->div_bottom, 0, h - bh - 1, w, 3);
}

static void draw_account(struct session *se, cairo_t *cr, bool selected, const char *status) {
	struct wlr_box a = account_box(se);
	if (selected)
		draw_surface(cr, se->account_sel, a.x, a.y);
	int fx = a.x + 7, fy = a.y + 7;
	draw_surface(cr, selected ? se->frame_hot : se->frame, fx, fy);
	if (se->server->theme.user_pic)
		stretch(cr, se->server->theme.user_pic, fx + 5, fy + 5, 48, 48);
	const char *user = getenv("USER") ? getenv("USER") : "User";
	draw_text(cr, "Tahoma 14", user, fx + 58 + 7, fy + (status ? 8 : 17), 220, 0xeff7ff, 0, 0, 0, false);
	if (status)
		draw_text(cr, "Tahoma Bold 8", status, fx + 58 + 7, fy + 34, 220, 0xeff7ff, 0, 0, 0, false);
}

/* pick logon/background-WxH.jpg with the closest aspect ratio to the screen */
static void load_w7_bg(struct session *se) {
	if (se->w7_bg && se->w7_bg_w == se->w && se->w7_bg_h == se->h)
		return;
	struct theme *t7 = &se->server->theme7;
	if (!t7->dir[0] || se->w <= 0 || se->h <= 0)
		return;
	char dir[600], best[768] = "";
	snprintf(dir, sizeof(dir), "%s/logon", t7->dir);
	DIR *d = opendir(dir);
	if (!d)
		return;
	double want = (double)se->w / se->h, best_diff = 1e9;
	int best_area = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		int bw = 0, bh = 0;
		if (sscanf(de->d_name, "background-%dx%d.jpg", &bw, &bh) == 2 && bw > 0 && bh > 0) {
			double diff = fabs((double)bw / bh - want);
			int area = abs(bw * bh - se->w * se->h);
			if (diff < best_diff - 0.01 || (fabs(diff - best_diff) <= 0.01 && area < best_area)) {
				best_diff = diff;
				best_area = area;
				snprintf(best, sizeof(best), "%s/%s", dir, de->d_name);
			}
		}
	}
	closedir(d);
	if (best[0]) {
		if (se->w7_bg)
			cairo_surface_destroy(se->w7_bg);
		se->w7_bg = theme_load_image(best);
		se->w7_bg_w = se->w;
		se->w7_bg_h = se->h;
	}
}

static void draw_welcome7(struct session *se, cairo_t *cr) {
	int w = se->w, h = se->h;
	struct theme *t7 = &se->server->theme7;
	theme_use_fonts(t7);
	load_w7_bg(se);
	if (se->w7_bg) {
		stretch(cr, se->w7_bg, 0, 0, w, h);
	} else {
		rgb(cr, 0x0b4177);
		cairo_rectangle(cr, 0, 0, w, h);
		cairo_fill(cr);
	}
	if (se->w7_branding[0])
		draw_frame_dpi(cr, se->w7_branding[0], se->w7_branding[1], 1, 0, (w - 350) / 2, h - 74);

	if (se->state != ST_WELCOME) {
		const char *msg = se->state == ST_LOGGING_ON ? "Welcome" :
			se->state == ST_LOGGING_OFF ? "Logging off..." : "Shutting down...";
		int tw = text_width("Segoe UI 18", msg);
		int sx = (w - (30 + 10 + tw)) / 2, sy = h / 2 - 20;
		if (se->w7_spinner) {
			struct image sp = {.s = se->w7_spinner, .count = 18, .vertical = false};
			draw_image(cr, &sp, 3, sx, sy + 2, 30, 30);
		}
		draw_text(cr, "Segoe UI 18", msg, sx + 40, sy + 1, 0, 0xffffff, 1, 2, 0x000000, false);
		return;
	}

	/* centered user tile (190x190 frame around 126x126 account picture) */
	int fx = (w - 190) / 2, fy = h / 2 - 165;
	cairo_surface_t *upic = t7->user_pic ? t7->user_pic : se->server->theme.user_pic;
	if (upic)
		stretch(cr, upic, fx + 32, fy + 32, 126, 126);
	draw_surface(cr, se->w7_tile_frame, fx, fy);

	const char *user = getenv("USER") ? getenv("USER") : "User";
	int uw = text_width("Segoe UI 18", user);
	draw_text(cr, "Segoe UI 18", user, (w - uw) / 2, fy + 188, 0, 0xffffff, 1, 2, 0x000000, false);

	/* Go arrow button beneath the user name */
	struct wlr_box gb = w7_go_box(se);
	int gf = se->pressed == HOT_ACCOUNT ? 2 : se->hot == HOT_ACCOUNT ? 1 : 0;
	draw_surface(cr, se->w7_go[gf], gb.x, gb.y);

	/* bottom-left Ease of Access button */
	if (se->w7_button[0]) {
		draw_surface(cr, se->w7_button[0], 32, h - 32 - 28);
		if (se->w7_ease)
			draw_surface(cr, se->w7_ease, 32 + (35 - 24) / 2, h - 32 - 28 + 2);
	}

	/* bottom-right red Shut down button + arrow */
	struct wlr_box tb = turnoff_box(se), ab = w7_arrow_box(se);
	int sf = se->pressed == HOT_TURNOFF ? 2 : se->hot == HOT_TURNOFF ? 1 : 0;
	int af = (se->pressed == HOT_W7_ARROW || se->w7_power_menu) ? 2 : se->hot == HOT_W7_ARROW ? 1 : 0;
	draw_surface(cr, se->w7_shutdown[sf], tb.x, tb.y);
	draw_surface(cr, se->w7_shutdown_arrow[af], ab.x, ab.y);

	if (se->w7_power_menu) {
		struct wlr_box mb = w7_power_menu_box(se);
		mb.y += 24;
		mb.height -= 24;
		rgb(cr, 0x646464);
		cairo_rectangle(cr, mb.x, mb.y, mb.width, mb.height);
		cairo_fill(cr);
		rgb(cr, 0xf0f0f0);
		cairo_rectangle(cr, mb.x + 1, mb.y + 1, mb.width - 2, mb.height - 2);
		cairo_fill(cr);
		static const char *labels[2] = {"Restart", "Sleep"};
		static const enum hot hots[2] = {HOT_W7_MENU_RESTART, HOT_W7_MENU_SLEEP};
		for (int i = 0; i < 2; i++) {
			int ry = mb.y + 2 + i * 24;
			if (se->hot == hots[i]) {
				rgb(cr, 0x3399ff);
				cairo_rectangle(cr, mb.x + 2, ry, mb.width - 4, 24);
				cairo_fill(cr);
			}
			draw_text(cr, "Segoe UI 9", labels[i], mb.x + 12, ry + 4, 0,
				se->hot == hots[i] ? 0xffffff : 0x000000, 0, 0, 0, false);
		}
	}
}

static void draw_welcome(struct session *se, cairo_t *cr) {
	if (logon7(se)) {
		draw_welcome7(se, cr);
		return;
	}
	theme_use_fonts(&se->server->theme);
	int w = se->w, h = se->h, mid = content_mid(se);
	draw_frame_screen(se, cr);
	int ch = h - top_h(se) - bottom_h(se);
	stretch(cr, se->div_v, w / 2, mid - ch * 0.4, 1, ch * 0.8);

	if (se->state == ST_LOGGING_ON) {
		/* "welcome", Arial bold italic 36pt with its offset blue shadow */
		text_right(cr, "Arial, Bold Italic 36", "welcome", w / 2 - 22, mid - 40, 0xeff7ff, 2, 3, 0x3151b5);
		draw_account(se, cr, true, "Loading your personal settings...");
		return;
	}
	if (se->state == ST_LOGGING_OFF || se->state == ST_SHUTTING_DOWN) {
		draw_surface(cr, se->logo, w / 2 - 30 - 137, mid - 60);
		const char *msg = se->state == ST_LOGGING_OFF ? "Logging off..." : "Windows is shutting down...";
		draw_text(cr, "Arial 18", msg, w / 2 + 30, mid - 26, 0, 0xeff7ff, 0, 0, 0, false);
		draw_text(cr, "Arial 10", "Saving your settings...", w / 2 + 31, mid + 6, 0, 0xeff7ff, 0, 0, 0, false);
		return;
	}
	draw_surface(cr, se->logo, w / 2 - 30 - 137, mid - 86);
	text_right(cr, "Arial 14", "To begin, click your user name", w / 2 - 30, mid + 8, 0xeff7ff, 0, 0, 0);
	draw_account(se, cr, se->hot == HOT_ACCOUNT, NULL);

	struct wlr_box t = turnoff_box(se);
	draw_surface(cr, se->hot == HOT_TURNOFF ? se->turnoff_hot : se->turnoff, t.x, t.y);
	int tx = t.x + 26 + 8, ty = t.y + 1;
	draw_text(cr, "Tahoma 14", "Turn off computer", tx, ty, 0, 0xffffff, 0, 0, 0, false);
	if (se->hot == HOT_TURNOFF) {
		rgb(cr, 0xffffff);
		cairo_rectangle(cr, tx, ty + 22, text_width("Tahoma 14", "Turn off computer"), 1);
		cairo_fill(cr);
	}
	draw_text(cr, "Tahoma 9", "After you log on, you can add or change accounts.\nJust go to Control Panel and click User Accounts.",
		w / 2 + 18, h - bottom_h(se) + 26, 0, 0xffffff, 0, 0, 0, false);
}

static void draw_turnoff(struct session *se, cairo_t *cr) {
	/* XP fades the desktop to grey behind this dialog */
	cairo_set_source_rgba(cr, 0.35, 0.35, 0.35, 0.55);
	cairo_paint(cr);
	struct wlr_box d = dialog_box(se);
	cairo_save(cr);
	cairo_translate(cr, d.x, d.y);
	draw_surface(cr, se->dialog, 0, 0);
	draw_surface(cr, se->dialog_flag, 313 - 48 - 6, 2);
	draw_text(cr, "Arial 14", "Turn off computer", 12, 12, 0, 0xffffff, 0, 0, 0, false);

	/* strip frames: turn off 0-2, stand by 3-5, restart 6-8 (normal, pressed, hot) */
	static const char *labels[3] = {"Stand By", "Turn Off", "Restart"};
	static const int base[3] = {3, 0, 6};
	static const enum hot hots[3] = {HOT_STANDBY, HOT_OFF, HOT_RESTART};
	for (int i = 0; i < 3; i++) {
		struct wlr_box b = dlg_button(i);
		int f = base[i] + (se->pressed == hots[i] ? 1 : se->hot == hots[i] ? 2 : 0);
		draw_frame_true(cr, se->dialog_buttons, 10, f, b.x, b.y);
		int tw = text_width("Tahoma 8", labels[i]);
		draw_text(cr, "Tahoma 8", labels[i], b.x + 16 - tw / 2, b.y + 38, 0, 0xffffff, 0, 0, 0, true);
	}
	struct theme *t = &se->server->theme;
	int bf = se->pressed == HOT_CANCEL ? 2 : se->hot == HOT_CANCEL ? 1 : 0;
	draw_image(cr, &t->push_button, bf, dlg_cancel.x, dlg_cancel.y, dlg_cancel.width, dlg_cancel.height);
	int cw = text_width("Tahoma 8", "Cancel");
	draw_text(cr, "Tahoma 8", "Cancel", dlg_cancel.x + (dlg_cancel.width - cw) / 2, dlg_cancel.y + 5, 0, 0x000000, 0, 0, 0, true);
	cairo_restore(cr);
}

static void redraw(struct session *se) {
	bool visible = se->state != ST_DESKTOP;
	if (se->node)
		wlr_scene_node_set_enabled(&se->node->node, visible);
	if (!visible)
		return;
	cairo_surface_t *s = canvas_new(se->w, se->h);
	cairo_t *cr = cairo_create(s);
	if (se->state == ST_TURNOFF)
		draw_turnoff(se, cr);
	else
		draw_welcome(se, cr);
	cairo_destroy(cr);
	scene_show(&se->node, se->server->layer_overlay, s, 0, 0);
}

/* ---- state changes ---- */

static void set_state(struct session *se, enum state st, int timeout_ms) {
	se->state = st;
	se->hot = se->pressed = HOT_NONE;
	se->w7_power_menu = false;
	wl_event_source_timer_update(se->timer, timeout_ms);
	redraw(se);
}

static void close_all_windows(struct server *s, bool force) {
	struct view *v, *tmp;
	wl_list_for_each_safe(v, tmp, &s->views, link) {
		if (force)
			wl_client_destroy(wl_resource_get_client(v->xt->resource));
		else
			view_close(v);
	}
}

static int on_timer(void *data) {
	struct session *se = data;
	struct server *s = se->server;
	switch (se->state) {
	case ST_LOGGING_ON:
		set_state(se, ST_DESKTOP, 0);
		sound_play(s, se->startup_played ? "logon" : "startup");
		se->startup_played = true;
		break;
	case ST_LOGGING_OFF:
		close_all_windows(s, true); /* anything that ignored the polite close */
		set_state(se, ST_WELCOME, 0);
		break;
	case ST_SHUTTING_DOWN:
		close_all_windows(s, true);
		if (getenv("EXPEDE_NO_POWEROFF"))
			wl_display_terminate(s->display);
		else
			spawn(se->after == AFTER_REBOOT ? "systemctl reboot" : "systemctl poweroff");
		break;
	default:
		break;
	}
	return 0;
}

static void shut_down(struct session *se, enum after after) {
	se->after = after;
	close_all_windows(se->server, false);
	sound_play(se->server, "shutdown");
	set_state(se, ST_SHUTTING_DOWN, 4000);
}

void session_logoff(struct session *se) {
	close_all_windows(se->server, false);
	sound_play(se->server, "logoff");
	set_state(se, ST_LOGGING_OFF, 2500);
}

void session_turnoff_dialog(struct session *se) {
	if (se->state != ST_TURNOFF)
		se->dialog_from = se->state;
	set_state(se, ST_TURNOFF, 0);
}

static void dialog_cancel(struct session *se) {
	set_state(se, se->dialog_from, 0);
}

/* ---- input ---- */

bool session_active(struct session *se) {
	return se->state != ST_DESKTOP;
}

static enum hot hot_at(struct session *se, double x, double y) {
	if (se->state == ST_WELCOME) {
		if (logon7(se) && se->w7_power_menu) {
			struct wlr_box mb = w7_power_menu_box(se);
			mb.y += 24;
			mb.height -= 24;
			if (in_box(mb, x, y)) {
				int idx = (int)(y - (mb.y + 2)) / 24;
				return idx == 0 ? HOT_W7_MENU_RESTART : HOT_W7_MENU_SLEEP;
			}
		}
		if (logon7(se) && in_box(w7_arrow_box(se), x, y))
			return HOT_W7_ARROW;
		if (in_box(account_box(se), x, y))
			return HOT_ACCOUNT;
		if (in_box(turnoff_box(se), x, y))
			return HOT_TURNOFF;
	} else if (se->state == ST_TURNOFF) {
		struct wlr_box d = dialog_box(se);
		double dx = x - d.x, dy = y - d.y;
		static const enum hot hots[3] = {HOT_STANDBY, HOT_OFF, HOT_RESTART};
		for (int i = 0; i < 3; i++)
			if (in_box(dlg_button(i), dx, dy))
				return hots[i];
		if (in_box(dlg_cancel, dx, dy))
			return HOT_CANCEL;
	}
	return HOT_NONE;
}

void session_motion(struct session *se, double x, double y) {
	enum hot h = hot_at(se, x, y);
	if (h != se->hot) {
		se->hot = h;
		redraw(se);
	}
	struct server *s = se->server;
	wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, h != HOT_NONE && se->state == ST_WELCOME ? "pointer" : "default");
}

void session_button(struct session *se, double x, double y, bool pressed) {
	enum hot h = hot_at(se, x, y);
	if (pressed) {
		if (se->w7_power_menu && h != HOT_W7_ARROW && h != HOT_W7_MENU_RESTART && h != HOT_W7_MENU_SLEEP)
			se->w7_power_menu = false;
		se->pressed = h;
		redraw(se);
		return;
	}
	enum hot p = se->pressed;
	se->pressed = HOT_NONE;
	if (p != h || h == HOT_NONE) {
		redraw(se);
		return;
	}
	switch (h) {
	case HOT_ACCOUNT:
		set_state(se, ST_LOGGING_ON, 1800);
		break;
	case HOT_TURNOFF:
		if (logon7(se))
			shut_down(se, AFTER_POWEROFF);
		else
			session_turnoff_dialog(se);
		break;
	case HOT_W7_ARROW:
		se->w7_power_menu = !se->w7_power_menu;
		redraw(se);
		break;
	case HOT_W7_MENU_RESTART:
		se->w7_power_menu = false;
		shut_down(se, AFTER_REBOOT);
		break;
	case HOT_W7_MENU_SLEEP:
		se->w7_power_menu = false;
		redraw(se);
		if (!getenv("EXPEDE_NO_POWEROFF"))
			spawn("systemctl suspend");
		break;
	case HOT_OFF:
		shut_down(se, AFTER_POWEROFF);
		break;
	case HOT_RESTART:
		shut_down(se, AFTER_REBOOT);
		break;
	case HOT_STANDBY:
		dialog_cancel(se);
		if (!getenv("EXPEDE_NO_POWEROFF"))
			spawn("systemctl suspend");
		break;
	case HOT_CANCEL:
		dialog_cancel(se);
		break;
	default:
		break;
	}
}

void session_key(struct session *se, uint32_t sym) {
	if (se->state == ST_TURNOFF && sym == XKB_KEY_Escape)
		dialog_cancel(se);
	else if (se->state == ST_WELCOME && se->w7_power_menu && sym == XKB_KEY_Escape) {
		se->w7_power_menu = false;
		redraw(se);
	} else if (se->state == ST_WELCOME && sym == XKB_KEY_Return)
		set_state(se, ST_LOGGING_ON, 1800);
}

/* ---- setup ---- */

void session_layout(struct session *se) {
	server_screen(se->server, &se->w, &se->h);
	redraw(se);
}

struct session *session_create(struct server *s, bool autologin) {
	struct session *se = calloc(1, sizeof(*se));
	se->server = s;
	struct theme *t = &s->theme;
	se->glow = theme_png(t, "logon/glow.png");
	se->logo = theme_png(t, "logon/logo.png");
	se->frame = theme_png(t, "logon/frame.png");
	se->frame_hot = theme_png(t, "logon/frame-hot.png");
	se->account_sel = theme_png(t, "logon/account-selected.png");
	se->div_v = theme_png(t, "logon/divider-v.png");
	se->div_top = theme_png(t, "logon/divider-top.png");
	se->div_bottom = theme_png(t, "logon/divider-bottom.png");
	se->turnoff = theme_png(t, "logon/turnoff.png");
	se->turnoff_hot = theme_png(t, "logon/turnoff-hot.png");
	se->dialog = theme_png(t, "logon/dialog.png");
	se->dialog_flag = theme_png(t, "logon/dialog-flag.png");
	se->dialog_buttons = theme_png(t, "logon/dialog-buttons.png");
	if (s->have7) {
		struct theme *t7 = &s->theme7;
		se->w7_branding[0] = theme_png(t7, "logon/branding.png");
		se->w7_branding[1] = theme_png(t7, "logon/branding-120.png");
		se->w7_tile_frame = theme_png(t7, "logon/tile-frame.png");
		se->w7_spinner = theme_png(t7, "logon/spinner.png");
		se->w7_ease = theme_png(t7, "logon/ease-of-access.png");
		se->w7_go[0] = theme_png(t7, "logon/go.png");
		se->w7_go[1] = theme_png(t7, "logon/go-hot.png");
		se->w7_go[2] = theme_png(t7, "logon/go-pressed.png");
		se->w7_shutdown[0] = theme_png(t7, "logon/shutdown.png");
		se->w7_shutdown[1] = theme_png(t7, "logon/shutdown-hot.png");
		se->w7_shutdown[2] = theme_png(t7, "logon/shutdown-pressed.png");
		se->w7_shutdown_arrow[0] = theme_png(t7, "logon/shutdown-arrow.png");
		se->w7_shutdown_arrow[1] = theme_png(t7, "logon/shutdown-arrow-hot.png");
		se->w7_shutdown_arrow[2] = theme_png(t7, "logon/shutdown-arrow-pressed.png");
		se->w7_button[0] = theme_png(t7, "logon/button.png");
		se->w7_button[1] = theme_png(t7, "logon/button-hot.png");
		se->w7_button[2] = theme_png(t7, "logon/button-pressed.png");
	}

	se->timer = wl_event_loop_add_timer(wl_display_get_event_loop(s->display), on_timer, se);
	server_screen(s, &se->w, &se->h);
	if (autologin) {
		/* XP skips the Welcome screen for a single account with no password */
		se->state = ST_LOGGING_ON;
		wl_event_source_timer_update(se->timer, 1);
	} else {
		se->state = ST_WELCOME;
		/* "Start Windows" plays as the Welcome screen comes up */
		sound_play(s, "startup");
		se->startup_played = true;
	}
	return se;
}
