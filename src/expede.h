#ifndef EXPEDE_H
#define EXPEDE_H

#include <stdbool.h>
#include <cairo.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "theme.h"

/* takes ownership of s */
struct wlr_buffer *buffer_from_cairo(cairo_surface_t *s);
/* put a canvas_new() surface in the scene (creating *node the first time) at its logical size; takes s */
void scene_show(struct wlr_scene_buffer **node, struct wlr_scene_tree *parent, cairo_surface_t *s, int x, int y);

/* ---- compositor (main.c) ---- */

#define BORDER 4
#define CAPTION 30
#define TASKBAR_H 30 /* XP; Windows 7's superbar is 40 */

enum part { PART_NONE, PART_CAPTION, PART_CLOSE, PART_MAX, PART_MIN, PART_BORDER };

struct view {
	struct wl_list link;
	struct server *server;
	struct wlr_xdg_toplevel *xt;
	struct wlr_scene_tree *tree, *content;
	struct wlr_scene_buffer *deco[4]; /* caption, left, right, bottom */
	struct wlr_scene_buffer *glass[5]; /* Windows 7: blurred desktop behind the frame */
	struct icon_set icon;
	int x, y, cw, ch;
	bool mapped, minimized, maximized, active, snapped;
	bool csd; /* the app draws its own title bar; Expede adds no frame */
	bool fullscreen;
	struct wlr_box pre_fullscreen;
	struct wlr_box saved, presnap;
	enum part hot, pressed;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener map, unmap, commit, destroy, request_move, request_resize;
	struct wl_listener request_maximize, request_minimize, request_fullscreen, set_title, set_app_id;
	struct wl_listener deco_destroy, deco_request_mode;
};

enum grab_mode { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE, GRAB_BUTTON, GRAB_SNIP };

struct server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layer_desktop, *layer_views, *layer_shell, *layer_menu;
	struct wlr_output_layout *output_layout;
	struct wl_list outputs, keyboards, views;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xdg_decoration_manager_v1 *deco_mgr;
	struct wlr_server_decoration_manager *kde_deco_mgr;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_seat *seat;
	struct theme theme;  /* Windows XP (Luna) */
	struct theme theme7; /* Windows 7 (Aero), when it has been ripped */
	struct theme theme10; /* Windows 10 */
	bool have_xp, have7, have10;
	/* Windows 10: accent colour, dark taskbar and Start, acrylic, accent on title bars */
	uint32_t accent_rgb;
	bool opt_dark, opt_transparency, opt_accent_titlebars;
	/* which Windows each part looks like: 0 = XP, 7 = Windows 7 ("style" sets them all;
	 * the advanced options mix them) */
	int style, st_frames, st_taskbar, st_start, st_logon, st_sounds, st_cursors, st_icons;
	bool opt_glass;          /* Aero transparency */
	uint32_t glass_rgb;      /* window colour */
	int glass_intensity;     /* 0..100 */
	struct wlr_buffer *glass_buf; /* the desktop, blurred, at 1/GLASS_DIV of the screen */
	char cursor_theme[32];
	bool own_gtk_theme;
	bool super_alone;
	bool greeter; /* expede -g: the login screen for greetd */ /* Win is down and nothing else has been pressed with it */

	enum grab_mode grab;
	struct view *grab_view;
	double grab_x, grab_y;
	struct wlr_box grab_box;
	uint32_t resize_edges;
	bool snip_dragging;
	int snip_mode, snip_hot;
	struct view *snip_view;
	struct wlr_scene_buffer *snip_overlay;
	uint32_t last_click_ms;
	/* options from ~/.config/expede/expede.conf */
	bool opt_snap, opt_tap, opt_modern_start, opt_natural_scroll;
	int opt_pointer_speed; /* -100..100, 0 = default */
	struct wl_list pointers; /* struct pointer_dev */
	double opt_scale; /* 0 = pick from the screen's DPI */
	bool opt_sounds, opt_clock24;
	char opt_wallpaper[256], opt_wallpaper_style[16], opt_user_picture[64], opt_keyboard[64];
	int opt_screen_off; /* minutes idle before the screens turn off; 0 = never */
	struct output_conf { char name[32]; int w, h, refresh_mhz; bool disabled, has_pos, primary; int x, y, rotation; } out_conf[8];
	/* Windows' "Multiple displays": 0 extend, 1 duplicate */
	int display_mode;
	int n_out_conf;
	/* idle */
	struct wl_event_source *idle_timer;
	bool screens_off;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	int snap_zone;
	struct wlr_scene_rect *snap_preview;
	double last_click_x, last_click_y;

	struct shell *shell;
	struct session *session;
	struct wlr_scene_tree *layer_overlay, *layer_fullscreen;

	struct wl_listener new_output, new_xdg_toplevel, new_xdg_popup, new_decoration;
	struct wl_listener cursor_motion, cursor_motion_abs, cursor_button, cursor_axis, cursor_frame;
	struct wl_listener new_input, request_cursor, request_set_selection, request_set_primary;
	struct wl_listener request_activate, request_cursor_shape;
};

#define GLASS_DIV 4
/* one blur pass along a line of n pixels, step apart (ARGB, premultiplied) */
void blur_line(uint32_t *px, int n, int step, int r, uint32_t *tmp);
/* blur a gw x gh CAIRO_FORMAT_ARGB32 image surface in place (3-pass box blur) */
void blur_surface(cairo_surface_t *g, int r);
/* the theme that draws a part: theme_of(s, s->st_frames) */
struct theme *theme_of(struct server *s, int style);
int taskbar_h(struct server *s);
/* a node showing the blurred scene behind screen rect (x, y, w, h) (created on first use);
 * hidden when glass is off. Keep it below whatever it backs. */
void glass_backdrop(struct server *s, struct wlr_scene_buffer **node, struct wlr_scene_tree *parent,
	int px, int py, int x, int y, int w, int h);
/* shell.c made a new desktop picture: s->glass_buf changed */
void server_glass_changed(struct server *s);
/* update the taskbar and Start menu glass backdrops when windows behind them change */
void shell_update_glass(struct shell *sh);
/* draw the glass fill for a w x h panel at screen position (sx, sy): tint, streaks, edges */
void draw_glass(struct server *s, cairo_t *cr, double x, double y, double w, double h, double r_top,
	double r_bottom, bool active);

/* a Segoe MDL2 Assets glyph centred on (cx, cy) */
void draw_mdl2(cairo_t *cr, const char *utf8, double size_pt, double cx, double cy, uint32_t rgb);

void view_focus(struct view *v);
void view_set_minimized(struct view *v, bool min);
void view_close(struct view *v);
void spawn(const char *cmd);
void server_screen(struct server *s, int *w, int *h); /* the primary screen's size */
/* the screen under (x, y) (the primary one if none), whole or minus the taskbar */
struct wlr_box server_output_box(struct server *s, double x, double y);
struct wlr_box server_work_area(struct server *s, double x, double y);
/* boxes of the other enabled screens (not the primary, not mirrors of it) */
int server_other_outputs(struct server *s, struct wlr_box *boxes, int max);

#include "volume.h"

/* ---- installed applications (apps.c) ---- */

struct app {
	char *id, *name, *exec, *icon_name, *keywords;
	cairo_surface_t *icon;
	bool icon_tried;
};

void apps_load(void);
struct app *apps_all(int *n);              /* sorted by name */
cairo_surface_t *app_icon(struct theme *t, struct app *a); /* 32px; XP's generic icon if none */
bool app_matches(struct app *a, const char *query);

/* ---- shell (shell.c): desktop, taskbar, start menu ---- */

struct shell *shell_create(struct server *s);
void shell_layout(struct shell *sh);   /* output size changed */
void shell_views_changed(struct shell *sh);
/* pointer events in layout coords; return true when the shell took the event */
bool shell_motion(struct shell *sh, double x, double y, bool over_view);
bool shell_button(struct shell *sh, double x, double y, bool pressed, bool dbl, bool over_view);
void shell_toggle_start(struct shell *sh);
/* Windows 10: Task View (every window as a live picture) and Action Center */
void shell_task_view(struct shell *sh);
void shell_action_center(struct shell *sh, bool open);
bool shell_overlay_open(struct shell *sh);
void shell_close_overlays(struct shell *sh);
cairo_surface_t *view_snapshot(struct view *v);
bool shell_start_open(struct shell *sh);
/* keys and wheel while the Start menu is open; true when used */
bool shell_key(struct shell *sh, uint32_t sym, const char *utf8);
bool shell_axis(struct shell *sh, double x, double y, double delta);
void shell_reconfigure(struct shell *sh); /* options changed */
/* volume and brightness keys: step in percent, or toggle mute */
void shell_volume_key(struct shell *sh, int step, bool toggle_mute);
void shell_brightness_key(struct shell *sh, int step);

/* ---- session (session.c): welcome screen, log off, Turn Off dialog, sounds ---- */

struct session *session_create(struct server *s, bool autologin);
void session_layout(struct session *se);
/* true while a full-screen session UI owns the pointer and keyboard */
bool session_active(struct session *se);
void session_motion(struct session *se, double x, double y);
void session_button(struct session *se, double x, double y, bool pressed);
void session_key(struct session *se, uint32_t sym, const char *utf8);
void session_logoff(struct session *se);
void session_turnoff_dialog(struct session *se);
/* XP sound events: startup, logon, logoff, shutdown, minimize, restore, ... */
void sound_play(struct server *s, const char *name);

#endif
