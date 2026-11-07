#ifndef EXPEDE_THEME_H
#define EXPEDE_THEME_H

/* Shared by the compositor and expede-settings; nothing here needs wlroots. */
#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

/* ---- theme (theme.c): Luna bitmaps + the msstyles ini that describes them ---- */

struct margins { int l, r, t, b; };

struct image {
	cairo_surface_t *s;
	int count;     /* frames stacked in the bitmap */
	bool vertical; /* frames stacked top to bottom */
	struct margins m;
	bool tile;
};

struct theme {
	char dir[512];
	struct image caption, max_caption, frame_left, frame_right, frame_bottom;
	struct image close_btn, caption_btn;
	cairo_surface_t *close_glyph, *max_glyph, *min_glyph, *restore_glyph;
	int close_off, max_off, min_off; /* caption button x offsets from the right edge */
	struct image tab_pane, tab_item, groupbox, slider_track, slider_thumb, slider_thumb_h;
	cairo_surface_t *checkbox, *radio;
	struct image taskbar, tray, task_button, start_button;
	struct image user_pane, user_tile, prog_list, places_list, more_progs, logoff, push_button;
	cairo_surface_t *more_arrow, *more_arrow_hot, *logoff_buttons, *logoff_buttons_hot;
	cairo_surface_t *prog_sep, *places_sep, *start_flag, *wallpaper, *user_pic;
	int wallpaper_style; /* WALL_STRETCH, WALL_CENTER, WALL_TILE */
	double desktop_bg[3];
};

struct icon_set { cairo_surface_t *s16, *s32; };

/* screen scale (1.0 = XP's 96 dpi); canvases are drawn at this many pixels per unit */
extern double ui_scale;
/* an ARGB surface of w x h logical pixels, backed by w*ui_scale x h*ui_scale real ones */
cairo_surface_t *canvas_new(int w, int h);

enum { WALL_STRETCH, WALL_CENTER, WALL_TILE };

bool theme_load(struct theme *t, const char *dir);
/* name: a file in <assets>/wallpapers, an absolute path, or "" for none; style "auto"
 * tiles the old .tile.png patterns and stretches photos, as XP did */
void theme_set_wallpaper(struct theme *t, const char *name, const char *style);
/* one of XP's account pictures by name ("chess"), falling back to the default */
void theme_set_user_picture(struct theme *t, const char *name);
/* any image gdk-pixbuf reads (JPEG, PNG, BMP...) as a cairo surface, or NULL */
cairo_surface_t *theme_load_image(const char *path);
struct icon_set theme_icon(struct theme *t, const char *name);
cairo_surface_t *theme_png(struct theme *t, const char *rel);
void draw_image(cairo_t *cr, struct image *img, int frame, double x, double y, double w, double h);
void draw_frame_true(cairo_t *cr, cairo_surface_t *s, int count, int frame, double x, double y);
void draw_surface(cairo_t *cr, cairo_surface_t *s, double x, double y);
/* font: pango description ("Tahoma 8"), crisp: no antialiasing (XP's small-text look) */
int draw_text(cairo_t *cr, const char *font, const char *text, double x, double y, double maxw,
	uint32_t rgb, int shadow_dx, int shadow_dy, uint32_t shadow_rgb, bool crisp);
void draw_label(cairo_t *cr, const char *font, const char *text, double cx, double y, double maxw,
	uint32_t rgb, uint32_t shadow_rgb, int64_t sel_rgb);
int text_width(const char *font, const char *text);

#endif
