#ifndef EXPEDE_UI_H
#define EXPEDE_UI_H

/* A small toolkit for Expede's own apps (Explorer, Control Panel): Wayland
 * windows drawn with cairo and the Windows theme bitmaps, immediate-mode
 * widgets with hit rectangles, keyboard with repeat, HiDPI. */
#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>
#include <xkbcommon/xkbcommon.h>
#include "theme.h"

enum { UI_SHIFT = 1, UI_CTRL = 2, UI_ALT = 4 };
enum { UI_LEFT = 1, UI_RIGHT = 3, UI_MIDDLE = 2 };

struct ui_rect { int id, x, y, w, h; };

struct ui_win {
	void *data;
	int w, h, scale;
	bool alive, configured, resizable;
	int hot, pressed;
	struct ui_rect rects[1024];
	int n_rects;
	/* callbacks; any may be NULL */
	void (*paint)(struct ui_win *w, cairo_t *cr);
	/* press/release on a widget id (0 = none); clicks is 2 for a double click */
	void (*button)(struct ui_win *w, int id, int button, bool pressed, double x, double y, int clicks, int mods);
	void (*motion)(struct ui_win *w, double x, double y, bool held);
	void (*key)(struct ui_win *w, xkb_keysym_t sym, const char *utf8, int mods);
	void (*scroll)(struct ui_win *w, double dy, double x, double y);
	void (*close)(struct ui_win *w); /* the title bar's X; default destroys the window */
	struct ui_priv *priv;
};

extern struct theme ui_theme;

bool ui_init(const char *who);
struct ui_win *ui_window(const char *title, const char *app_id, int w, int h, bool resizable);
void ui_set_title(struct ui_win *w, const char *title);
void ui_destroy(struct ui_win *w);
void ui_redraw(struct ui_win *w);
int ui_run(void); /* until the last window closes */
void ui_quit(void);

/* widgets: draw and register a hit rectangle */
void ui_reg(struct ui_win *w, int id, int x, int y, int ww, int hh);
int ui_state(struct ui_win *w, int id); /* 0 normal, 1 hot, 2 pressed */
void ui_fill(cairo_t *cr, double x, double y, double w, double h, uint32_t rgb);
void ui_text(cairo_t *cr, const char *font, const char *s, double x, double y, double maxw, uint32_t rgb);
void ui_icon(cairo_t *cr, cairo_surface_t *s, double x, double y, double size);
void ui_button(struct ui_win *w, cairo_t *cr, int id, int x, int y, int bw, const char *label, bool disabled, bool def);
void ui_check(struct ui_win *w, cairo_t *cr, int id, int x, int y, bool on, bool radio, const char *label);
void ui_group(cairo_t *cr, int x, int y, int gw, int gh, const char *title);
/* a one-line text field; *caret is a byte offset into text */
void ui_field(struct ui_win *w, cairo_t *cr, int id, int x, int y, int fw, int fh, const char *text, bool focused, int caret);
/* edit a field's text for a key; true when the key was used */
bool ui_edit(char *text, size_t cap, int *caret, xkb_keysym_t sym, const char *utf8, int mods);
/* XP context menu at (x, y); items with label NULL are separators; ids from base */
struct ui_menu_item { const char *label; bool bold, disabled; };
void ui_menu(struct ui_win *w, cairo_t *cr, int base_id, int x, int y, const struct ui_menu_item *items, int n);
int ui_menu_height(const struct ui_menu_item *items, int n);

/* run a shell command in the background */
void spawn(const char *cmd);
/* single-quote s for /bin/sh into out */
const char *ui_shell_quote(const char *s, char *out, size_t cap);
const char *ui_assets_dir(void);

#endif
