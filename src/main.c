/* Expede: a Wayland desktop that looks and behaves like Windows XP.
 * The window-management core follows wlroots' tinywl; everything visible is
 * drawn from the Luna theme ripped off the user's own XP ISO. */
#define _GNU_SOURCE
#include <assert.h>
#include <drm_fourcc.h>
#include <getopt.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <libinput.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include "expede.h"

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame, request_state, destroy;
};

struct pointer_dev {
	struct wl_list link;
	struct server *server;
	struct wlr_input_device *dev;
	struct wl_listener destroy;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

static struct server *the_server;
static void snip_begin(struct server *s);

void server_screen(struct server *s, int *w, int *h) {
	struct wlr_box box;
	wlr_output_layout_get_box(s->output_layout, NULL, &box);
	*w = box.width ? box.width : 1024;
	*h = box.height ? box.height : 768;
}

void spawn(const char *cmd) {
	if (!strcmp(cmd, "expede-snip")) {
		snip_begin(the_server);
		return;
	}
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", cmd, (void *)NULL);
		_exit(127);
	}
}

/* ---------------------------------------------------------------- views */

static struct icon_set icon_for_app(struct theme *t, const char *app_id) {
	if (app_id && strstr(app_id, "foot"))
		return theme_icon(t, "cmd");
	if (app_id && (strstr(app_id, "featherpad") || strstr(app_id, "FeatherPad") || strstr(app_id, "ousepad")))
		return theme_icon(t, "notepad");
	if (app_id && strstr(app_id, "expede-settings"))
		return theme_icon(t, "control-panel");
	if (app_id && (strstr(app_id, "pcmanfm") || strstr(app_id, "expede-explorer")))
		return theme_icon(t, "explorer");
	return theme_icon(t, "app");
}

struct theme *theme_of(struct server *s, int style) {
	if ((style == 7 && s->have7) || !s->have_xp)
		return &s->theme7;
	return &s->theme;
}

int taskbar_h(struct server *s) {
	return theme_of(s, s->st_taskbar)->version == 7 ? 40 : TASKBAR_H;
}

/* Windows 7 frames: 8px glass borders and a 30px caption (22 when maximized) */
#define W7_BORDER 8
#define W7_CAPTION 30
static bool w7(struct view *v) {
	return theme_of(v->server, v->server->st_frames)->version == 7;
}

/* Frame sizes. Apps that draw their own title bar (GTK4, Chromium, Electron...)
 * get no frame at all, so there is never a second title bar. */
static int side_w(struct view *v) {
	return v->csd || v->fullscreen || v->maximized ? 0 : w7(v) ? W7_BORDER : BORDER;
}

static int caption_h(struct view *v) {
	if (v->csd || v->fullscreen)
		return 0;
	if (w7(v))
		return v->maximized ? 22 : W7_CAPTION;
	return v->maximized ? CAPTION - BORDER : CAPTION;
}

static int bottom_h(struct view *v) {
	return v->csd || v->fullscreen || v->maximized ? 0 : w7(v) ? W7_BORDER : BORDER;
}

/* Windows 7 caption buttons hang from the top edge: min and max 26 wide, close 45 */
static void w7_buttons(struct view *v, int fw, struct wlr_box *mn, struct wlr_box *mx, struct wlr_box *cl) {
	int y = v->maximized ? 0 : 1, h = v->maximized ? 19 : 20, r = fw - (v->maximized ? 2 : 6);
	*cl = (struct wlr_box){r - 45, y, 45, h};
	*mx = (struct wlr_box){cl->x - 26, y, 26, h};
	*mn = (struct wlr_box){mx->x - 26, y, 26, h};
}

static int view_frame_w(struct view *v) {
	return v->cw + 2 * side_w(v);
}

static enum part button_at(struct view *v, double lx, double ly) {
	struct theme *t = theme_of(v->server, v->server->st_frames);
	if (v->csd || v->fullscreen)
		return PART_NONE;
	int fw = view_frame_w(v);
	double x = lx - v->x, y = ly - v->y;
	if (w7(v)) {
		struct wlr_box b[3];
		w7_buttons(v, fw, &b[0], &b[1], &b[2]);
		static const enum part parts[3] = {PART_MIN, PART_MAX, PART_CLOSE};
		for (int i = 0; i < 3; i++)
			if (x >= b[i].x && x < b[i].x + b[i].width && y >= b[i].y && y < b[i].y + b[i].height)
				return parts[i];
		return PART_NONE;
	}
	int by = v->maximized ? 2 : 5;
	if (y < by || y >= by + 21)
		return PART_NONE;
	if (x >= fw - t->close_off && x < fw - t->close_off + 21)
		return PART_CLOSE;
	if (x >= fw - t->max_off && x < fw - t->max_off + 21)
		return PART_MAX;
	if (x >= fw - t->min_off && x < fw - t->min_off + 21)
		return PART_MIN;
	return PART_NONE;
}

static void draw_caption_button(struct theme *t, cairo_t *cr, struct image *bg, cairo_surface_t *glyph,
		int x, int y, bool active, bool hot, bool pressed) {
	/* 8 frames: normal hot pressed disabled, then the same for inactive windows */
	int f = pressed ? 2 : hot ? 1 : 0;
	if (!active)
		f += 4;
	draw_image(cr, bg, f, x, y, 21, 21);
	if (glyph) {
		int gf = active ? (pressed ? 2 : hot ? 1 : 0) : 4;
		int gh = cairo_image_surface_get_height(glyph) / 8;
		int gw = cairo_image_surface_get_width(glyph);
		draw_frame_true(cr, glyph, 8, gf, x + (21 - gw) / 2, y + (21 - gh) / 2);
	}
	(void)t;
}

static void set_deco(struct wlr_scene_buffer **node, struct wlr_scene_tree *parent, cairo_surface_t *s, int x, int y) {
	scene_show(node, parent, s, x, y);
}

/* the scene's xdg-surface node already shifts out the client's shadow margins */
static void place_content(struct view *v) {
	wlr_scene_node_set_position(&v->content->node, side_w(v), caption_h(v));
}

/* ---- Windows 7 glass ---- */

static void round_path(cairo_t *cr, double x, double y, double w, double h, double rt, double rb) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - rt, y + rt, rt, -M_PI / 2, 0);
	cairo_arc(cr, x + w - rb, y + h - rb, rb, 0, M_PI / 2);
	cairo_arc(cr, x + rb, y + h - rb, rb, M_PI / 2, M_PI);
	cairo_arc(cr, x + rt, y + rt, rt, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void draw_glass(struct server *s, cairo_t *cr, double x, double y, double w, double h, double rt, double rb,
		bool active) {
	double r = ((s->glass_rgb >> 16) & 255) / 255.0, g = ((s->glass_rgb >> 8) & 255) / 255.0,
		b = (s->glass_rgb & 255) / 255.0, k = s->glass_intensity / 100.0;
	cairo_save(cr);
	round_path(cr, x, y, w, h, rt, rb);
	cairo_clip_preserve(cr);
	if (s->opt_glass) {
		/* over the blurred desktop: a milky layer, then the window colour */
		cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.16 : 0.32);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, r, g, b, active ? 0.18 + 0.6 * k : 0.06 + 0.25 * k);
	} else {
		/* transparency off: the colour over Windows 7's light grey, opaque */
		cairo_set_source_rgb(cr, 0.85, 0.88, 0.92);
		cairo_fill_preserve(cr);
		cairo_set_source_rgba(cr, r, g, b, active ? 0.22 + 0.6 * k : 0.08 + 0.25 * k);
	}
	cairo_fill(cr);
	struct theme *t = &s->theme7;
	if (t->w7_reflect) {
		cairo_set_source_surface(cr, t->w7_reflect, x, y);
		cairo_paint_with_alpha(cr, active ? 1 : 0.6);
	}
	cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + 26);
	cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, active ? 0.30 : 0.2);
	cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0);
	cairo_set_source(cr, p);
	cairo_rectangle(cr, x, y, w, 26);
	cairo_fill(cr);
	cairo_pattern_destroy(p);
	cairo_restore(cr);
	cairo_set_line_width(cr, 1);
	round_path(cr, x + 0.5, y + 0.5, w - 1, h - 1, rt, rb);
	cairo_set_source_rgba(cr, 0, 0, 0, active ? 0.6 : 0.4);
	cairo_stroke(cr);
	round_path(cr, x + 1.5, y + 1.5, w - 3, h - 3, rt > 1 ? rt - 1 : 0, rb > 1 ? rb - 1 : 0);
	cairo_set_source_rgba(cr, 1, 1, 1, active ? 0.55 : 0.4);
	cairo_stroke(cr);
}

/* soft white haze behind caption text, so black titles read on any glass */
static void title_glow(cairo_t *cr, double x, double y, double w, double h) {
	for (int i = 6; i >= 1; i--) {
		round_path(cr, x - i * 2, y - i, w + i * 4, h + i * 2, h / 2 + i, h / 2 + i);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.07);
		cairo_fill(cr);
	}
}

static void w7_caption_button(struct theme *t, cairo_t *cr, struct wlr_box b, int which, int glyph, bool active,
		bool hot, bool pressed) {
	int f = pressed ? 2 : hot ? 1 : 0;
	if (hot || pressed) {
		/* the hover glow spills past the button */
		struct image *gl = &t->w7_glow[which == 2];
		draw_image(cr, gl, 0, b.x - 8, b.y - 1, b.width + 16, b.height + 9);
	}
	draw_image(cr, &t->w7_btn[!active][which], f, b.x, b.y, b.width, b.height);
	cairo_surface_t *lo = t->w7_glyph[glyph][0], *hi = t->w7_glyph[glyph][1];
	if (lo) {
		int gw = cairo_image_surface_get_width(lo), gh = frame_height(lo, 4);
		draw_frame_dpi(cr, lo, hi, 4, active ? f : 0, b.x + (b.width - gw) / 2, b.y + (b.height - gh) / 2);
	}
}

/* one piece of the frame: the whole frame is drawn, clipped to the piece */
static cairo_surface_t *w7_piece(struct view *v, int px, int py, int pw, int ph) {
	struct server *s = v->server;
	struct theme *t = &s->theme7;
	int fw = view_frame_w(v), caph = caption_h(v), fh = caph + v->ch + bottom_h(v);
	cairo_surface_t *c = canvas_new(pw, ph);
	cairo_t *cr = cairo_create(c);
	cairo_translate(cr, -px, -py);
	cairo_rectangle(cr, px, py, pw, ph);
	cairo_clip(cr);
	bool max = v->maximized;
	draw_glass(s, cr, max ? -2 : 0, max ? -2 : 0, max ? fw + 4 : fw, max ? fh + 4 : fh, max ? 0 : 6,
		max ? 0 : 3, v->active);
	/* the client sits in a thin light line, then a dark one */
	int sw = side_w(v);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, sw - 1.5, caph - 1.5, v->cw + 3, v->ch + 3);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
	cairo_stroke(cr);
	cairo_rectangle(cr, sw - 0.5, caph - 0.5, v->cw + 1, v->ch + 1);
	cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
	cairo_stroke(cr);
	if (py < caph) {
		struct wlr_box mn, mx, cl;
		w7_buttons(v, fw, &mn, &mx, &cl);
		int ix = max ? 4 : 9, iy = (caph - 16) / 2 + (max ? 0 : 1);
		if (v->icon.s16)
			draw_surface(cr, v->icon.s16, ix, iy);
		const char *title = v->xt->title && *v->xt->title ? v->xt->title : "Untitled";
		theme_use_fonts(t);
		int tx = ix + 21, tmax = mn.x - tx - 8;
		int tw = text_width("Segoe UI 9", title);
		if (tw > tmax)
			tw = tmax;
		if (tw > 0) {
			title_glow(cr, tx, iy - 1, tw, 18);
			draw_text(cr, "Segoe UI 9", title, tx, iy - 1, tmax, v->active ? 0x000000 : 0x444444, 0, 0, 0, false);
		}
		w7_caption_button(t, cr, mn, 0, 2, v->active, v->hot == PART_MIN, v->pressed == PART_MIN);
		w7_caption_button(t, cr, mx, 1, max ? 3 : 1, v->active, v->hot == PART_MAX, v->pressed == PART_MAX);
		w7_caption_button(t, cr, cl, 2, 0, v->active, v->hot == PART_CLOSE, v->pressed == PART_CLOSE);
	}
	cairo_destroy(cr);
	return c;
}

static void view_update_glass(struct view *v) {
	struct server *s = v->server;
	bool on = w7(v) && s->opt_glass && !v->csd && !v->fullscreen;
	int fw = view_frame_w(v), caph = caption_h(v), sw = side_w(v), bh = bottom_h(v), r = v->maximized ? 0 : 6;
	struct wlr_box pieces[5] = {
		{r, 0, fw - 2 * r, r}, {0, r, fw, caph - r},
		{0, caph, sw, v->ch}, {sw + v->cw, caph, sw, v->ch}, {0, caph + v->ch, fw, bh},
	};
	for (int i = 0; i < 5; i++) {
		struct wlr_box b = pieces[i];
		if (!on || b.width <= 0 || b.height <= 0) {
			if (v->glass[i])
				wlr_scene_node_set_enabled(&v->glass[i]->node, false);
			continue;
		}
		glass_backdrop(s, &v->glass[i], v->tree, b.x, b.y, v->x + b.x, v->y + b.y, b.width, b.height);
		if (v->glass[i])
			wlr_scene_node_lower_to_bottom(&v->glass[i]->node);
	}
}

void glass_backdrop(struct server *s, struct wlr_scene_buffer **node, struct wlr_scene_tree *parent,
		int px, int py, int x, int y, int w, int h) {
	if (!s->glass_buf || !s->opt_glass) {
		if (*node)
			wlr_scene_node_set_enabled(&(*node)->node, false);
		return;
	}
	/* clip to the screen: the source box must stay inside the picture */
	int sw, sh;
	server_screen(s, &sw, &sh);
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > sw ? sw : x + w, y1 = y + h > sh ? sh : y + h;
	if (x1 <= x0 || y1 <= y0) {
		if (*node)
			wlr_scene_node_set_enabled(&(*node)->node, false);
		return;
	}
	if (!*node)
		*node = wlr_scene_buffer_create(parent, s->glass_buf);
	else if ((*node)->buffer != s->glass_buf)
		wlr_scene_buffer_set_buffer(*node, s->glass_buf);
	double bw = s->glass_buf->width, bh = s->glass_buf->height;
	struct wlr_fbox src = {x0 * bw / sw, y0 * bh / sh, (x1 - x0) * bw / sw, (y1 - y0) * bh / sh};
	if (src.x + src.width > bw) src.width = bw - src.x;
	if (src.y + src.height > bh) src.height = bh - src.y;
	wlr_scene_buffer_set_source_box(*node, &src);
	wlr_scene_buffer_set_dest_size(*node, x1 - x0, y1 - y0);
	wlr_scene_node_set_position(&(*node)->node, px + (x0 - x), py + (y0 - y));
	wlr_scene_node_set_enabled(&(*node)->node, true);
}

static void view_redraw_w7(struct view *v) {
	int fw = view_frame_w(v), caph = caption_h(v), sw = side_w(v), bh = bottom_h(v);
	set_deco(&v->deco[0], v->tree, w7_piece(v, 0, 0, fw, caph), 0, 0);
	if (v->maximized) {
		for (int i = 1; i < 4; i++)
			if (v->deco[i])
				wlr_scene_node_set_enabled(&v->deco[i]->node, false);
	} else {
		set_deco(&v->deco[1], v->tree, w7_piece(v, 0, caph, sw, v->ch), 0, caph);
		set_deco(&v->deco[2], v->tree, w7_piece(v, sw + v->cw, caph, sw, v->ch), sw + v->cw, caph);
		set_deco(&v->deco[3], v->tree, w7_piece(v, 0, caph + v->ch, fw, bh), 0, caph + v->ch);
	}
	view_update_glass(v);
}

/* move a window's node, keeping its glass in step with the desktop behind it */
static void view_move_node(struct view *v) {
	wlr_scene_node_set_position(&v->tree->node, v->x, v->y);
	if (v->glass[0] || (w7(v) && v->server->opt_glass))
		view_update_glass(v);
}

void server_glass_changed(struct server *s) {
	struct view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped)
			view_update_glass(v);
}

static void view_redraw(struct view *v) {
	struct theme *t = theme_of(v->server, v->server->st_frames);
	if (v->csd || v->fullscreen) {
		for (int i = 0; i < 4; i++)
			if (v->deco[i])
				wlr_scene_node_set_enabled(&v->deco[i]->node, false);
		place_content(v);
		return;
	}
	if (w7(v)) {
		view_redraw_w7(v);
		place_content(v);
		return;
	}
	for (int i = 0; i < 5; i++)
		if (v->glass[i])
			wlr_scene_node_set_enabled(&v->glass[i]->node, false);
	int fw = view_frame_w(v);
	int frame = v->active ? 0 : 1;
	int caph = caption_h(v);

	cairo_surface_t *s = canvas_new(fw, caph);
	cairo_t *cr = cairo_create(s);
	draw_image(cr, v->maximized ? &t->max_caption : &t->caption, frame, 0, 0, fw, caph);
	int by = v->maximized ? 2 : 5, ix = v->maximized ? 4 : 8;
	if (v->icon.s16)
		draw_surface(cr, v->icon.s16, ix - 2, by + 2);
	const char *title = v->xt->title && *v->xt->title ? v->xt->title : "Untitled";
	int tmax = fw - t->min_off - ix - 24;
	/* XP active caption: Trebuchet MS bold 10pt, white with a dark blue drop shadow */
	draw_text(cr, "Trebuchet MS, Bold 10", title, ix + 18, by + 2, tmax,
		v->active ? 0xffffff : 0xd8e4f8, v->active ? 1 : 0, v->active ? 1 : 0, 0x0a1883, false);
	draw_caption_button(t, cr, &t->caption_btn, t->min_glyph, fw - t->min_off, by, v->active,
		v->hot == PART_MIN, v->pressed == PART_MIN);
	draw_caption_button(t, cr, &t->caption_btn, v->maximized ? t->restore_glyph : t->max_glyph,
		fw - t->max_off, by, v->active, v->hot == PART_MAX, v->pressed == PART_MAX);
	draw_caption_button(t, cr, &t->close_btn, t->close_glyph, fw - t->close_off, by, v->active,
		v->hot == PART_CLOSE, v->pressed == PART_CLOSE);
	cairo_destroy(cr);
	set_deco(&v->deco[0], v->tree, s, 0, 0);

	if (v->maximized) {
		for (int i = 1; i < 4; i++)
			if (v->deco[i])
				wlr_scene_node_set_enabled(&v->deco[i]->node, false);
		place_content(v);
		return;
	}
	struct image *sides[2] = {&t->frame_left, &t->frame_right};
	for (int i = 0; i < 2; i++) {
		s = canvas_new(BORDER, v->ch);
		cr = cairo_create(s);
		draw_image(cr, sides[i], frame, 0, 0, BORDER, v->ch);
		cairo_destroy(cr);
		set_deco(&v->deco[1 + i], v->tree, s, i ? BORDER + v->cw : 0, caph);
	}
	s = canvas_new(fw, BORDER);
	cr = cairo_create(s);
	draw_image(cr, &t->frame_bottom, frame, 0, 0, fw, BORDER);
	cairo_destroy(cr);
	set_deco(&v->deco[3], v->tree, s, 0, caph + v->ch);
	place_content(v);
}

static struct view *focused_view(struct server *s) {
	struct view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->active)
			return v;
	return NULL;
}

void view_focus(struct view *v) {
	struct server *s = the_server;
	struct wlr_seat *seat = s->seat;
	struct view *old = focused_view(s);
	if (old && old != v) {
		old->active = false;
		wlr_xdg_toplevel_set_activated(old->xt, false);
		view_redraw(old);
	}
	if (!v) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		shell_views_changed(s->shell);
		return;
	}
	if (v->minimized)
		view_set_minimized(v, false);
	wlr_scene_node_raise_to_top(&v->tree->node);
	v->active = true;
	wlr_xdg_toplevel_set_activated(v->xt, true);
	view_redraw(v);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(seat, v->xt->base->surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	shell_views_changed(s->shell);
}

static void focus_next_visible(struct server *s, struct view *except) {
	/* scene order: the last child of the views layer is on top */
	struct wlr_scene_node *n;
	wl_list_for_each_reverse(n, &s->layer_views->children, link) {
		struct view *v = n->data;
		if (v && v != except && v->mapped && !v->minimized) {
			view_focus(v);
			return;
		}
	}
	view_focus(NULL);
}

void view_set_minimized(struct view *v, bool min) {
	if (v->minimized != min && v->mapped)
		sound_play(v->server, min ? "minimize" : "restore");
	v->minimized = min;
	wlr_scene_node_set_enabled(&v->tree->node, !min && v->mapped);
	if (min && v->active) {
		v->active = false;
		wlr_xdg_toplevel_set_activated(v->xt, false);
		view_redraw(v);
		focus_next_visible(v->server, v);
	}
	shell_views_changed(v->server->shell);
}

static void view_set_maximized(struct view *v, bool max) {
	if (v->maximized == max)
		return;
	int sw, sh;
	server_screen(v->server, &sw, &sh);
	if (max) {
		v->saved = (struct wlr_box){v->x, v->y, v->cw, v->ch};
		v->maximized = true;
		v->x = 0;
		v->y = 0;
		wlr_xdg_toplevel_set_size(v->xt, sw, sh - taskbar_h(v->server) - caption_h(v));
	} else {
		v->maximized = false;
		v->x = v->saved.x;
		v->y = v->saved.y;
		wlr_xdg_toplevel_set_size(v->xt, v->saved.width, v->saved.height);
	}
	wlr_xdg_toplevel_set_maximized(v->xt, max);
	view_move_node(v);
	view_redraw(v);
}

/* Fullscreen video and browsers: no frame, the whole screen, above the taskbar */
static void view_set_fullscreen(struct view *v, bool on) {
	struct server *s = v->server;
	if (v->fullscreen == on)
		return;
	int sw, sh;
	server_screen(s, &sw, &sh);
	if (on) {
		v->pre_fullscreen = (struct wlr_box){v->x, v->y, v->cw, v->ch};
		v->fullscreen = true;
		v->x = v->y = 0;
		wlr_scene_node_reparent(&v->tree->node, s->layer_fullscreen);
		wlr_xdg_toplevel_set_size(v->xt, sw, sh);
	} else {
		v->fullscreen = false;
		v->x = v->pre_fullscreen.x;
		v->y = v->pre_fullscreen.y;
		wlr_scene_node_reparent(&v->tree->node, s->layer_views);
		if (v->maximized)
			wlr_xdg_toplevel_set_size(v->xt, sw, sh - taskbar_h(v->server) - caption_h(v));
		else
			wlr_xdg_toplevel_set_size(v->xt, v->pre_fullscreen.width, v->pre_fullscreen.height);
	}
	wlr_xdg_toplevel_set_fullscreen(v->xt, on);
	view_move_node(v);
	view_redraw(v);
}

void view_close(struct view *v) {
	wlr_xdg_toplevel_send_close(v->xt);
}

/* ---- optional Windows 7-style snapping (not an XP feature, so off by default) ---- */

enum { SNAP_NONE, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT };

static struct wlr_box snap_area(struct server *s, int zone) {
	int sw, sh;
	server_screen(s, &sw, &sh);
	int h = sh - taskbar_h(s);
	switch (zone) {
	case SNAP_LEFT: return (struct wlr_box){0, 0, sw / 2, h};
	case SNAP_RIGHT: return (struct wlr_box){sw - sw / 2, 0, sw / 2, h};
	default: return (struct wlr_box){0, 0, sw, h};
	}
}

static void snap_preview(struct server *s, int zone) {
	if (zone == s->snap_zone)
		return;
	s->snap_zone = zone;
	if (!zone) {
		if (s->snap_preview)
			wlr_scene_node_set_enabled(&s->snap_preview->node, false);
		return;
	}
	struct wlr_box a = snap_area(s, zone);
	/* XP-blue, see-through; sits above the wallpaper and below every window */
	const float color[4] = {0.19f * 0.4f, 0.42f * 0.4f, 0.77f * 0.4f, 0.4f};
	if (!s->snap_preview)
		s->snap_preview = wlr_scene_rect_create(s->layer_desktop, a.width, a.height, color);
	wlr_scene_rect_set_size(s->snap_preview, a.width, a.height);
	wlr_scene_node_set_position(&s->snap_preview->node, a.x, a.y);
	wlr_scene_node_raise_to_top(&s->snap_preview->node);
	wlr_scene_node_set_enabled(&s->snap_preview->node, true);
}

static void view_snap(struct view *v, int zone) {
	struct wlr_box a = snap_area(v->server, zone);
	if (!v->snapped)
		v->presnap = (struct wlr_box){v->x, v->y, v->cw, v->ch};
	v->snapped = true;
	v->x = a.x;
	v->y = a.y;
	wlr_xdg_toplevel_set_size(v->xt, a.width - 2 * side_w(v), a.height - caption_h(v) - bottom_h(v));
	view_move_node(v);
}

static enum wlr_xdg_toplevel_decoration_v1_mode deco_mode_for(struct view *v) {
	/* XP frames by default, but an app that insists on its own title bar keeps it */
	return v->decoration->requested_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE ?
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
}

/* Does the app want Expede to draw its frame? Yes if it asked through
 * xdg-decoration (Qt, foot) or KDE's protocol (GTK3) and didn't ask for
 * client-side; no for apps that never ask (GTK4, Chromium, Electron). */
static bool wants_server_frame(struct view *v) {
	if (v->decoration)
		return v->decoration->requested_mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
	struct wlr_server_decoration *d;
	wl_list_for_each(d, &v->server->kde_deco_mgr->decorations, link)
		if (d->surface == v->xt->base->surface)
			return d->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
	return false;
}

static void view_update_frame_kind(struct view *v) {
	bool csd = !wants_server_frame(v);
	if (csd == v->csd || !v->mapped)
		return;
	v->csd = csd;
	view_redraw(v);
}

static void on_map(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, map);
	struct wlr_box g;
	wlr_xdg_surface_get_geometry(v->xt->base, &g);
	v->cw = g.width;
	v->ch = g.height;
	v->csd = !wants_server_frame(v);
	/* cascade new windows like Explorer does */
	static int cascade;
	int sw, sh;
	server_screen(v->server, &sw, &sh);
	v->x = 40 + (cascade % 8) * 26;
	v->y = 30 + (cascade % 8) * 26;
	cascade++;
	if (v->x + v->cw > sw) v->x = 0;
	if (v->y + v->ch + caption_h(v) > sh - taskbar_h(v->server)) v->y = 0;
	view_move_node(v);
	v->mapped = true;
	wlr_scene_node_set_enabled(&v->tree->node, true);
	v->icon = icon_for_app(theme_of(v->server, v->server->st_icons), v->xt->app_id);
	wlr_log(WLR_DEBUG, "mapped \"%s\" (app_id %s)", v->xt->title ? v->xt->title : "", v->xt->app_id ? v->xt->app_id : "none");
	view_focus(v);
	/* apps can ask for fullscreen before their first frame (foot --fullscreen, kiosk modes) */
	if (v->xt->requested.fullscreen)
		view_set_fullscreen(v, true);
}

static void on_unmap(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, unmap);
	v->mapped = false;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	if (v->server->grab_view == v) {
		v->server->grab = GRAB_NONE;
		v->server->grab_view = NULL;
	}
	if (v->active) {
		v->active = false;
		focus_next_visible(v->server, v);
	}
	shell_views_changed(v->server->shell);
}

static void on_commit(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, commit);
	if (v->xt->base->initial_commit) {
		wlr_xdg_toplevel_set_size(v->xt, 0, 0);
		if (v->decoration)
			wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration, deco_mode_for(v));
		return;
	}
	struct wlr_box g;
	wlr_xdg_surface_get_geometry(v->xt->base, &g);
	if (v->mapped && (g.width != v->cw || g.height != v->ch)) {
		/* keep the right/bottom edge put when resizing from the left/top */
		if (v->server->grab == GRAB_RESIZE && v->server->grab_view == v) {
			if (v->server->resize_edges & WLR_EDGE_LEFT)
				v->x += v->cw - g.width;
			if (v->server->resize_edges & WLR_EDGE_TOP)
				v->y += v->ch - g.height;
			view_move_node(v);
		}
		v->cw = g.width;
		v->ch = g.height;
		view_redraw(v);
	}
	place_content(v);
}

static void on_destroy(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, destroy);
	if (v->decoration) {
		wl_list_remove(&v->deco_destroy.link);
		wl_list_remove(&v->deco_request_mode.link);
		v->decoration = NULL;
	}
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_minimize.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	wl_list_remove(&v->link);
	wlr_scene_node_destroy(&v->tree->node);
	struct server *s = v->server;
	free(v);
	shell_views_changed(s->shell);
}

static void begin_grab(struct view *v, enum grab_mode mode, uint32_t edges) {
	struct server *s = v->server;
	if (v->maximized && mode != GRAB_MOVE)
		return;
	s->grab = mode;
	s->grab_view = v;
	s->grab_x = s->cursor->x;
	s->grab_y = s->cursor->y;
	s->grab_box = (struct wlr_box){v->x, v->y, v->cw, v->ch};
	s->resize_edges = edges;
}

static void on_request_move(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, request_move);
	begin_grab(v, GRAB_MOVE, 0);
}

static void on_request_resize(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, request_resize);
	struct wlr_xdg_toplevel_resize_event *e = data;
	begin_grab(v, GRAB_RESIZE, e->edges);
}

static void on_request_maximize(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, request_maximize);
	if (v->xt->base->initialized)
		view_set_maximized(v, v->xt->requested.maximized);
}

static void on_request_fullscreen(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, request_fullscreen);
	if (v->xt->base->initialized)
		view_set_fullscreen(v, v->xt->requested.fullscreen);
}

static void on_request_minimize(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, request_minimize);
	view_set_minimized(v, true);
}

static void on_set_title(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, set_title);
	if (v->mapped)
		view_redraw(v);
	shell_views_changed(v->server->shell);
}

static void on_set_app_id(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, set_app_id);
	v->icon = icon_for_app(theme_of(v->server, v->server->st_icons), v->xt->app_id);
	if (v->mapped)
		view_redraw(v);
}

static void on_new_toplevel(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xt = data;
	struct view *v = calloc(1, sizeof(*v));
	v->server = s;
	v->xt = xt;
	v->tree = wlr_scene_tree_create(s->layer_views);
	v->tree->node.data = v;
	v->content = wlr_scene_xdg_surface_create(v->tree, xt->base);
	xt->base->data = v->content;
	wlr_scene_node_set_enabled(&v->tree->node, false);

	v->map.notify = on_map;
	wl_signal_add(&xt->base->surface->events.map, &v->map);
	v->unmap.notify = on_unmap;
	wl_signal_add(&xt->base->surface->events.unmap, &v->unmap);
	v->commit.notify = on_commit;
	wl_signal_add(&xt->base->surface->events.commit, &v->commit);
	v->destroy.notify = on_destroy;
	wl_signal_add(&xt->events.destroy, &v->destroy);
	v->request_move.notify = on_request_move;
	wl_signal_add(&xt->events.request_move, &v->request_move);
	v->request_resize.notify = on_request_resize;
	wl_signal_add(&xt->events.request_resize, &v->request_resize);
	v->request_maximize.notify = on_request_maximize;
	wl_signal_add(&xt->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = on_request_fullscreen;
	wl_signal_add(&xt->events.request_fullscreen, &v->request_fullscreen);
	v->request_minimize.notify = on_request_minimize;
	wl_signal_add(&xt->events.request_minimize, &v->request_minimize);
	v->set_title.notify = on_set_title;
	wl_signal_add(&xt->events.set_title, &v->set_title);
	v->set_app_id.notify = on_set_app_id;
	wl_signal_add(&xt->events.set_app_id, &v->set_app_id);
	wl_list_insert(s->views.prev, &v->link);
}

static void on_popup_commit(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *p = data;
	(void)l;
	(void)p;
}

struct popup {
	struct wlr_xdg_popup *xp;
	struct wl_listener commit, destroy;
};

static struct view *view_for_toplevel(struct server *s, struct wlr_xdg_toplevel *xt);

/* Keep menus on screen: tell wlroots the screen area in the coordinates of the
 * popup's toplevel, so a right-click near an edge flips or slides instead of
 * spilling off. */
static void popup_commit(struct wl_listener *l, void *data) {
	struct popup *p = wl_container_of(l, p, commit);
	if (!p->xp->base->initial_commit)
		return;
	struct wlr_xdg_surface *top = wlr_xdg_surface_try_from_wlr_surface(p->xp->parent);
	while (top && top->role == WLR_XDG_SURFACE_ROLE_POPUP)
		top = wlr_xdg_surface_try_from_wlr_surface(top->popup->parent);
	struct view *v = top && top->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL ? view_for_toplevel(the_server, top->toplevel) : NULL;
	if (v) {
		int sw, sh;
		server_screen(the_server, &sw, &sh);
		struct wlr_box g;
		wlr_xdg_surface_get_geometry(top, &g);
		struct wlr_box box = {-(v->x + side_w(v)) + g.x, -(v->y + caption_h(v)) + g.y, sw, sh};
		wlr_xdg_popup_unconstrain_from_box(p->xp, &box);
	}
	wlr_xdg_surface_schedule_configure(p->xp->base);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void on_new_popup(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *xp = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xp->parent);
	assert(parent);
	struct wlr_scene_tree *pt = parent->data;
	xp->base->data = wlr_scene_xdg_surface_create(pt, xp->base);
	struct popup *p = calloc(1, sizeof(*p));
	p->xp = xp;
	p->commit.notify = popup_commit;
	wl_signal_add(&xp->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xp->events.destroy, &p->destroy);
	(void)on_popup_commit;
}

/* ---- server-side decorations: Expede always draws the Luna frame ---- */

static struct view *view_for_toplevel(struct server *s, struct wlr_xdg_toplevel *xt) {
	struct view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->xt == xt)
			return v;
	return NULL;
}

static void deco_request_mode(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, deco_request_mode);
	if (v->xt->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration, deco_mode_for(v));
	view_update_frame_kind(v);
}

static void deco_destroy(struct wl_listener *l, void *data) {
	struct view *v = wl_container_of(l, v, deco_destroy);
	v->decoration = NULL;
	wl_list_remove(&v->deco_destroy.link);
	wl_list_remove(&v->deco_request_mode.link);
	view_update_frame_kind(v);
}

static void on_new_decoration(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct view *v = view_for_toplevel(s, d->toplevel);
	if (!v)
		return;
	v->decoration = d;
	v->deco_destroy.notify = deco_destroy;
	wl_signal_add(&d->events.destroy, &v->deco_destroy);
	v->deco_request_mode.notify = deco_request_mode;
	wl_signal_add(&d->events.request_mode, &v->deco_request_mode);
	if (v->xt->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(d, deco_mode_for(v));
	view_update_frame_kind(v);
}

/* ---------------------------------------------------------------- input */

static uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct view *view_at(struct server *s, double lx, double ly, struct wlr_surface **surf, double *sx, double *sy) {
	*surf = NULL;
	struct wlr_scene_node *n = wlr_scene_node_at(&s->layer_fullscreen->node, lx, ly, sx, sy);
	if (!n)
		n = wlr_scene_node_at(&s->layer_views->node, lx, ly, sx, sy);
	if (!n)
		return NULL;
	if (n->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
		if (ss)
			*surf = ss->surface;
	}
	struct wlr_scene_tree *t = n->parent;
	while (t && !t->node.data)
		t = t->node.parent;
	return t ? t->node.data : NULL;
}

static uint32_t border_edges(struct view *v, double lx, double ly) {
	if (v->maximized)
		return 0;
	double x = lx - v->x, y = ly - v->y;
	if (v->csd || v->fullscreen)
		return 0; /* the app's own frame handles its resize edges */
	int fw = view_frame_w(v), fh = v->ch + caption_h(v) + bottom_h(v);
	uint32_t e = 0;
	int bw = w7(v) ? W7_BORDER : BORDER;
	if (x < bw) e |= WLR_EDGE_LEFT;
	if (x >= fw - bw) e |= WLR_EDGE_RIGHT;
	if (y < (w7(v) ? 5 : BORDER)) e |= WLR_EDGE_TOP;
	if (y >= fh - bw) e |= WLR_EDGE_BOTTOM;
	return e;
}

static const char *edge_cursor(uint32_t e) {
	if (e == (WLR_EDGE_TOP | WLR_EDGE_LEFT) || e == (WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT))
		return "nwse-resize";
	if (e == (WLR_EDGE_TOP | WLR_EDGE_RIGHT) || e == (WLR_EDGE_BOTTOM | WLR_EDGE_LEFT))
		return "nesw-resize";
	if (e & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))
		return "ew-resize";
	if (e & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM))
		return "ns-resize";
	return "default";
}

static void set_hot(struct server *s, struct view *hv, enum part p) {
	struct view *v;
	wl_list_for_each(v, &s->views, link) {
		enum part want = v == hv ? p : PART_NONE;
		if (v->hot != want) {
			v->hot = want;
			if (v->mapped)
				view_redraw(v);
		}
	}
}

static void reset_idle(struct server *s);

/* ---- scene rendering, screenshots and Snipping Tool ---- */

static void render_node(cairo_t *cr, struct wlr_scene_node *n, int ox, int oy,
		const struct wlr_box *clip, struct wlr_scene_node *stop_before, bool *stopped) {
	if (*stopped)
		return;
	if (n == stop_before) {
		*stopped = true;
		return;
	}
	if (!n->enabled)
		return;
	int nx = ox + n->x, ny = oy + n->y;
	if (n->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *t = wlr_scene_tree_from_node(n);
		struct wlr_scene_node *c;
		wl_list_for_each(c, &t->children, link)
			render_node(cr, c, nx, ny, clip, stop_before, stopped);
		return;
	}
	if (n->type == WLR_SCENE_NODE_RECT) {
		struct wlr_scene_rect *r = wlr_scene_rect_from_node(n);
		if (nx + r->width <= clip->x || ny + r->height <= clip->y ||
				nx >= clip->x + clip->width || ny >= clip->y + clip->height)
			return;
		float a = r->color[3];
		if (a <= 0)
			return;
		cairo_save(cr);
		cairo_set_source_rgba(cr, r->color[0] / a, r->color[1] / a, r->color[2] / a, a);
		cairo_rectangle(cr, nx, ny, r->width, r->height);
		cairo_fill(cr);
		cairo_restore(cr);
		return;
	}
	if (n->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(n);
		if (!sb->buffer)
			return;
		int bw = sb->buffer->width, bh = sb->buffer->height;
		int dw = sb->dst_width > 0 ? sb->dst_width : bw;
		int dh = sb->dst_height > 0 ? sb->dst_height : bh;
		if (dw <= 0 || dh <= 0 || nx + dw <= clip->x || ny + dh <= clip->y ||
				nx >= clip->x + clip->width || ny >= clip->y + clip->height)
			return;
		struct wlr_buffer *locked = sb->buffer;
		void *data = NULL;
		uint32_t fmt = 0;
		size_t stride = 0;
		bool ptr_ok = wlr_buffer_begin_data_ptr_access(locked, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride);
		struct wlr_client_buffer *cb = ptr_ok ? NULL : wlr_client_buffer_get(sb->buffer);
		if (!ptr_ok && cb && cb->source) {
			locked = cb->source;
			ptr_ok = wlr_buffer_begin_data_ptr_access(locked, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &fmt, &stride);
		}
		unsigned char *alloc_px = NULL;
		if (!ptr_ok || (fmt != DRM_FORMAT_ARGB8888 && fmt != DRM_FORMAT_XRGB8888)) {
			if (ptr_ok) {
				wlr_buffer_end_data_ptr_access(locked);
				ptr_ok = false;
			}
			struct wlr_texture *tex = sb->texture ? sb->texture : (cb ? cb->texture : NULL);
			if (!tex)
				return;
			stride = (size_t)bw * 4;
			alloc_px = malloc(stride * bh);
			if (!alloc_px || !wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
					.data = alloc_px, .format = DRM_FORMAT_ARGB8888, .stride = stride })) {
				free(alloc_px);
				return;
			}
			data = alloc_px;
			fmt = DRM_FORMAT_ARGB8888;
		}
		cairo_format_t cfmt = fmt == DRM_FORMAT_XRGB8888 ? CAIRO_FORMAT_RGB24 : CAIRO_FORMAT_ARGB32;
		cairo_surface_t *img = cairo_image_surface_create_for_data(data, cfmt, bw, bh, stride);
		struct wlr_fbox src = sb->src_box;
		if (wlr_fbox_empty(&src))
			src = (struct wlr_fbox){0, 0, bw, bh};
		cairo_save(cr);
		cairo_rectangle(cr, nx, ny, dw, dh);
		cairo_clip(cr);
		cairo_translate(cr, nx, ny);
		cairo_scale(cr, dw / src.width, dh / src.height);
		cairo_set_source_surface(cr, img, -src.x, -src.y);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
		if (sb->opacity < 1.0f)
			cairo_paint_with_alpha(cr, sb->opacity);
		else
			cairo_paint(cr);
		cairo_restore(cr);
		cairo_surface_destroy(img);
		if (ptr_ok)
			wlr_buffer_end_data_ptr_access(locked);
		free(alloc_px);
	}
}

static cairo_surface_t *capture_box(struct wlr_scene_node *root, int rx, int ry, struct wlr_box box) {
	if (box.width <= 0 || box.height <= 0)
		return NULL;
	cairo_surface_t *s = canvas_new(box.width, box.height);
	cairo_t *cr = cairo_create(s);
	cairo_translate(cr, -box.x, -box.y);
	bool stopped = false;
	render_node(cr, root, rx, ry, &box, NULL, &stopped);
	cairo_destroy(cr);
	cairo_surface_set_device_scale(s, 1, 1);
	return s;
}

struct png_buf {
	unsigned char *data;
	size_t len, cap;
};

static cairo_status_t png_write_cb(void *closure, const unsigned char *data, unsigned int length) {
	struct png_buf *b = closure;
	if (b->len + length > b->cap) {
		b->cap = (b->len + length) * 2 + 4096;
		b->data = realloc(b->data, b->cap);
	}
	memcpy(b->data + b->len, data, length);
	b->len += length;
	return CAIRO_STATUS_SUCCESS;
}

struct clip_source {
	struct wlr_data_source base;
	unsigned char *data;
	size_t len;
};

static void clip_send(struct wlr_data_source *source, const char *mime_type, int32_t fd) {
	struct clip_source *cs = wl_container_of(source, cs, base);
	if (!strcmp(mime_type, "image/png")) {
		pid_t pid = fork();
		if (pid == 0) {
			if (fork() == 0) {
				size_t off = 0;
				while (off < cs->len) {
					ssize_t n = write(fd, cs->data + off, cs->len - off);
					if (n <= 0)
						break;
					off += n;
				}
				close(fd);
				_exit(0);
			}
			_exit(0);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}
	close(fd);
}

static void clip_destroy(struct wlr_data_source *source) {
	struct clip_source *cs = wl_container_of(source, cs, base);
	free(cs->data);
	free(cs);
}

static const struct wlr_data_source_impl clip_impl = {
	.send = clip_send,
	.destroy = clip_destroy,
};

static void save_screenshot(struct server *s, cairo_surface_t *img) {
	if (!img)
		return;
	struct png_buf pb = {0};
	cairo_surface_write_to_png_stream(img, png_write_cb, &pb);
	cairo_surface_destroy(img);
	if (!pb.data || !pb.len) {
		free(pb.data);
		return;
	}
	const char *home = getenv("HOME");
	if (home && *home) {
		char dir[512], path[640], ts[64];
		snprintf(dir, sizeof(dir), "%s/Pictures", home);
		mkdir(dir, 0755);
		snprintf(dir, sizeof(dir), "%s/Pictures/Screenshots", home);
		mkdir(dir, 0755);
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H-%M-%S", &tm);
		snprintf(path, sizeof(path), "%s/Screenshot %s.png", dir, ts);
		for (int n = 2; access(path, F_OK) == 0 && n < 100; n++)
			snprintf(path, sizeof(path), "%s/Screenshot %s (%d).png", dir, ts, n);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(pb.data, 1, pb.len, f);
			fclose(f);
			wlr_log(WLR_INFO, "saved screenshot to %s", path);
		}
	}
	struct clip_source *cs = calloc(1, sizeof(*cs));
	wlr_data_source_init(&cs->base, &clip_impl);
	char **mime = wl_array_add(&cs->base.mime_types, sizeof(*mime));
	*mime = strdup("image/png");
	cs->data = pb.data;
	cs->len = pb.len;
	wlr_seat_set_selection(s->seat, &cs->base, wl_display_next_serial(s->display));
}

static void screenshot_full(struct server *s) {
	int sw, sh;
	server_screen(s, &sw, &sh);
	save_screenshot(s, capture_box(&s->scene->tree.node, 0, 0, (struct wlr_box){0, 0, sw, sh}));
}

static void screenshot_window(struct server *s) {
	struct view *v = focused_view(s);
	if (!v || !v->mapped || v->minimized) {
		screenshot_full(s);
		return;
	}
	int fw = view_frame_w(v), fh = caption_h(v) + v->ch + bottom_h(v);
	save_screenshot(s, capture_box(&v->tree->node, 0, 0, (struct wlr_box){0, 0, fw, fh}));
}

static void snip_redraw(struct server *s) {
	int sw, sh;
	server_screen(s, &sw, &sh);
	cairo_surface_t *c = canvas_new(sw, sh);
	cairo_t *cr = cairo_create(c);
	/* Windows 7 Snipping Tool veil: milky white wash over the desktop */
	cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
	cairo_paint(cr);
	if (s->snip_dragging && s->grab_box.width > 0 && s->grab_box.height > 0) {
		struct wlr_box b = s->grab_box;
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
		cairo_rectangle(cr, b.x, b.y, b.width, b.height);
		cairo_fill(cr);
		cairo_restore(cr);
		cairo_set_line_width(cr, 1);
		cairo_set_source_rgb(cr, 0.85, 0.08, 0.08);
		cairo_rectangle(cr, b.x + 0.5, b.y + 0.5, b.width - 1, b.height - 1);
		cairo_stroke(cr);
	} else {
		const char *msg = "Drag the cursor around the area you want to capture (Esc to cancel)";
		struct theme *t = theme_of(s, s->style);
		theme_use_fonts(t);
		int tw = text_width("Tahoma 9", msg);
		int bw = tw + 24, bh = 28, bx = (sw - bw) / 2, by = 18;
		round_path(cr, bx + 0.5, by + 0.5, bw - 1, bh - 1, 4, 4);
		cairo_set_source_rgba(cr, 1, 1, 0.92, 0.95);
		cairo_fill_preserve(cr);
		cairo_set_line_width(cr, 1);
		cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);
		cairo_stroke(cr);
		draw_text(cr, "Tahoma 9", msg, bx + 12, by + 6, 0, 0x000000, 0, 0, 0, true);
	}
	cairo_destroy(cr);
	scene_show(&s->snip_overlay, s->layer_overlay, c, 0, 0);
}

static void snip_end(struct server *s) {
	s->grab = GRAB_NONE;
	s->snip_dragging = false;
	if (s->snip_overlay)
		wlr_scene_node_set_enabled(&s->snip_overlay->node, false);
	wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
}

static void snip_begin(struct server *s) {
	if (!s || (s->session && session_active(s->session)))
		return;
	if (s->shell && shell_start_open(s->shell))
		shell_toggle_start(s->shell);
	s->grab = GRAB_SNIP;
	s->snip_dragging = false;
	s->grab_box = (struct wlr_box){0};
	snip_redraw(s);
	wlr_seat_pointer_clear_focus(s->seat);
	wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "crosshair");
}

static void process_motion(struct server *s, uint32_t time) {
	reset_idle(s);
	double lx = s->cursor->x, ly = s->cursor->y;
	if (s->session && session_active(s->session)) {
		s->grab = GRAB_NONE;
		wlr_seat_pointer_clear_focus(s->seat);
		session_motion(s->session, lx, ly);
		return;
	}
	if (s->grab == GRAB_SNIP) {
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "crosshair");
		if (s->snip_dragging) {
			int sw, sh;
			server_screen(s, &sw, &sh);
			int x0 = (int)(s->grab_x < lx ? s->grab_x : lx);
			int y0 = (int)(s->grab_y < ly ? s->grab_y : ly);
			int x1 = (int)(s->grab_x > lx ? s->grab_x : lx);
			int y1 = (int)(s->grab_y > ly ? s->grab_y : ly);
			if (x0 < 0) x0 = 0;
			if (y0 < 0) y0 = 0;
			if (x1 > sw) x1 = sw;
			if (y1 > sh) y1 = sh;
			s->grab_box = (struct wlr_box){x0, y0, x1 - x0, y1 - y0};
			snip_redraw(s);
		}
		return;
	}
	if (s->grab == GRAB_MOVE) {
		struct view *v = s->grab_view;
		if (v->maximized) {
			/* dragging a maximized caption restores it under the pointer, like XP */
			double rel = (lx - v->x) / (double)view_frame_w(v);
			view_set_maximized(v, false);
			s->grab_box.x = lx - rel * view_frame_w(v);
			s->grab_box.y = 0;
			s->grab_x = lx;
			s->grab_y = ly;
		}
		if (v->snapped) {
			/* pulling a snapped window away gives it back its old size */
			double rel = (lx - v->x) / (double)view_frame_w(v);
			v->snapped = false;
			wlr_xdg_toplevel_set_size(v->xt, v->presnap.width, v->presnap.height);
			s->grab_box.x = lx - rel * (v->presnap.width + 2 * side_w(v));
			s->grab_box.y = v->y;
			s->grab_x = lx;
			s->grab_y = ly;
		}
		v->x = s->grab_box.x + (lx - s->grab_x);
		v->y = s->grab_box.y + (ly - s->grab_y);
		view_move_node(v);
		if (s->opt_snap) {
			int sw, sh;
			server_screen(s, &sw, &sh);
			snap_preview(s, ly <= 1 ? SNAP_MAX : lx <= 1 ? SNAP_LEFT : lx >= sw - 2 ? SNAP_RIGHT : SNAP_NONE);
		}
		return;
	}
	if (s->grab == GRAB_RESIZE) {
		struct view *v = s->grab_view;
		struct wlr_box b = s->grab_box;
		int dx = lx - s->grab_x, dy = ly - s->grab_y;
		int w = b.width, h = b.height;
		if (s->resize_edges & WLR_EDGE_RIGHT) w += dx;
		if (s->resize_edges & WLR_EDGE_LEFT) w -= dx;
		if (s->resize_edges & WLR_EDGE_BOTTOM) h += dy;
		if (s->resize_edges & WLR_EDGE_TOP) h -= dy;
		if (w < 120) w = 120;
		if (h < 40) h = 40;
		wlr_xdg_toplevel_set_size(v->xt, w, h);
		return;
	}
	if (s->grab == GRAB_BUTTON) {
		struct view *v = s->grab_view;
		enum part p = button_at(v, lx, ly);
		set_hot(s, v, p == v->pressed ? p : PART_NONE);
		return;
	}
	struct wlr_surface *surf;
	double sx, sy;
	struct view *v = view_at(s, lx, ly, &surf, &sx, &sy);
	/* a fullscreen window covers the taskbar, so the shell only gets the pointer elsewhere */
	if (!(v && v->fullscreen) && shell_motion(s->shell, lx, ly, v != NULL)) {
		set_hot(s, NULL, PART_NONE);
		wlr_seat_pointer_clear_focus(s->seat);
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
		return;
	}
	set_hot(s, v, v && !surf ? button_at(v, lx, ly) : PART_NONE);
	if (!surf)
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, edge_cursor(v ? border_edges(v, lx, ly) : 0));
	if (surf) {
		/* back to the arrow when crossing into an app; it sets its own pointer if it wants one
		 * (otherwise a resize arrow from the frame edge would stick) */
		if (s->seat->pointer_state.focused_surface != surf)
			wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
		wlr_seat_pointer_notify_enter(s->seat, surf, sx, sy);
		wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(s->seat);
	}
}

static void on_cursor_motion(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, cursor_motion);
	struct wlr_pointer_motion_event *e = data;
	wlr_cursor_move(s->cursor, &e->pointer->base, e->delta_x, e->delta_y);
	process_motion(s, e->time_msec);
}

static void on_cursor_motion_abs(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, cursor_motion_abs);
	struct wlr_pointer_motion_absolute_event *e = data;
	wlr_cursor_warp_absolute(s->cursor, &e->pointer->base, e->x, e->y);
	process_motion(s, e->time_msec);
}

static void on_cursor_button(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, cursor_button);
	struct wlr_pointer_button_event *e = data;
	double lx = s->cursor->x, ly = s->cursor->y;
	bool pressed = e->state == WL_POINTER_BUTTON_STATE_PRESSED;

	if (s->grab == GRAB_SNIP) {
		if (e->button == BTN_RIGHT && pressed) {
			snip_end(s);
		} else if (e->button == BTN_LEFT && pressed) {
			s->snip_dragging = true;
			s->grab_x = lx;
			s->grab_y = ly;
			s->grab_box = (struct wlr_box){(int)lx, (int)ly, 0, 0};
			snip_redraw(s);
		} else if (e->button == BTN_LEFT && !pressed && s->snip_dragging) {
			struct wlr_box b = s->grab_box;
			snip_end(s);
			if (b.width > 1 && b.height > 1)
				save_screenshot(s, capture_box(&s->scene->tree.node, 0, 0, b));
		}
		return;
	}

	if (!pressed && s->grab != GRAB_NONE) {
		if (s->grab == GRAB_BUTTON) {
			struct view *v = s->grab_view;
			enum part p = v->pressed;
			v->pressed = PART_NONE;
			bool inside = button_at(v, lx, ly) == p;
			view_redraw(v);
			if (inside && p == PART_CLOSE)
				view_close(v);
			else if (inside && p == PART_MAX)
				view_set_maximized(v, !v->maximized);
			else if (inside && p == PART_MIN)
				view_set_minimized(v, true);
		}
		if (s->grab == GRAB_MOVE && s->snap_zone) {
			if (s->snap_zone == SNAP_MAX)
				view_set_maximized(s->grab_view, true);
			else
				view_snap(s->grab_view, s->snap_zone);
			snap_preview(s, SNAP_NONE);
		}
		s->grab = GRAB_NONE;
		s->grab_view = NULL;
		wlr_seat_pointer_notify_button(s->seat, e->time_msec, e->button, e->state);
		return;
	}

	if (s->session && session_active(s->session)) {
		if (e->button == BTN_LEFT)
			session_button(s->session, lx, ly, pressed);
		return;
	}

	bool dbl = false;
	if (pressed && e->button == BTN_LEFT) {
		uint32_t t = now_ms();
		dbl = t - s->last_click_ms < 400 && abs((int)(lx - s->last_click_x)) < 4 && abs((int)(ly - s->last_click_y)) < 4;
		s->last_click_ms = dbl ? 0 : t;
		s->last_click_x = lx;
		s->last_click_y = ly;
	}

	struct wlr_surface *surf;
	double sx, sy;
	struct view *v = view_at(s, lx, ly, &surf, &sx, &sy);
	if (!(v && v->fullscreen) && shell_button(s->shell, lx, ly, pressed, dbl, v != NULL))
		return;

	if (pressed && v) {
		view_focus(v);
		if (!surf && e->button == BTN_LEFT) {
			enum part p = button_at(v, lx, ly);
			uint32_t edges = border_edges(v, lx, ly);
			if (p != PART_NONE) {
				v->pressed = p;
				v->hot = p;
				view_redraw(v);
				s->grab = GRAB_BUTTON;
				s->grab_view = v;
			} else if (dbl && !edges) {
				view_set_maximized(v, !v->maximized);
			} else if (edges) {
				begin_grab(v, GRAB_RESIZE, edges);
			} else {
				begin_grab(v, GRAB_MOVE, 0);
			}
			return;
		}
	} else if (pressed && !v) {
		view_focus(NULL);
	}
	wlr_seat_pointer_notify_button(s->seat, e->time_msec, e->button, e->state);
}

static void on_cursor_axis(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, cursor_axis);
	struct wlr_pointer_axis_event *e = data;
	if (e->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
			shell_axis(s->shell, s->cursor->x, s->cursor->y, e->delta))
		return;
	wlr_seat_pointer_notify_axis(s->seat, e->time_msec, e->orientation, e->delta,
		e->delta_discrete, e->source, e->relative_direction);
}

static void on_cursor_frame(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, cursor_frame);
	wlr_seat_pointer_notify_frame(s->seat);
}

static void cycle_windows(struct server *s) {
	/* Alt+Tab: bring the bottom-most window to the front */
	struct wlr_scene_node *n;
	wl_list_for_each(n, &s->layer_views->children, link) {
		struct view *v = n->data;
		if (v && v->mapped && !v->active) {
			view_focus(v);
			return;
		}
	}
}

static bool handle_binding(struct server *s, xkb_keysym_t sym, uint32_t mods) {
	if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT) && sym == XKB_KEY_BackSpace) {
		wl_display_terminate(s->display);
		return true;
	}
	if (s->grab == GRAB_SNIP && sym == XKB_KEY_Escape) {
		snip_end(s);
		return true;
	}
	if (sym == XKB_KEY_Print || sym == XKB_KEY_Sys_Req) {
		if (mods & WLR_MODIFIER_SHIFT)
			snip_begin(s);
		else if (mods & WLR_MODIFIER_ALT)
			screenshot_window(s);
		else
			screenshot_full(s);
		return true;
	}
	if ((mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT)) == (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT) &&
			(sym == XKB_KEY_s || sym == XKB_KEY_S)) {
		snip_begin(s);
		return true;
	}
	if (mods & WLR_MODIFIER_ALT) {
		struct view *v = focused_view(s);
		switch (sym) {
		case XKB_KEY_F4:
			if (v)
				view_close(v);
			return true;
		case XKB_KEY_Tab:
			cycle_windows(s);
			return true;
		}
	}
	/* laptop volume keys */
	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume: shell_volume_key(s->shell, 5, false); return true;
	case XKB_KEY_XF86AudioLowerVolume: shell_volume_key(s->shell, -5, false); return true;
	case XKB_KEY_XF86AudioMute: shell_volume_key(s->shell, 0, true); return true;
	/* laptop brightness keys; brightnessctl goes through logind, so no root needed */
	case XKB_KEY_XF86MonBrightnessUp: spawn("brightnessctl -q set 5%+"); return true;
	case XKB_KEY_XF86MonBrightnessDown: spawn("brightnessctl -q set 5%-"); return true;
	}
	if (sym == XKB_KEY_Escape && shell_start_open(s->shell)) {
		shell_toggle_start(s->shell);
		return true;
	}
	if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
		shell_toggle_start(s->shell);
		return true;
	}
	if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_r) {
		spawn("foot");
		return true;
	}
	if ((mods & WLR_MODIFIER_LOGO) && sym == XKB_KEY_e) {
		spawn("expede-explorer");
		return true;
	}
	return false;
}

static void kb_modifiers(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static void kb_key(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, key);
	struct server *s = k->server;
	struct wlr_keyboard_key_event *e = data;
	reset_idle(s);
	uint32_t keycode = e->keycode + 8;
	const xkb_keysym_t *syms;
	int n = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
	uint32_t mods = wlr_keyboard_get_modifiers(k->wlr_keyboard);
	bool handled = false;
	if (s->session && session_active(s->session)) {
		/* the Welcome screen and Turn Off dialog take the keyboard; Ctrl+Alt+Backspace still quits */
		if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED)
			for (int i = 0; i < n; i++) {
				if ((mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT) &&
						syms[i] == XKB_KEY_BackSpace)
					wl_display_terminate(s->display);
				session_key(s->session, syms[i]);
			}
		return;
	}
	if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED && shell_start_open(s->shell)) {
		char utf8[16] = "";
		xkb_state_key_get_utf8(k->wlr_keyboard->xkb_state, keycode, utf8, sizeof(utf8));
		for (int i = 0; i < n; i++)
			if (shell_key(s->shell, syms[i], utf8))
				return;
	}
	if (e->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		for (int i = 0; i < n; i++)
			handled |= handle_binding(s, syms[i], mods);
	if (!handled) {
		wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
		wlr_seat_keyboard_notify_key(s->seat, e->time_msec, e->keycode, e->state);
	}
}

static void kb_destroy(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

/* touchpad and mouse options; called for new devices and when settings change */
static void configure_pointer(struct server *s, struct wlr_input_device *dev) {
	if (!wlr_input_device_is_libinput(dev))
		return;
	struct libinput_device *li = wlr_libinput_get_device_handle(dev);
	bool touchpad = libinput_device_config_tap_get_finger_count(li) > 0;
	if (touchpad)
		libinput_device_config_tap_set_enabled(li, s->opt_tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	/* natural scrolling is for touchpads only; a wheel always scrolls the usual way */
	if (libinput_device_config_scroll_has_natural_scroll(li))
		libinput_device_config_scroll_set_natural_scroll_enabled(li, touchpad && s->opt_natural_scroll);
	if (libinput_device_config_accel_is_available(li))
		libinput_device_config_accel_set_speed(li, s->opt_pointer_speed / 100.0);
}

static void pointer_destroy(struct wl_listener *l, void *data) {
	struct pointer_dev *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

/* keyboard_layout = "gb" or "gb(extd)"; empty means the system default (XKB_DEFAULT_*) */
static void apply_keymap(struct server *s, struct wlr_keyboard *kb) {
	char layout[64] = "", variant[64] = "";
	if (s->opt_keyboard[0]) {
		snprintf(layout, sizeof(layout), "%s", s->opt_keyboard);
		char *paren = strchr(layout, '(');
		if (paren) {
			*paren = 0;
			snprintf(variant, sizeof(variant), "%s", paren + 1);
			variant[strcspn(variant, ")")] = 0;
		}
	}
	struct xkb_rule_names names = {.layout = layout[0] ? layout : NULL, .variant = variant[0] ? variant : NULL};
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		wlr_log(WLR_ERROR, "unknown keyboard layout \"%s\", keeping the default", s->opt_keyboard);
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(kb, km);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
}

static void on_new_input(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;
	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD) {
		struct keyboard *k = calloc(1, sizeof(*k));
		k->server = s;
		k->wlr_keyboard = wlr_keyboard_from_input_device(dev);
		apply_keymap(s, k->wlr_keyboard);
		wlr_keyboard_set_repeat_info(k->wlr_keyboard, 25, 600);
		k->modifiers.notify = kb_modifiers;
		wl_signal_add(&k->wlr_keyboard->events.modifiers, &k->modifiers);
		k->key.notify = kb_key;
		wl_signal_add(&k->wlr_keyboard->events.key, &k->key);
		k->destroy.notify = kb_destroy;
		wl_signal_add(&dev->events.destroy, &k->destroy);
		wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
		wl_list_insert(&s->keyboards, &k->link);
	} else if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		wlr_cursor_attach_input_device(s->cursor, dev);
		struct pointer_dev *p = calloc(1, sizeof(*p));
		p->server = s;
		p->dev = dev;
		p->destroy.notify = pointer_destroy;
		wl_signal_add(&dev->events.destroy, &p->destroy);
		wl_list_insert(&s->pointers, &p->link);
		configure_pointer(s, dev);
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

static void on_request_cursor(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *e = data;
	if (s->seat->pointer_state.focused_client == e->seat_client)
		wlr_cursor_set_surface(s->cursor, e->surface, e->hotspot_x, e->hotspot_y);
}

static void on_request_set_primary(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, request_set_primary);
	struct wlr_seat_request_set_primary_selection_event *e = data;
	wlr_seat_set_primary_selection(s->seat, e->source, e->serial);
}

/* an app asking to be brought forward (a link opened in an existing browser) */
static void on_request_activate(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *e = data;
	struct wlr_xdg_surface *xs = wlr_xdg_surface_try_from_wlr_surface(e->surface);
	if (!xs || xs->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;
	struct view *v = view_for_toplevel(s, xs->toplevel);
	if (v && v->mapped)
		view_focus(v);
}

/* apps that name a pointer shape (Chromium, GTK4) get XP's pointer for it */
static void on_request_cursor_shape(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, request_cursor_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *e = data;
	if (s->seat->pointer_state.focused_client == e->seat_client)
		wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, wlr_cursor_shape_v1_name(e->shape));
}

static void on_request_set_selection(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, request_set_selection);
	struct wlr_seat_request_set_selection_event *e = data;
	wlr_seat_set_selection(s->seat, e->source, e->serial);
}

/* ---------------------------------------------------------------- outputs */

static void output_frame(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(o->server->scene, o->wlr_output);
	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *e = data;
	wlr_output_commit_state(o->wlr_output, e->state);
	shell_layout(o->server->shell);
	if (o->server->session)
		session_layout(o->server->session);
}

static void write_outputs_status(struct server *s);

static void output_destroy(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);
	struct server *s = o->server;
	free(o);
	write_outputs_status(s);
}

/* Like Windows: 96 dpi is 100%, and a sharper screen gets the next step down from
 * its dpi/96 (a 224 dpi 2560x1600 laptop panel gets 200%). */
static double pick_scale(struct server *s, struct wlr_output *wo, int width_px) {
	if (s->opt_scale >= 0.5)
		return s->opt_scale;
	if (wo->phys_width <= 0 || width_px <= 0)
		return 1.0;
	double ratio = width_px / (wo->phys_width / 25.4) / 96.0;
	static const double steps[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
	double pick = 1.0;
	for (size_t i = 0; i < sizeof(steps) / sizeof(*steps); i++)
		if (steps[i] <= ratio + 0.05)
			pick = steps[i];
	return pick;
}

static struct output_conf *output_conf(struct server *s, const char *name) {
	for (int i = 0; i < s->n_out_conf; i++)
		if (!strcmp(s->out_conf[i].name, name))
			return &s->out_conf[i];
	return NULL;
}

/* the mode the Control Panel picked for this screen, else the screen's own preference */
static struct wlr_output_mode *wanted_mode(struct server *s, struct wlr_output *wo) {
	struct output_conf *c = output_conf(s, wo->name);
	if (c && c->w > 0) {
		struct wlr_output_mode *m, *best = NULL;
		wl_list_for_each(m, &wo->modes, link)
			if (m->width == c->w && m->height == c->h &&
					(!best || abs(m->refresh - c->refresh_mhz) < abs(best->refresh - c->refresh_mhz)))
				best = m;
		if (best)
			return best;
	}
	return wlr_output_preferred_mode(wo);
}

/* ~/.cache/expede/outputs: one line per screen for the Control Panel's Settings tab:
 * name, description, current WxH, refresh (mHz), scale, enabled, then every mode */
static void write_outputs_status(struct server *s) {
	char dir[512], path[600];
	const char *xdg = getenv("XDG_CACHE_HOME");
	if (xdg && *xdg)
		snprintf(dir, sizeof(dir), "%s/expede", xdg);
	else
		snprintf(dir, sizeof(dir), "%s/.cache/expede", getenv("HOME"));
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/outputs", dir);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	struct output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct wlr_output *wo = o->wlr_output;
		fprintf(f, "%s\t%s %s\t%dx%d\t%d\t%.2f\t%d\t", wo->name, wo->make ? wo->make : "", wo->model ? wo->model : "",
			wo->width, wo->height, wo->refresh, wo->scale, wo->enabled);
		struct wlr_output_mode *m;
		bool first = true;
		wl_list_for_each(m, &wo->modes, link) {
			fprintf(f, "%s%dx%d@%d", first ? "" : ",", m->width, m->height, m->refresh);
			first = false;
		}
		fputc('\n', f);
	}
	fclose(f);
}

static void on_new_output(struct wl_listener *l, void *data) {
	struct server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wo = data;
	wlr_output_init_render(wo, s->allocator, s->renderer);
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	struct wlr_output_mode *mode = wanted_mode(s, wo);
	if (mode)
		wlr_output_state_set_mode(&st, mode);
	struct output_conf *oc = output_conf(s, wo->name);
	if (oc && oc->disabled && !wl_list_empty(&s->outputs))
		wlr_output_state_set_enabled(&st, false); /* never switch off the only screen */
	double scale = pick_scale(s, wo, mode ? mode->width : wo->width);
	wlr_output_state_set_scale(&st, scale);
	ui_scale = scale;
	wlr_log(WLR_INFO, "%s: scale %.0f%%", wo->name, scale * 100);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);

	struct output *o = calloc(1, sizeof(*o));
	o->server = s;
	o->wlr_output = wo;
	o->frame.notify = output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);
	wl_list_insert(&s->outputs, &o->link);

	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
	write_outputs_status(s);
	struct wlr_scene_output *so = wlr_scene_output_create(s->scene, wo);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	if (s->shell)
		shell_layout(s->shell);
	if (s->session)
		session_layout(s->session);
}

/* ---------------------------------------------------------------- main */

static const char *default_assets(void) {
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

/* ~/.config/expede/expede.conf, "key = value" lines:
 *   snap = true            drag to the top to maximise, to a side for half the screen
 *   tap_to_click = false   touchpad taps don't click (on by default)
 *   scale = 150            screen scale in percent; "auto" (default) goes by the screen's DPI
 *   natural_scroll = false touchpad content follows your fingers (on by default)
 *   pointer_speed = 30     -100 (slow) to 100 (fast); 0 is libinput's default
 *   wallpaper = Tulips.jpg a file in <assets>/wallpapers, a full path, or none
 *   wallpaper_style = auto stretch, center or tile ("auto" tiles the small patterns)
 *   user_picture = duck    one of XP's account pictures
 *   sounds = false         no system sounds
 *   clock = 24             24-hour clock in the tray
 *   keyboard_layout = gb   any XKB layout, optionally "gb(extd)" style variant
 *   screen_off = 10        minutes idle before the screens switch off (0 = never)
 *   output.HDMI-A-1.mode = 1920x1080@60     output.eDP-1.enabled = false */
/* "xp", "7" or "auto" (follow the main style) */
static int conf_style(const char *v) {
	if (!strcasecmp(v, "7") || !strcasecmp(v, "win7") || !strcasecmp(v, "windows7"))
		return 7;
	if (!strcasecmp(v, "xp") || !strcasecmp(v, "classic"))
		return 0;
	return -1;
}

/* Windows 7's window colours (Personalization > Window Color) */
static const struct { const char *name; uint32_t rgb; } glass_colors[] = {
	{"sky", 0x74b8fc}, {"twilight", 0x3a6fd8}, {"sea", 0x3fb6c9}, {"leaf", 0x5fc35f}, {"lime", 0xb3d736},
	{"sun", 0xf6d33c}, {"pumpkin", 0xf89e38}, {"blush", 0xef6a8e}, {"violet", 0xc160d3}, {"lavender", 0xa69bd9},
	{"taupe", 0xa89f8a}, {"chocolate", 0x7a5737}, {"slate", 0x6f7b88}, {"frost", 0xe4eaf0}, {"ruby", 0xd02f3b},
};

static uint32_t conf_color(const char *v) {
	for (size_t i = 0; i < sizeof(glass_colors) / sizeof(glass_colors[0]); i++)
		if (!strcasecmp(v, glass_colors[i].name))
			return glass_colors[i].rgb;
	return (uint32_t)strtoul(v[0] == '#' ? v + 1 : v, NULL, 16) & 0xffffff;
}

static bool conf_true(const char *v) {
	return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1");
}

static void resolve_styles(struct server *s, bool wall_set) {
	int *parts[] = {&s->st_frames, &s->st_taskbar, &s->st_start, &s->st_logon, &s->st_sounds, &s->st_cursors,
		&s->st_icons};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		if (*parts[i] < 0 || (*parts[i] == 7 && !s->have7) || (*parts[i] == 0 && !s->have_xp))
			*parts[i] = s->style;
	/* each version opens on its own default wallpaper */
	if (!wall_set)
		snprintf(s->opt_wallpaper, sizeof(s->opt_wallpaper), "%s", s->style == 7 ? "Windows 7.jpg" : "Bliss.jpg");
}

static void load_config(struct server *s) {
	s->opt_tap = true;
	s->opt_natural_scroll = true;
	s->opt_modern_start = false;
	s->opt_pointer_speed = 0;
	s->opt_scale = 0;
	s->opt_sounds = true;
	s->opt_clock24 = false;
	snprintf(s->opt_wallpaper, sizeof(s->opt_wallpaper), "Bliss.jpg");
	snprintf(s->opt_wallpaper_style, sizeof(s->opt_wallpaper_style), "auto");
	s->opt_user_picture[0] = 0;
	s->opt_keyboard[0] = 0;
	s->opt_screen_off = 10;
	s->n_out_conf = 0;
	/* Windows version: XP unless only Windows 7 has been ripped */
	s->style = s->have_xp ? 0 : 7;
	s->st_frames = s->st_taskbar = s->st_start = s->st_logon = s->st_sounds = s->st_cursors = s->st_icons = -1;
	s->opt_glass = true;
	s->glass_rgb = 0x74b8fc;
	s->glass_intensity = 45;
	bool wall_set = false;
	char path[512];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/expede/expede.conf", xdg);
	else
		snprintf(path, sizeof(path), "%s/.config/expede/expede.conf", getenv("HOME"));
	FILE *f = fopen(path, "r");
	if (!f) {
		resolve_styles(s, wall_set);
		return;
	}
	char line[512], key[64], val[256];
	while (fgets(line, sizeof(line), f)) {
		/* key = value, where the value runs to the end of the line (wallpaper names have spaces) */
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
		if (!strcmp(key, "snap"))
			s->opt_snap = conf_true(val);
		else if (!strcmp(key, "tap_to_click"))
			s->opt_tap = conf_true(val);
		/* "start_menu = modern" (the old hybrid mode) is retired: its search lives on
		 * in the Windows 7 Start menu */
		else if (!strcmp(key, "natural_scroll"))
			s->opt_natural_scroll = conf_true(val);
		else if (!strcmp(key, "pointer_speed"))
			s->opt_pointer_speed = atoi(val);
		else if (!strcmp(key, "scale"))
			s->opt_scale = !strcasecmp(val, "auto") ? 0 : atof(val) / 100.0; /* "150" = 150% */
		else if (!strcmp(key, "sounds"))
			s->opt_sounds = conf_true(val);
		else if (!strcmp(key, "clock"))
			s->opt_clock24 = !strcmp(val, "24");
		else if (!strcmp(key, "wallpaper")) {
			snprintf(s->opt_wallpaper, sizeof(s->opt_wallpaper), "%s", !strcasecmp(val, "none") ? "" : val);
			wall_set = true;
		}
		else if (!strcmp(key, "style"))
			s->style = conf_style(val) == 7 && s->have7 ? 7 : s->have_xp ? 0 : 7;
		else if (!strcmp(key, "glass"))
			s->opt_glass = conf_true(val);
		else if (!strcmp(key, "glass_color"))
			s->glass_rgb = conf_color(val);
		else if (!strcmp(key, "glass_intensity"))
			s->glass_intensity = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
		else if (!strcmp(key, "frames"))
			s->st_frames = conf_style(val);
		else if (!strcmp(key, "taskbar"))
			s->st_taskbar = conf_style(val);
		else if (!strcmp(key, "start_menu")) {
			s->st_start = conf_style(val);
			s->opt_modern_start = !strcasecmp(val, "modern") || !strcasecmp(val, "search") || s->st_start == 7;
		}
		else if (!strcmp(key, "logon"))
			s->st_logon = conf_style(val);
		else if (!strcmp(key, "sound_scheme"))
			s->st_sounds = conf_style(val);
		else if (!strcmp(key, "pointers"))
			s->st_cursors = conf_style(val);
		else if (!strcmp(key, "icons"))
			s->st_icons = conf_style(val);
		else if (!strcmp(key, "wallpaper_style"))
			snprintf(s->opt_wallpaper_style, sizeof(s->opt_wallpaper_style), "%s", val);
		else if (!strcmp(key, "user_picture"))
			snprintf(s->opt_user_picture, sizeof(s->opt_user_picture), "%s", val);
		else if (!strcmp(key, "screen_off"))
			s->opt_screen_off = atoi(val);
		else if (!strncmp(key, "output.", 7) && strrchr(key, '.') > key + 7 && s->n_out_conf < 8) {
			/* output.<name>.mode = 1920x1080@60   output.<name>.enabled = false */
			char name[32];
			const char *dot = strrchr(key, '.');
			snprintf(name, sizeof(name), "%.*s", (int)(dot - key - 7), key + 7);
			struct output_conf *c = output_conf(s, name);
			if (!c) {
				c = &s->out_conf[s->n_out_conf++];
				memset(c, 0, sizeof(*c));
				snprintf(c->name, sizeof(c->name), "%s", name);
			}
			if (!strcmp(dot, ".mode")) {
				double hz = 0;
				sscanf(val, "%dx%d@%lf", &c->w, &c->h, &hz);
				c->refresh_mhz = (int)(hz * 1000 + 0.5);
			} else if (!strcmp(dot, ".enabled")) {
				c->disabled = !conf_true(val);
			}
		}
		else if (!strcmp(key, "keyboard_layout"))
			snprintf(s->opt_keyboard, sizeof(s->opt_keyboard), "%s", val);
	}
	fclose(f);
	resolve_styles(s, wall_set);
}

/* The desktop follows the icon style; a wallpaper or picture named in the other
 * version's folder is found there too. */
static void apply_desktop(struct server *s) {
	struct theme *t = theme_of(s, s->st_icons), *other = t == &s->theme ? &s->theme7 : &s->theme;
	char path[1024];
	const char *wall = s->opt_wallpaper;
	if (*wall && wall[0] != '/') {
		snprintf(path, sizeof(path), "%s/wallpapers/%s", t->dir, wall);
		if (access(path, R_OK) != 0 && other->dir[0]) {
			snprintf(path, sizeof(path), "%s/wallpapers/%s", other->dir, wall);
			if (access(path, R_OK) == 0)
				wall = path;
		}
	}
	theme_set_wallpaper(t, wall, s->opt_wallpaper_style);
	/* the account picture may be one of either version's; both Start menus show it */
	struct theme *pt = t;
	if (s->opt_user_picture[0] && other->dir[0]) {
		snprintf(path, sizeof(path), "%s/pictures/%s.png", t->dir, s->opt_user_picture);
		if (access(path, R_OK) != 0)
			pt = other;
	}
	theme_set_user_picture(pt, s->opt_user_picture);
	struct theme *dst = pt == t ? other : t;
	if (dst->dir[0] && pt->user_pic) {
		if (dst->user_pic)
			cairo_surface_destroy(dst->user_pic);
		dst->user_pic = cairo_surface_reference(pt->user_pic);
	}
}

/* pointers and the GTK theme for apps started from now on */
static void apply_env(struct server *s) {
	const char *cur = s->st_cursors == 7 ? "expede-7" : "expede-xp";
	setenv("XCURSOR_THEME", cur, true);
	if (strcmp(cur, s->cursor_theme)) {
		snprintf(s->cursor_theme, sizeof(s->cursor_theme), "%s", cur);
		if (s->cursor_mgr) {
			wlr_xcursor_manager_destroy(s->cursor_mgr);
			s->cursor_mgr = wlr_xcursor_manager_create(cur, 32);
			wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
		}
	}
	if (s->own_gtk_theme)
		setenv("GTK_THEME", s->style == 7 ? "Expede-7" : "Expede-XP", true);
}

/* expede-settings sends SIGHUP after saving, so changes apply without logging off */
/* ---- screens off after idling, like XP's "Turn off monitor" power setting ---- */

static void set_screens(struct server *s, bool on) {
	struct output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct output_conf *oc = output_conf(s, o->wlr_output->name);
		if (on && oc && oc->disabled && wl_list_length(&s->outputs) > 1)
			continue;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, on);
		wlr_output_commit_state(o->wlr_output, &st);
		wlr_output_state_finish(&st);
	}
	s->screens_off = !on;
}

static int idle_fire(void *data) {
	struct server *s = data;
	/* video players and the like hold an idle inhibitor while they play */
	if (!wl_list_empty(&s->idle_inhibit->inhibitors)) {
		wl_event_source_timer_update(s->idle_timer, 30000);
		return 0;
	}
	set_screens(s, false);
	return 0;
}

static void reset_idle(struct server *s) {
	if (s->screens_off)
		set_screens(s, true);
	if (s->idle_timer)
		wl_event_source_timer_update(s->idle_timer, s->opt_screen_off > 0 ? s->opt_screen_off * 60000 : 0);
}

static int on_sighup(int sig, void *data) {
	struct server *s = data;
	load_config(s);
	struct pointer_dev *p;
	wl_list_for_each(p, &s->pointers, link)
		configure_pointer(s, p->dev);
	struct keyboard *k;
	wl_list_for_each(k, &s->keyboards, link)
		apply_keymap(s, k->wlr_keyboard);
	apply_desktop(s);
	apply_env(s);
	session_layout(s->session);
	struct output *o;
	wl_list_for_each(o, &s->outputs, link) {
		struct wlr_output *wo = o->wlr_output;
		struct wlr_output_mode *mode = wanted_mode(s, wo);
		struct output_conf *oc = output_conf(s, wo->name);
		bool enable = !(oc && oc->disabled) || wl_list_length(&s->outputs) == 1;
		double scale = pick_scale(s, wo, mode ? mode->width : wo->width);
		if (scale == wo->scale && (!mode || mode == wo->current_mode) && enable == wo->enabled)
			continue;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, enable);
		if (mode)
			wlr_output_state_set_mode(&st, mode);
		wlr_output_state_set_scale(&st, scale);
		wlr_output_commit_state(wo, &st);
		wlr_output_state_finish(&st);
		write_outputs_status(s);
		ui_scale = scale;
		/* redraw everything at the new size */
		shell_layout(s->shell);
		session_layout(s->session);
		struct view *v;
		wl_list_for_each(v, &s->views, link) {
			if (v->maximized) {
				v->maximized = false;
				view_set_maximized(v, true);
			} else if (v->mapped) {
				view_redraw(v);
			}
		}
	}
	shell_reconfigure(s->shell);
	/* frames may have changed version or colour */
	struct view *v;
	wl_list_for_each(v, &s->views, link) {
		if (!v->mapped)
			continue;
		if (v->maximized) {
			v->maximized = false;
			view_set_maximized(v, true);
		} else {
			view_redraw(v);
		}
	}
	if (s->opt_snap == false)
		snap_preview(s, SNAP_NONE);
	reset_idle(s);
	wlr_log(WLR_INFO, "settings reloaded");
	return 0;
}

static int on_sigusr1(int sig, void *data) {
	(void)sig;
	snip_begin(data);
	return 0;
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_INFO, NULL);
	const char *startup = NULL;
	bool autologin = false;
	int c;
	while ((c = getopt(argc, argv, "s:ah")) != -1) {
		if (c == 's')
			startup = optarg;
		else if (c == 'a')
			autologin = true;
		else {
			printf("Usage: %s [-a] [-s startup command]\n"
				"  -a  skip the Welcome screen and log straight on\n", argv[0]);
			return 0;
		}
	}

	struct server s = {0};
	the_server = &s;
	/* <data>/expede/xp from xprip, <data>/expede/7 from w7rip: either or both */
	char dir7[600];
	snprintf(dir7, sizeof(dir7), "%s", default_assets());
	char *slash = strrchr(dir7, '/');
	if (slash)
		snprintf(slash + 1, sizeof(dir7) - (slash + 1 - dir7), "7");
	s.have_xp = theme_load(&s.theme, default_assets());
	s.have7 = theme_load(&s.theme7, dir7) && s.theme7.version == 7;
	if (!s.have_xp && !s.have7)
		return 1;
	load_config(&s);
	/* the pointers each rip turned into Xcursor themes; exported so client-drawn cursors match */
	char cpath[1400];
	const char *old = getenv("XCURSOR_PATH");
	snprintf(cpath, sizeof(cpath), "%s/cursors:%s/cursors:%s", default_assets(), dir7,
		old ? old : "/usr/share/icons:/usr/share/pixmaps");
	setenv("XCURSOR_PATH", cpath, true);
	setenv("XCURSOR_SIZE", "32", true);
	s.own_gtk_theme = !getenv("GTK_THEME");
	apply_env(&s);
	apply_desktop(&s);

	s.display = wl_display_create();
	s.backend = wlr_backend_autocreate(wl_display_get_event_loop(s.display), NULL);
	if (!s.backend)
		return 1;
	s.renderer = wlr_renderer_autocreate(s.backend);
	wlr_renderer_init_wl_display(s.renderer, s.display);
	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
	wlr_compositor_create(s.display, 6, s.renderer);
	/* let apps draw sharp at 125%/150%/200% instead of being stretched */
	wlr_viewporter_create(s.display);
	wlr_fractional_scale_manager_v1_create(s.display, 1);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);
	wlr_screencopy_manager_v1_create(s.display);

	s.output_layout = wlr_output_layout_create(s.display);
	wl_list_init(&s.outputs);
	s.new_output.notify = on_new_output;
	wl_signal_add(&s.backend->events.new_output, &s.new_output);

	s.scene = wlr_scene_create();
	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
	s.layer_desktop = wlr_scene_tree_create(&s.scene->tree);
	s.layer_views = wlr_scene_tree_create(&s.scene->tree);
	s.layer_shell = wlr_scene_tree_create(&s.scene->tree);
	s.layer_fullscreen = wlr_scene_tree_create(&s.scene->tree); /* above the taskbar */
	s.layer_menu = wlr_scene_tree_create(&s.scene->tree);
	s.layer_overlay = wlr_scene_tree_create(&s.scene->tree);

	wl_list_init(&s.views);
	s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
	s.new_xdg_toplevel.notify = on_new_toplevel;
	wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
	s.new_xdg_popup.notify = on_new_popup;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);
	/* GTK3 only asks for server-side decorations through KDE's older protocol */
	s.kde_deco_mgr = wlr_server_decoration_manager_create(s.display);
	wlr_server_decoration_manager_set_default_mode(s.kde_deco_mgr, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	s.deco_mgr = wlr_xdg_decoration_manager_v1_create(s.display);
	s.new_decoration.notify = on_new_decoration;
	wl_signal_add(&s.deco_mgr->events.new_toplevel_decoration, &s.new_decoration);

	s.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s.cursor, s.output_layout);
	s.cursor_mgr = wlr_xcursor_manager_create(s.cursor_theme, 32);
	s.cursor_motion.notify = on_cursor_motion;
	wl_signal_add(&s.cursor->events.motion, &s.cursor_motion);
	s.cursor_motion_abs.notify = on_cursor_motion_abs;
	wl_signal_add(&s.cursor->events.motion_absolute, &s.cursor_motion_abs);
	s.cursor_button.notify = on_cursor_button;
	wl_signal_add(&s.cursor->events.button, &s.cursor_button);
	s.cursor_axis.notify = on_cursor_axis;
	wl_signal_add(&s.cursor->events.axis, &s.cursor_axis);
	s.cursor_frame.notify = on_cursor_frame;
	wl_signal_add(&s.cursor->events.frame, &s.cursor_frame);

	wl_list_init(&s.keyboards);
	wl_list_init(&s.pointers);
	s.new_input.notify = on_new_input;
	wl_signal_add(&s.backend->events.new_input, &s.new_input);
	s.seat = wlr_seat_create(s.display, "seat0");
	s.request_cursor.notify = on_request_cursor;
	wl_signal_add(&s.seat->events.request_set_cursor, &s.request_cursor);
	wlr_primary_selection_v1_device_manager_create(s.display);
	s.request_set_primary.notify = on_request_set_primary;
	wl_signal_add(&s.seat->events.request_set_primary_selection, &s.request_set_primary);
	struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(s.display);
	s.request_activate.notify = on_request_activate;
	wl_signal_add(&activation->events.request_activate, &s.request_activate);
	struct wlr_cursor_shape_manager_v1 *shapes = wlr_cursor_shape_manager_v1_create(s.display, 1);
	s.request_cursor_shape.notify = on_request_cursor_shape;
	wl_signal_add(&shapes->events.request_set_shape, &s.request_cursor_shape);
	s.request_set_selection.notify = on_request_set_selection;
	wl_signal_add(&s.seat->events.request_set_selection, &s.request_set_selection);

	s.shell = shell_create(&s);
	s.idle_inhibit = wlr_idle_inhibit_v1_create(s.display);
	s.idle_timer = wl_event_loop_add_timer(wl_display_get_event_loop(s.display), idle_fire, &s);
	wl_event_loop_add_signal(wl_display_get_event_loop(s.display), SIGHUP, on_sighup, &s);
	wl_event_loop_add_signal(wl_display_get_event_loop(s.display), SIGUSR1, on_sigusr1, &s);

	const char *socket = wl_display_add_socket_auto(s.display);
	if (!socket || !wlr_backend_start(s.backend)) {
		wlr_backend_destroy(s.backend);
		wl_display_destroy(s.display);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "Expede", true);
	/* GTK apps draw their own widgets with the theme xprip generated from the same
	 * Luna bitmaps (only apps started inside Expede; other sessions are untouched) */
	apply_env(&s);
	/* Qt apps take colours, fonts and dialogs from the GTK theme */
	if (!getenv("QT_QPA_PLATFORMTHEME"))
		setenv("QT_QPA_PLATFORMTHEME", "gtk3", true);
	setenv("QT_QPA_PLATFORM", "wayland", true);
	shell_layout(s.shell);
	s.session = session_create(&s, autologin);
	session_layout(s.session);
	wlr_cursor_set_xcursor(s.cursor, s.cursor_mgr, "default");
	reset_idle(&s);
	/* asks for your password when a setting needs it (time zone, printers...), in XP style
	 * since it is a GTK program */
	if (access("/usr/bin/lxpolkit", X_OK) == 0)
		spawn("/usr/bin/lxpolkit");
	if (startup)
		spawn(startup);
	wlr_log(WLR_INFO, "Expede running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(s.display);

	wl_display_destroy_clients(s.display);
	wlr_scene_node_destroy(&s.scene->tree.node);
	wlr_xcursor_manager_destroy(s.cursor_mgr);
	wlr_cursor_destroy(s.cursor);
	wlr_allocator_destroy(s.allocator);
	wlr_renderer_destroy(s.renderer);
	wlr_backend_destroy(s.backend);
	wl_display_destroy(s.display);
	return 0;
}
