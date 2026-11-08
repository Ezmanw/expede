/* Luna theme engine: reads the bitmaps and the NormalBlue ini that xprip pulled
 * out of the user's XP ISO, and draws them the way uxtheme does (nine-slice
 * stretch/tile, multi-frame state strips, caption text with shadow). */
#define _GNU_SOURCE
#include <ctype.h>
#ifndef EXPEDE_CLIENT
#include <drm_fourcc.h>
#endif
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef EXPEDE_CLIENT
#include "theme.h"
#define wlr_log(level, fmt, ...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include "expede.h"
#endif

/* ---- ini ---- */

static char *ini_text;

static const char *ini_get(const char *section, const char *key) {
	static char val[256];
	if (!ini_text)
		return NULL;
	size_t slen = strlen(section), klen = strlen(key);
	bool in = false;
	for (char *line = ini_text; line && *line;) {
		char *nl = strchr(line, '\n');
		size_t len = nl ? (size_t)(nl - line) : strlen(line);
		while (len && isspace((unsigned char)*line)) { line++; len--; }
		if (len && *line == '[') {
			in = len >= slen + 2 && !strncasecmp(line + 1, section, slen) && line[slen + 1] == ']';
		} else if (in && len > klen && !strncasecmp(line, key, klen)) {
			const char *p = line + klen;
			while (*p == ' ' || *p == '\t') p++;
			if (*p == '=') {
				p++;
				while (*p == ' ' || *p == '\t') p++;
				size_t n = 0;
				while (p + n < line + len && p[n] != ';' && n < sizeof(val) - 1) n++;
				while (n && isspace((unsigned char)p[n - 1])) n--;
				memcpy(val, p, n);
				val[n] = 0;
				return val;
			}
		}
		line = nl ? nl + 1 : NULL;
	}
	return NULL;
}

static int ini_ints(const char *section, const char *key, int *out, int n) {
	const char *v = ini_get(section, key);
	int got = 0;
	while (v && *v && got < n) {
		while (*v && !isdigit((unsigned char)*v) && *v != '-') v++;
		if (!*v)
			break;
		out[got++] = strtol(v, (char **)&v, 10);
	}
	return got;
}

/* ---- scale ---- */

double ui_scale = 1.0;

cairo_surface_t *canvas_new(int w, int h) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)(w * ui_scale + 0.999), (int)(h * ui_scale + 0.999));
	cairo_surface_set_device_scale(s, ui_scale, ui_scale);
	return s;
}

/* ---- images ---- */

cairo_surface_t *theme_png(struct theme *t, const char *rel) {
	char path[768];
	snprintf(path, sizeof(path), "%s/%s", t->dir, rel);
	cairo_surface_t *s = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "theme: cannot load %s", path);
		cairo_surface_destroy(s);
		return NULL;
	}
	return s;
}

/* "Blue\FrameCaption.bmp" -> luna/framecaption.png */
static cairo_surface_t *load_ini_file(struct theme *t, const char *section, const char *key) {
	const char *v = ini_get(section, key);
	if (!v)
		return NULL;
	const char *b = strrchr(v, '\\');
	b = b ? b + 1 : v;
	char name[128];
	size_t i = 0;
	for (; b[i] && b[i] != '.' && i < sizeof(name) - 12; i++)
		name[i] = tolower((unsigned char)b[i]);
	strcpy(name + i, ".png");
	char rel[160];
	snprintf(rel, sizeof(rel), "luna/%s", name);
	return theme_png(t, rel);
}

static void load_image(struct theme *t, struct image *img, const char *section) {
	img->s = load_ini_file(t, section, "ImageFile");
	int c = 1;
	ini_ints(section, "ImageCount", &c, 1);
	img->count = c > 0 ? c : 1;
	const char *lay = ini_get(section, "ImageLayout");
	img->vertical = !lay || !strcasecmp(lay, "vertical");
	int m[4] = {0};
	ini_ints(section, "SizingMargins", m, 4);
	img->m = (struct margins){m[0], m[1], m[2], m[3]};
	const char *st = ini_get(section, "SizingType");
	img->tile = st && !strcasecmp(st, "tile");
}

static void frame_rect(cairo_surface_t *s, int count, bool vertical, int frame, int *fx, int *fy, int *fw, int *fh) {
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	if (frame >= count)
		frame = count - 1;
	if (vertical) {
		*fw = w; *fh = h / count; *fx = 0; *fy = *fh * frame;
	} else {
		*fw = w / count; *fh = h; *fx = *fw * frame; *fy = 0;
	}
}

/* draw source rect (sx,sy,sw,sh) of s into dest rect, stretched or tiled */
static void blit(cairo_t *cr, cairo_surface_t *s, double sx, double sy, double sw, double sh,
		double dx, double dy, double dw, double dh, bool tile) {
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	cairo_save(cr);
	cairo_rectangle(cr, dx, dy, dw, dh);
	cairo_clip(cr);
	if (tile) {
		cairo_surface_t *sub = cairo_surface_create_for_rectangle(s, sx, sy, sw, sh);
		cairo_pattern_t *p = cairo_pattern_create_for_surface(sub);
		cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
		cairo_matrix_t mtx;
		cairo_matrix_init_translate(&mtx, -dx, -dy);
		cairo_pattern_set_matrix(p, &mtx);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
		cairo_surface_destroy(sub);
	} else {
		cairo_translate(cr, dx, dy);
		cairo_scale(cr, dw / sw, dh / sh);
		cairo_set_source_surface(cr, s, -sx, -sy);
		/* uxtheme stretches with plain StretchBlt: nearest neighbour, no smoothing.
		 * At 125%/150% that makes uneven pixels, so smooth those scales instead. */
		cairo_pattern_set_filter(cairo_get_source(cr),
			ui_scale == (int)ui_scale ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_GOOD);
		cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
		cairo_paint(cr);
	}
	cairo_restore(cr);
}

void draw_image(cairo_t *cr, struct image *img, int frame, double x, double y, double w, double h) {
	if (!img->s)
		return;
	int fx, fy, fw, fh;
	frame_rect(img->s, img->count, img->vertical, frame, &fx, &fy, &fw, &fh);
	struct margins m = img->m;
	if (m.l + m.r > fw) m.l = m.r = 0;
	if (m.t + m.b > fh) m.t = m.b = 0;
	/* margins larger than the destination shrink proportionally, as uxtheme does */
	double ml = m.l, mr = m.r, mt = m.t, mb = m.b;
	if (ml + mr > w) { double k = w / (ml + mr); ml *= k; mr *= k; }
	if (mt + mb > h) { double k = h / (mt + mb); mt *= k; mb *= k; }
	double sxs[3] = {fx, fx + m.l, fx + fw - m.r}, sws[3] = {m.l, fw - m.l - m.r, m.r};
	double sys[3] = {fy, fy + m.t, fy + fh - m.b}, shs[3] = {m.t, fh - m.t - m.b, m.b};
	double dxs[3] = {x, x + ml, x + w - mr}, dws[3] = {ml, w - ml - mr, mr};
	double dys[3] = {y, y + mt, y + h - mb}, dhs[3] = {mt, h - mt - mb, mb};
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			/* corners are drawn at true size, edges and middle stretch (or tile) */
			bool tile = img->tile && (r == 1 || c == 1);
			blit(cr, img->s, sxs[c], sys[r], sws[c], shs[r], dxs[c], dys[r], dws[c], dhs[r], tile);
		}
}

void draw_frame_true(cairo_t *cr, cairo_surface_t *s, int count, int frame, double x, double y) {
	if (!s)
		return;
	int fx, fy, fw, fh;
	frame_rect(s, count, true, frame, &fx, &fy, &fw, &fh);
	blit(cr, s, fx, fy, fw, fh, x, y, fw, fh, false);
}

int frame_height(cairo_surface_t *s, int count) {
	return s ? cairo_image_surface_get_height(s) / (count > 0 ? count : 1) : 0;
}

void draw_frame_dpi(cairo_t *cr, cairo_surface_t *lo, cairo_surface_t *hi, int count, int frame, double x, double y) {
	if (!lo)
		return;
	if (!hi || ui_scale < 1.5) {
		draw_frame_true(cr, lo, count, frame, x, y);
		return;
	}
	/* the 192 dpi art drawn at the 96 dpi size: sharp on 2x screens */
	double k = (double)cairo_image_surface_get_width(lo) / cairo_image_surface_get_width(hi);
	int fh = frame_height(hi, count);
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, k, k);
	cairo_rectangle(cr, 0, 0, cairo_image_surface_get_width(hi), fh);
	cairo_clip(cr);
	cairo_set_source_surface(cr, hi, 0, -fh * frame);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

void draw_surface(cairo_t *cr, cairo_surface_t *s, double x, double y) {
	if (!s)
		return;
	cairo_set_source_surface(cr, s, x, y);
	cairo_paint(cr);
}

/* ---- text ---- */

static int font_version;

void theme_use_fonts(const struct theme *t) {
	font_version = t ? t->version : 0;
}

/* Windows 7 drew its UI in Segoe UI 9 where XP used Tahoma 8 (and Trebuchet
 * for captions): map the XP names so every caller follows the version. */
static PangoFontDescription *version_font(const char *font) {
	PangoFontDescription *d = pango_font_description_from_string(font);
	const char *fam = pango_font_description_get_family(d);
	if (font_version >= 7 && fam && (!strcasecmp(fam, "Tahoma") || !strcasecmp(fam, "Trebuchet MS"))) {
		pango_font_description_set_family(d, "Segoe UI");
		int sz = pango_font_description_get_size(d) / PANGO_SCALE;
		if (sz == 8)
			pango_font_description_set_size(d, 9 * PANGO_SCALE);
		if (!strcasecmp(fam, "Trebuchet MS") && sz <= 10)
			pango_font_description_set_weight(d, PANGO_WEIGHT_NORMAL);
	}
	return d;
}

static PangoLayout *make_layout(cairo_t *cr, const char *font, const char *text, double maxw, bool crisp) {
	PangoLayout *l = pango_cairo_create_layout(cr);
	/* ClearType-era text (7 and 10) is smoothed, and so is XP's on high-DPI
	 * screens, where unsmoothed pixels just look jagged */
	if (font_version >= 7 || ui_scale >= 1.5)
		crisp = false;
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_font_options_set_antialias(fo, crisp ? CAIRO_ANTIALIAS_NONE : CAIRO_ANTIALIAS_GRAY);
	cairo_font_options_set_hint_style(fo, crisp ? CAIRO_HINT_STYLE_FULL : CAIRO_HINT_STYLE_SLIGHT);
	cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_ON);
	pango_cairo_context_set_font_options(pango_layout_get_context(l), fo);
	cairo_font_options_destroy(fo);
	/* XP sizes are points at 96 dpi */
	pango_cairo_context_set_resolution(pango_layout_get_context(l), 96);
	pango_layout_context_changed(l);
	PangoFontDescription *d = version_font(font);
	pango_layout_set_font_description(l, d);
	pango_font_description_free(d);
	pango_layout_set_text(l, text, -1);
	if (maxw > 0) {
		pango_layout_set_width(l, maxw * PANGO_SCALE);
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	}
	return l;
}

static void set_rgb(cairo_t *cr, uint32_t rgb) {
	cairo_set_source_rgb(cr, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

int draw_text(cairo_t *cr, const char *font, const char *text, double x, double y, double maxw,
		uint32_t rgb, int sdx, int sdy, uint32_t srgb, bool crisp) {
	PangoLayout *l = make_layout(cr, font, text, maxw, crisp);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	if (sdx || sdy) {
		set_rgb(cr, srgb);
		cairo_move_to(cr, x + sdx, y + sdy);
		pango_cairo_show_layout(cr, l);
	}
	set_rgb(cr, rgb);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, l);
	cairo_new_path(cr); /* leave no current point for the next path to join up with */
	g_object_unref(l);
	return w;
}

/* Desktop icon caption: centred, word-wrapped to two lines like Explorer. With
 * sel_rgb set it sits on a highlight box instead of casting a shadow. */
void draw_label(cairo_t *cr, const char *font, const char *text, double cx, double y, double maxw,
		uint32_t rgb, uint32_t shadow_rgb, int64_t sel_rgb) {
	PangoLayout *l = make_layout(cr, font, text, 0, true);
	pango_layout_set_width(l, maxw * PANGO_SCALE);
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
	pango_layout_set_height(l, -2);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	PangoRectangle ink, log;
	pango_layout_get_pixel_extents(l, &ink, &log);
	double x = cx - maxw / 2;
	if (sel_rgb >= 0) {
		set_rgb(cr, (uint32_t)sel_rgb);
		cairo_rectangle(cr, x + log.x - 2, y + log.y - 1, log.width + 4, log.height + 2);
		cairo_fill(cr);
	} else {
		set_rgb(cr, shadow_rgb);
		cairo_move_to(cr, x + 1, y + 1);
		pango_cairo_show_layout(cr, l);
	}
	set_rgb(cr, rgb);
	cairo_move_to(cr, x, y);
	pango_cairo_show_layout(cr, l);
	g_object_unref(l);
}

int text_width(const char *font, const char *text) {
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(s);
	PangoLayout *l = make_layout(cr, font, text, 0, true);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	cairo_destroy(cr);
	cairo_surface_destroy(s);
	return w;
}

#ifndef EXPEDE_CLIENT
/* ---- cairo surface as a wlr_buffer the scene graph can show ---- */

struct cbuf {
	struct wlr_buffer base;
	cairo_surface_t *s;
};

static void cbuf_destroy(struct wlr_buffer *b) {
	struct cbuf *c = wl_container_of(b, c, base);
	cairo_surface_destroy(c->s);
	free(c);
}

static bool cbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct cbuf *c = wl_container_of(b, c, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;
	cairo_surface_flush(c->s);
	*data = cairo_image_surface_get_data(c->s);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(c->s);
	return true;
}

static void cbuf_end(struct wlr_buffer *b) {
	(void)b;
}

static const struct wlr_buffer_impl cbuf_impl = {
	.destroy = cbuf_destroy,
	.begin_data_ptr_access = cbuf_begin,
	.end_data_ptr_access = cbuf_end,
};

/* takes ownership of s */
struct wlr_buffer *buffer_from_cairo(cairo_surface_t *s) {
	struct cbuf *c = calloc(1, sizeof(*c));
	c->s = s;
	wlr_buffer_init(&c->base, &cbuf_impl, cairo_image_surface_get_width(s), cairo_image_surface_get_height(s));
	return &c->base;
}

void scene_show(struct wlr_scene_buffer **node, struct wlr_scene_tree *parent, cairo_surface_t *s, int x, int y) {
	double sx, sy;
	cairo_surface_get_device_scale(s, &sx, &sy);
	int lw = (int)(cairo_image_surface_get_width(s) / sx + 0.5), lh = (int)(cairo_image_surface_get_height(s) / sy + 0.5);
	struct wlr_buffer *b = buffer_from_cairo(s);
	if (!*node)
		*node = wlr_scene_buffer_create(parent, b);
	else
		wlr_scene_buffer_set_buffer(*node, b);
	wlr_buffer_drop(b);
	/* the buffer has ui_scale times the pixels; show it at its logical size */
	wlr_scene_buffer_set_dest_size(*node, lw, lh);
	wlr_scene_node_set_position(&(*node)->node, x, y);
	wlr_scene_node_set_enabled(&(*node)->node, true);
}

#endif

/* ---- loading ---- */

cairo_surface_t *theme_load_image(const char *path) {
	GError *err = NULL;
	GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, &err);
	if (!pb) {
		wlr_log(WLR_ERROR, "wallpaper %s: %s", path, err->message);
		g_error_free(err);
		return NULL;
	}
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
	unsigned char *dst = cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const guchar *p = src + y * rs + x * n;
			((uint32_t *)(dst + y * ds))[x] = 0xff000000u | p[0] << 16 | p[1] << 8 | p[2];
		}
	cairo_surface_mark_dirty(s);
	g_object_unref(pb);
	return s;
}

void theme_set_wallpaper(struct theme *t, const char *name, const char *style) {
	if (t->wallpaper)
		cairo_surface_destroy(t->wallpaper);
	t->wallpaper = NULL;
	char path[1024];
	if (name && *name) {
		if (name[0] == '/')
			snprintf(path, sizeof(path), "%s", name);
		else
			snprintf(path, sizeof(path), "%s/wallpapers/%s", t->dir, name);
		t->wallpaper = theme_load_image(path);
		if (!t->wallpaper) { /* older rips only have bliss.jpg at the top */
			snprintf(path, sizeof(path), "%s/bliss.jpg", t->dir);
			t->wallpaper = theme_load_image(path);
		}
	}
	bool tiled = name && strstr(name, ".tile.");
	if (style && !strcasecmp(style, "center"))
		t->wallpaper_style = WALL_CENTER;
	else if (style && !strcasecmp(style, "tile"))
		t->wallpaper_style = WALL_TILE;
	else if (style && !strcasecmp(style, "stretch"))
		t->wallpaper_style = WALL_STRETCH;
	else
		t->wallpaper_style = tiled ? WALL_TILE : WALL_STRETCH;
}

void theme_set_user_picture(struct theme *t, const char *name) {
	if (t->user_pic)
		cairo_surface_destroy(t->user_pic);
	t->user_pic = NULL;
	char rel[160];
	if (name && *name) {
		snprintf(rel, sizeof(rel), "pictures/%s.png", name);
		t->user_pic = theme_png(t, rel);
	}
	if (!t->user_pic)
		t->user_pic = theme_png(t, "userpic.png");
}

struct icon_set theme_icon(struct theme *t, const char *name) {
	char rel[128];
	struct icon_set i;
	snprintf(rel, sizeof(rel), "icons/%s-16.png", name);
	i.s16 = theme_png(t, rel);
	snprintf(rel, sizeof(rel), "icons/%s-32.png", name);
	i.s32 = theme_png(t, rel);
	return i;
}

static int button_offset(const char *section, int fallback) {
	int o[2] = {fallback, 5};
	ini_ints(section, "Offset", o, 2);
	return -o[0];
}

static void load_w7(struct theme *t) {
	/* DWMWindow parts: 3/4 min, 5/6 max, 7/8 close (active/inactive) */
	static const char *btn[2][3] = {{"DWMWindow.3", "DWMWindow.5", "DWMWindow.7"},
		{"DWMWindow.4", "DWMWindow.6", "DWMWindow.8"}};
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 3; j++)
			load_image(t, &t->w7_btn[i][j], btn[i][j]);
	/* glyphs: close 12-15, max 21-24, min 25-28, restore 29-32, smallest first */
	static const int glyph[4] = {12, 21, 25, 29};
	for (int g = 0; g < 4; g++) {
		char sec[32];
		snprintf(sec, sizeof(sec), "DWMWindow.%d", glyph[g]);
		t->w7_glyph[g][0] = load_ini_file(t, sec, "ImageFile");
		snprintf(sec, sizeof(sec), "DWMWindow.%d", glyph[g] + 3);
		t->w7_glyph[g][1] = load_ini_file(t, sec, "ImageFile");
	}
	load_image(t, &t->w7_glow[0], "DWMWindow.16");
	load_image(t, &t->w7_glow[1], "DWMWindow.11");
	t->w7_reflect = load_ini_file(t, "DWMWindow.40", "ImageFile");
	t->orb[0] = theme_png(t, "icons/orb.png");
	t->orb[1] = theme_png(t, "icons/orb-192.png");
	t->w7_userframe = theme_png(t, "icons/userframe.png");
	load_image(t, &t->w7_task, "TaskBand2.5");
	load_image(t, &t->w7_tray_btn, "TrayNotifyHoriz::Button");
	load_image(t, &t->w7_show_desktop, "ShowDesktop::Button");
	load_image(t, &t->w7_sp_left, "StartPanelComposited::StartPanelPriv.6");
	load_image(t, &t->w7_sp_right, "StartPanelComposited::StartPanelPriv.7");
	load_image(t, &t->w7_sp_search, "StartPanelPriv.15");
	load_image(t, &t->w7_sp_item, "StartPanelPriv.18");
	load_image(t, &t->w7_sp_place, "StartPanelPriv.19");
	load_image(t, &t->w7_sp_shutdown, "StartPanelPriv.23");
	load_image(t, &t->w7_sp_shutdown_arrow, "StartPanelPriv.24");
	load_image(t, &t->w7_sp_progs, "StartPanelPriv.12");
	load_image(t, &t->slider_thumb_h, "TrackBar.ThumbBottom");
	load_image(t, &t->w7_edit, "Edit.EditBorderNoScroll");
	load_image(t, &t->w7_menu_item, "Menu.PopupItem");
	load_image(t, &t->w7_list_hot, "Explorer::ListView.ListItem(2)");
	load_image(t, &t->w7_list_sel, "Explorer::ListView.ListItem(3)");
	load_image(t, &t->w7_list_hot_sel, "Explorer::ListView.ListItem(6)");
	load_image(t, &t->w7_header, "ItemsView::Header.HeaderItem");
	load_image(t, &t->w7_scroll_track, "ScrollBar.LowerTrackVert");
	load_image(t, &t->w7_scroll_thumb, "ScrollBar.ThumbBtnVert");
	t->w7_scroll_grip = load_ini_file(t, "ScrollBar.GripperVert", "ImageFile1");
	t->w7_tree_glyph = load_ini_file(t, "Explorer::TreeView.Glyph", "ImageFile1");
	t->w7_tree_glyph_hot = load_ini_file(t, "Explorer::TreeView.HotGlyph", "ImageFile1");
	load_image(t, &t->w7_progress_bar, "Progress.Bar");
	load_image(t, &t->w7_progress_fill, "Progress.Fill");
	load_image(t, &t->w7_nav_bg, "Navbar::Rebar.Background");
	t->w7_back = load_ini_file(t, "Navigation.1", "ImageFile1");
	t->w7_fwd = load_ini_file(t, "Navigation.2", "ImageFile1");
	load_image(t, &t->w7_addr_band, "AB::AddressBand.1");
	load_image(t, &t->w7_crumb_btn, "BB::Toolbar.Button");
	t->w7_crumb_sep = load_ini_file(t, "BB::Toolbar.SplitButtonDropDown", "ImageFile1");
	load_image(t, &t->w7_search_box, "SearchBox.1");
	load_image(t, &t->w7_cmd_bar, "CommandModule.1");
	load_image(t, &t->w7_cmd_btn, "CommandModule.3");
	load_image(t, &t->w7_details_bg, "PreviewPane.1(1)");
}

bool theme_load(struct theme *t, const char *dir) {
	snprintf(t->dir, sizeof(t->dir), "%s", dir);
	char path[768];
	snprintf(path, sizeof(path), "%s/luna/normalblue.ini", dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_ERROR, "no Luna theme at %s: run tools/xprip.py on your XP ISO first", dir);
		return false;
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	free(ini_text);
	ini_text = calloc(1, n + 1);
	fread(ini_text, 1, n, f);
	fclose(f);
	t->version = strstr(ini_text, "; Windows 7") == ini_text ? 7 : strstr(ini_text, "; Windows 10") == ini_text ? 10 : 0;

	load_image(t, &t->caption, "Window.Caption");
	load_image(t, &t->max_caption, "Window.MaxCaption");
	load_image(t, &t->frame_left, "Window.FrameLeft");
	load_image(t, &t->frame_right, "Window.FrameRight");
	load_image(t, &t->frame_bottom, "Window.FrameBottom");
	load_image(t, &t->close_btn, "Window.CloseButton");
	load_image(t, &t->caption_btn, "Window.MinButton");
	t->close_glyph = load_ini_file(t, "Window.CloseButton", "ImageFile3");
	t->max_glyph = load_ini_file(t, "Window.MaxButton", "ImageFile3");
	t->min_glyph = load_ini_file(t, "Window.MinButton", "ImageFile3");
	t->restore_glyph = load_ini_file(t, "Window.RestoreButton", "ImageFile3");
	t->close_off = button_offset("Window.CloseButton", -25);
	t->max_off = button_offset("Window.MaxButton", -48);
	t->min_off = button_offset("Window.MinButton", -73);

	load_image(t, &t->taskbar, "TaskBar.BackgroundBottom");
	load_image(t, &t->tray, "TrayNotifyHoriz::TrayNotify.Background");
	load_image(t, &t->task_button, "TaskBand::Toolbar.Button");
	load_image(t, &t->start_button, "Start::Button");
	load_image(t, &t->user_pane, "StartPanel.UserPane");
	load_image(t, &t->user_tile, "StartPanel.UserPicture");
	load_image(t, &t->prog_list, "StartPanel.ProgList");
	load_image(t, &t->places_list, "StartPanel.PlacesList");
	load_image(t, &t->more_progs, "StartPanel.MorePrograms");
	load_image(t, &t->logoff, "StartPanel.Logoff");
	load_image(t, &t->push_button, "Button.Pushbutton");
	load_image(t, &t->tab_pane, "Tab.Pane");
	load_image(t, &t->tab_item, "Tab.TopTabItemLeftEdge");
	load_image(t, &t->groupbox, "Button.GroupBox");
	load_image(t, &t->slider_track, "TrackBar.Track");
	load_image(t, &t->slider_thumb, "TrackBar.ThumbVert");
	load_image(t, &t->slider_thumb_h, "TrackBar.Thumb");
	t->checkbox = theme_png(t, "luna/checkbox13.png");
	t->radio = theme_png(t, "luna/radiobutton13.png");
	t->more_arrow = load_ini_file(t, "StartPanel.MoreProgramsArrow", "ImageFile");
	t->more_arrow_hot = load_ini_file(t, "StartPanel.MoreProgramsArrow(hot)", "ImageFile");
	t->logoff_buttons = load_ini_file(t, "StartPanel.LogoffButtons", "ImageFile");
	t->logoff_buttons_hot = load_ini_file(t, "StartPanel.LogoffButtons(hot)", "ImageFile");
	t->prog_sep = load_ini_file(t, "StartPanel.ProgListSeparator", "ImageFile");
	t->places_sep = load_ini_file(t, "StartPanel.PlacesListSeparator", "ImageFile");
	t->start_flag = theme_png(t, "icons/start-flag.png");

	if (t->version == 7)
		load_w7(t);

	int bg[3] = {0, 78, 152};
	ini_ints("SysMetrics", "Background", bg, 3);
	for (int i = 0; i < 3; i++)
		t->desktop_bg[i] = bg[i] / 255.0;
	theme_set_wallpaper(t, t->version == 10 ? "Windows 10.jpg" : t->version == 7 ? "Windows 7.jpg" : "Bliss.jpg", "auto");
	theme_set_user_picture(t, "");
	return t->caption.s != NULL;
}
