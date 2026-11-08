/* Windows 10 Settings, for when Expede looks like Windows 10.
 *
 * Included by settings.c, so it shares that file's state and actions: the
 * home page's category grid and each category's navigation pane are drawn
 * here, and a page's content is the classic applet's, drawn in Windows 10's
 * style (section headings and switches; see ui_w10_look) and applied as
 * soon as it's changed, as Windows 10 does. */

#define W10_W 1000
#define W10_H 680
#define W10_NAV 300

enum {
	ID10_HOME = 7000, ID10_SEARCH, ID10_BACK,
	ID10_CAT = 7100,    /* + category */
	ID10_PAGE = 7200,   /* + page */
	ID10_RESULT = 7300, /* + search result */
	ID10_DD_OPT = 7400, /* + option of the open dropdown */
	ID10_DD = 7500,     /* dropdowns: */
	DD_FIT = ID10_DD, DD_MODE, DD_SCALE, DD_RES, DD_ROT, DD_MULTI, DD_PART0,
	ID10_THUMB = 7600,  /* + wallpaper */
	ID10_STYLE = 7700,  /* + 0 XP, 1 7, 2 10 */
	ID10_ACCENT = 7710, /* + accent */
	ID10_TRANSP, ID10_ACCENT_BARS,
};

/* Settings > category > page, each page one classic applet tab */
static const struct page10 { int cat; const char *title, *glyph; enum kind kind; int tab; } pages10[] = {
	{0, "Display", "", K_DISPLAY, 2}, {0, "Scale and text size", "", K_DISPLAY, 1},
	{0, "Sound", "", K_SOUNDS, 0}, {0, "Sound devices", "", K_SOUNDS, 2},
	{0, "Power & sleep", "", K_POWER, 0}, {0, "Battery", "", K_POWER, 1},
	{0, "Storage", "", K_CLEANUP, 0}, {0, "About", "", K_SYSTEM, 0},
	{1, "Bluetooth & other devices", "", K_BLUETOOTH, 0}, {1, "Printers & scanners", "", K_PRINTERS, 0},
	{1, "Mouse", "", K_MOUSE, 0}, {1, "Touchpad", "", K_MOUSE, 1}, {1, "Typing", "", K_KEYBOARD, 0},
	{2, "Wi-Fi", "", K_NETWORK, 0}, {2, "Status", "", K_NETWORK, 1},
	{3, "Background", "", K_DISPLAY, 0}, {3, "Colors", "", K_PERSONALIZE, 0},
	{3, "Themes", "", K_PERSONALIZE, 1}, {3, "Start", "", K_TASKBAR, 1}, {3, "Taskbar", "", K_TASKBAR, 0},
	{4, "Apps & features", "", K_PROGRAMS, 0}, {4, "Default apps", "", K_DEFAULTS, 0},
	{5, "Your info", "", K_USERS, 0},
	{6, "Date & time", "", K_DATETIME, 0}, {6, "Region", "", K_DATETIME, 1},
	{6, "Language", "", K_DATETIME, 2},
	{7, "Display", "", K_ACCESS, 0}, {7, "Mouse pointer", "", K_ACCESS, 1},
	{8, "Windows Update", "", K_UPDATES, 0},
};
#define N_PAGES10 ((int)(sizeof(pages10) / sizeof(*pages10)))

static const struct { const char *name, *desc, *glyph; } cats10[] = {
	{"System", "Display, sound, power", ""}, {"Devices", "Bluetooth, printers, mouse", ""},
	{"Network & Internet", "Wi-Fi, airplane mode", ""}, {"Personalization", "Background, colors, themes", ""},
	{"Apps", "Uninstall, defaults", ""}, {"Accounts", "Your account picture", ""},
	{"Time & Language", "Date, region, keyboard", ""}, {"Ease of Access", "Text size, mouse pointer", ""},
	{"Update & Security", "Windows Update", ""},
};
#define N_CATS10 ((int)(sizeof(cats10) / sizeof(*cats10)))

static int page10 = -1;      /* -1: the home page */
static struct sheet w10_sheet; /* the page's applet state, for the shared handlers */
static char search10[64];
static int search10_caret, results10[16], n_results10;
static bool search10_focus;

static void set_rgb10(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
}

static void glyph10(cairo_t *cr, const char *g, double pt, double cx, double cy, uint32_t c) {
	char font[48];
	snprintf(font, sizeof(font), "Segoe MDL2 Assets %.1f", pt);
	int w = text_width(font, g);
	draw_text(cr, font, g, cx - w / 2.0, cy - pt * 96 / 72 * 0.62, 0, c, 0, 0, 0, false);
}

static void open_page10(int p) {
	page10 = p;
	search10[0] = 0;
	search10_caret = 0;
	search10_focus = false;
	n_results10 = 0;
	if (p >= 0) {
		w10_sheet = (struct sheet){.kind = pages10[p].kind, .tab = pages10[p].tab, .win = w10_win};
		refresh_kind(pages10[p].kind);
	}
	redraw_all();
}

/* "Find a setting": page titles and category names that contain what's typed */
static void search10_update(void) {
	n_results10 = 0;
	for (int i = 0; i < N_PAGES10 && n_results10 < 8 && search10[0]; i++)
		if (strcasestr(pages10[i].title, search10) || strcasestr(cats10[pages10[i].cat].name, search10))
			results10[n_results10++] = i;
}

static void search_box10(struct ui_win *w, cairo_t *cr, int x, int y, int bw) {
	ui_fill(cr, x, y, bw, 32, search10_focus ? ui_accent : 0x999999);
	ui_fill(cr, x + 1, y + 1, bw - 2, 30, 0xffffff);
	if (search10[0] || search10_focus) {
		int tw = draw_text(cr, "Segoe UI 10", search10, x + 10, y + 7, bw - 44, 0x000000, 0, 0, 0, false);
		if (search10_focus)
			ui_fill(cr, x + 11 + tw, y + 8, 1, 16, 0x000000);
	} else {
		draw_text(cr, "Segoe UI 10", "Find a setting", x + 10, y + 7, 0, 0x767676, 0, 0, 0, false);
	}
	glyph10(cr, "", 10, x + bw - 18, y + 16, 0x000000);
	ui_reg(w, ID10_SEARCH, x, y, bw, 32);
	for (int i = 0; i < n_results10; i++) {
		int ry = y + 34 + i * 36;
		bool hot = w->hot == ID10_RESULT + i;
		ui_fill(cr, x, ry, bw, 36, hot ? 0xe5e5e5 : 0xf2f2f2);
		glyph10(cr, pages10[results10[i]].glyph, 11, x + 20, ry + 18, 0x000000);
		draw_text(cr, "Segoe UI 10", pages10[results10[i]].title, x + 44, ry + 9, bw - 52, 0x000000, 0, 0, 0, false);
		ui_reg(w, ID10_RESULT + i, x, ry, bw, 36);
	}
}

static void paint_home10(struct ui_win *w, cairo_t *cr) {
	int tw = text_width("Segoe UI Light 24", "Windows Settings");
	draw_text(cr, "Segoe UI Light 24", "Windows Settings", (w->w - tw) / 2, 36, 0, 0x000000, 0, 0, 0, false);
	/* the categories, three across */
	int cw = 290, ch = 96, cols = 3, x0 = (w->w - cols * cw) / 2, y0 = 170;
	for (int i = 0; i < N_CATS10; i++) {
		int x = x0 + (i % cols) * cw, y = y0 + (i / cols) * ch;
		bool hot = w->hot == ID10_CAT + i;
		if (hot) {
			ui_fill(cr, x, y, cw - 10, ch - 10, 0x999999);
			ui_fill(cr, x + 1, y + 1, cw - 12, ch - 12, 0xffffff);
		}
		glyph10(cr, cats10[i].glyph, 20, x + 36, y + 40, ui_accent);
		draw_text(cr, "Segoe UI 11", cats10[i].name, x + 72, y + 18, cw - 90, 0x000000, 0, 0, 0, false);
		draw_text(cr, "Segoe UI 9", cats10[i].desc, x + 72, y + 42, cw - 90, 0x767676, 0, 0, 0, false);
		ui_reg(w, ID10_CAT + i, x, y, cw - 10, ch - 10);
	}
	/* the search box goes last: its results drop over the grid */
	search_box10(w, cr, (w->w - 360) / 2, 100, 360);
}

/* a page's classic content, in Windows 10's layout */
static void paint_content10(struct ui_win *w, cairo_t *cr, int x0, int y0) {
	struct sheet *s = &w10_sheet;
	s->win = w;
	cairo_save(cr);
	cairo_rectangle(cr, W10_NAV, 0, w->w - W10_NAV, w->h);
	cairo_clip(cr);
	/* classic pages start below their tab strip (y 40) */
	cairo_translate(cr, x0 - 16, y0 - 40);
	ui_set_origin(x0 - 16, y0 - 40);
	ui_w10_look = true;
	switch (s->kind) {
	case K_DISPLAY: paint_display(s, cr); break;
	case K_PERSONALIZE: paint_personalize(s, cr); break;
	case K_TASKBAR: paint_taskbar(s, cr); break;
	case K_NETWORK: paint_network(s, cr); break;
	case K_PROGRAMS: paint_programs(s, cr); break;
	case K_DEFAULTS: paint_defaults(s, cr); break;
	case K_SOUNDS: paint_sounds(s, cr); break;
	case K_SYSTEM: paint_system(s, cr); break;
	case K_POWER: paint_power(s, cr); break;
	case K_MOUSE: paint_mouse(s, cr); break;
	case K_BLUETOOTH: paint_bluetooth(s, cr); break;
	case K_PRINTERS: paint_printers(s, cr); break;
	case K_KEYBOARD: paint_keyboard(s, cr); break;
	case K_ACCESS: paint_access(s, cr); break;
	case K_CLEANUP: paint_cleanup(s, cr); break;
	case K_UPDATES: paint_updates(s, cr); break;
	case K_USERS: paint_users(s, cr); break;
	case K_DATETIME: paint_datetime(s, cr); break;
	default: break;
	}
	ui_w10_look = false;
	ui_set_origin(0, 0);
	cairo_restore(cr);
}

/* ---- Windows 10's own controls ---- */

static int dd_open; /* the dropdown that's open, 0 for none */
static struct { int id, x, y, w, n, cur; const char *opts[64]; } dd_list;

/* a closed dropdown: white box, grey border, the chevron on the right */
static void dd10(struct ui_win *w, cairo_t *cr, int id, int x, int y, int wd, const char *label) {
	bool hot = w->hot == id || dd_open == id;
	ui_fill(cr, x, y, wd, 32, hot ? 0x666666 : 0x999999);
	ui_fill(cr, x + 2, y + 2, wd - 4, 28, 0xffffff);
	draw_text(cr, "Segoe UI 10", label, x + 10, y + 6, wd - 40, 0x000000, 0, 0, 0, false);
	glyph10(cr, "\uE70D", 8, x + wd - 18, y + 16, 0x000000);
	ui_reg(w, id, x, y, wd, 32);
}

/* remember an open dropdown's list; it's drawn last, over everything */
static void dd10_options(int id, int x, int y, int wd, const char *const *opts, int n, int cur) {
	if (dd_open != id)
		return;
	dd_list.id = id;
	dd_list.x = x;
	dd_list.y = y + 32;
	dd_list.w = wd;
	dd_list.n = n > 64 ? 64 : n;
	dd_list.cur = cur;
	for (int i = 0; i < dd_list.n; i++)
		dd_list.opts[i] = opts[i];
}

static void dd10_draw(struct ui_win *w, cairo_t *cr) {
	if (!dd_open || dd_list.id != dd_open)
		return;
	int rows = dd_list.n > 10 ? 10 : dd_list.n, y = dd_list.y;
	if (y + rows * 36 > w->h - 8)
		y = w->h - 8 - rows * 36;
	ui_fill(cr, dd_list.x - 1, y - 1, dd_list.w + 2, rows * 36 + 2, 0xcccccc);
	ui_fill(cr, dd_list.x, y, dd_list.w, rows * 36, 0xf2f2f2);
	int first = dd_list.cur >= rows ? dd_list.cur - rows + 1 : 0;
	for (int r = 0; r < rows && first + r < dd_list.n; r++) {
		int i = first + r, ry = y + r * 36;
		bool hot = w->hot == ID10_DD_OPT + i;
		if (i == dd_list.cur || hot)
			ui_fill(cr, dd_list.x, ry, dd_list.w, 36, hot ? 0xdadada : 0xe5e5e5);
		if (i == dd_list.cur)
			ui_fill(cr, dd_list.x, ry + 8, 3, 20, ui_accent);
		draw_text(cr, "Segoe UI 10", dd_list.opts[i], dd_list.x + 12, ry + 8, dd_list.w - 20, 0x000000, 0, 0, 0, false);
		ui_reg(w, ID10_DD_OPT + i, dd_list.x, ry, dd_list.w, 36);
	}
}

static const char *fits10[] = {"Fill", "Center", "Tile"};
static const char *fit_values[] = {"stretch", "center", "tile"};
static const char *modes10[] = {"Light", "Dark"};
static const char *scales10[] = {"Automatic (Recommended)", "100%", "125%", "150%", "175%", "200%"};
static const int scale_pcts10[] = {0, 100, 125, 150, 175, 200};
static const char *rots10[] = {"Landscape", "Portrait", "Landscape (flipped)", "Portrait (flipped)"};
static const char *multi10[] = {"Extend these displays", "Duplicate these displays"};
static const char *parts10[] = {"Same as Windows version", "Windows XP", "Windows 7", "Windows 10"};

static const char *cur_wallpaper10(void) {
	return cfg.wall_set ? cfg.wallpaper : "Windows 10.jpg";
}

static cairo_surface_t *thumb10(int i) {
	if (wallpapers[i].img || !wallpapers[i].value[0])
		return wallpapers[i].img;
	char path[900];
	snprintf(path, sizeof(path), "%s/wallpapers/%s", ui_theme.dir, wallpapers[i].value);
	cairo_surface_t *full = theme_load_image(path);
	if (!full)
		return NULL;
	/* a square crop, as Windows 10 shows them */
	int fw = cairo_image_surface_get_width(full), fh = cairo_image_surface_get_height(full), side = fw < fh ? fw : fh;
	cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 96, 96);
	cairo_t *cr = cairo_create(t);
	cairo_scale(cr, 96.0 / side, 96.0 / side);
	cairo_set_source_surface(cr, full, -(fw - side) / 2.0, -(fh - side) / 2.0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(full);
	wallpapers[i].img = t;
	return t;
}

static void page_background10(struct ui_win *w, cairo_t *cr, int x, int y) {
	/* the preview: the desktop picture with a small window on it */
	int pw = 330, ph = 186;
	ui_fill(cr, x, y, pw, ph, 0x000000);
	char path[900];
	snprintf(path, sizeof(path), "%s/wallpapers/%s", ui_theme.dir, cur_wallpaper10());
	static cairo_surface_t *preview_img;
	static char preview_path[900];
	if (strcmp(preview_path, path)) {
		if (preview_img)
			cairo_surface_destroy(preview_img);
		preview_img = theme_load_image(path);
		snprintf(preview_path, sizeof(preview_path), "%s", path);
	}
	if (preview_img) {
		cairo_save(cr);
		cairo_rectangle(cr, x, y, pw, ph);
		cairo_clip(cr);
		double k = fmax((double)pw / cairo_image_surface_get_width(preview_img),
			(double)ph / cairo_image_surface_get_height(preview_img));
		cairo_translate(cr, x, y);
		cairo_scale(cr, k, k);
		cairo_set_source_surface(cr, preview_img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	ui_fill(cr, x + 60, y + 50, 150, 90, 0xffffff);
	ui_fill(cr, x + 60, y + 50, 150, 14, ui_accent);
	draw_text(cr, "Segoe UI 8", "Aa", x + 74, y + 74, 0, 0x000000, 0, 0, 0, false);
	ui_fill(cr, x, y + ph - 12, pw, 12, 0x101010);
	y += ph + 24;
	draw_text(cr, "Segoe UI 10", "Choose your picture", x, y, 0, 0x000000, 0, 0, 0, false);
	y += 28;
	int shown = 0;
	for (int i = 1; i < n_wallpapers && shown < 10; i++) {
		if (wallpapers[i].value[0] == '.')
			continue;
		int tx = x + (shown % 5) * 104, ty = y + (shown / 5) * 104;
		cairo_surface_t *t = thumb10(i);
		if (!strcmp(wallpapers[i].value, cur_wallpaper10()))
			ui_fill(cr, tx - 3, ty - 3, 102, 102, ui_accent);
		if (t)
			draw_surface(cr, t, tx, ty);
		ui_reg(w, ID10_THUMB + i, tx, ty, 96, 96);
		shown++;
	}
	y += ((shown + 4) / 5) * 104 + 16;
	draw_text(cr, "Segoe UI 10", "Choose a fit", x, y, 0, 0x000000, 0, 0, 0, false);
	int fit = !strcmp(cfg.wall_style, "center") ? 1 : !strcmp(cfg.wall_style, "tile") ? 2 : 0;
	dd10(w, cr, DD_FIT, x, y + 26, 260, fits10[fit]);
	dd10_options(DD_FIT, x, y + 26, 260, fits10, 3, fit);
}

static void accent_grid10(struct ui_win *w, cairo_t *cr, int x, int y) {
	for (int i = 0; i < N_ACCENTS; i++) {
		int sx = x + (i % 8) * 50, sy = y + (i / 8) * 50;
		bool sel = !strcasecmp(cfg.accent, accents[i].hex);
		ui_fill(cr, sx, sy, 46, 46, accents[i].rgb);
		if (sel) {
			cairo_set_source_rgb(cr, 1, 1, 1);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, sx + 2, sy + 2, 42, 42);
			cairo_stroke(cr);
			glyph10(cr, "\uE73E", 10, sx + 23, sy + 23, 0xffffff);
		} else if (w->hot == ID10_ACCENT + i) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_set_line_width(cr, 2);
			cairo_rectangle(cr, sx + 1, sy + 1, 44, 44);
			cairo_stroke(cr);
		}
		ui_reg(w, ID10_ACCENT + i, sx, sy, 46, 46);
	}
}

static void page_colors10(struct ui_win *w, cairo_t *cr, int x, int y) {
	draw_text(cr, "Segoe UI 10", "Choose your color", x, y, 0, 0x000000, 0, 0, 0, false);
	dd10(w, cr, DD_MODE, x, y + 26, 260, modes10[cfg.dark ? 1 : 0]);
	int ddy = y + 26;
	y += 82;
	ui_w10_look = true;
	ui_check(w, cr, ID10_TRANSP, x, y, cfg.transparency, false, "Transparency effects");
	ui_w10_look = false;
	y += 44;
	draw_text(cr, "Segoe UI Semibold 13", "Choose your accent color", x, y, 0, 0x000000, 0, 0, 0, false);
	y += 34;
	draw_text(cr, "Segoe UI 10", "Windows colors", x, y, 0, 0x000000, 0, 0, 0, false);
	accent_grid10(w, cr, x, y + 26);
	y += 26 + 100 + 20;
	draw_text(cr, "Segoe UI 10", "Show accent color on the following surfaces", x, y, 0, 0x000000, 0, 0, 0, false);
	ui_check(w, cr, ID10_ACCENT_BARS, x, y + 28, cfg.accent_titlebars, false, "Title bars and window borders");
	dd10_options(DD_MODE, x, ddy, 260, modes10, 2, cfg.dark ? 1 : 0);
}

static void page_themes10(struct ui_win *w, cairo_t *cr, int x, int y) {
	draw_text(cr, "Segoe UI Semibold 13", "Change theme", x, y, 0, 0x000000, 0, 0, 0, false);
	y += 34;
	static const char *names[3] = {"Windows XP", "Windows 7", "Windows 10"};
	static const char *walls[3] = {"Bliss.jpg", "Windows 7.jpg", "Windows 10.jpg"};
	static cairo_surface_t *thumbs[3];
	int st = style_num(cfg.style), sel = st == 10 ? 2 : st == 7 ? 1 : 0;
	for (int i = 0; i < 3; i++) {
		int cx = x + i * 190;
		if (!thumbs[i]) {
			char path[900], base[600];
			snprintf(base, sizeof(base), "%s", ui_theme.dir);
			char *sl = strrchr(base, '/');
			if (sl)
				*sl = 0;
			static const char *dirs[3] = {"xp", "7", "10"};
			snprintf(path, sizeof(path), "%s/%s/wallpapers/%s", base, dirs[i], walls[i]);
			cairo_surface_t *full = theme_load_image(path);
			if (full) {
				thumbs[i] = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 176, 110);
				cairo_t *t = cairo_create(thumbs[i]);
				cairo_scale(t, 176.0 / cairo_image_surface_get_width(full), 110.0 / cairo_image_surface_get_height(full));
				cairo_set_source_surface(t, full, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(t), CAIRO_FILTER_GOOD);
				cairo_paint(t);
				cairo_destroy(t);
				cairo_surface_destroy(full);
			}
		}
		if (i == sel || w->hot == ID10_STYLE + i)
			ui_fill(cr, cx - 3, y - 3, 182, 116, i == sel ? ui_accent : 0x999999);
		if (thumbs[i])
			draw_surface(cr, thumbs[i], cx, y);
		else
			ui_fill(cr, cx, y, 176, 110, 0xcccccc);
		draw_text(cr, "Segoe UI 10", names[i], cx, y + 120, 0, 0x000000, 0, 0, 0, false);
		ui_reg(w, ID10_STYLE + i, cx, y, 176, 140);
	}
	y += 170;
	draw_text(cr, "Segoe UI Semibold 13", "Mix parts of each version", x, y, 0, 0x000000, 0, 0, 0, false);
	y += 34;
	static const char *pnames[7] = {"Window frames", "Taskbar", "Start menu", "Sign-in screen", "Sounds",
		"Mouse pointers", "Icons"};
	int open_y = 0, open_p = -1, open_cur = 0;
	for (int p = 0; p < 7; p++) {
		int ry = y + p * 40, sn = style_num(part_val(p)), cur = sn == 10 ? 3 : sn == 7 ? 2 : sn == 0 ? 1 : 0;
		draw_text(cr, "Segoe UI 10", pnames[p], x, ry + 6, 0, 0x000000, 0, 0, 0, false);
		dd10(w, cr, DD_PART0 + p, x + 170, ry, 260, parts10[cur]);
		if (dd_open == DD_PART0 + p) {
			open_y = ry;
			open_p = p;
			open_cur = cur;
		}
	}
	if (open_p >= 0)
		dd10_options(DD_PART0 + open_p, x + 170, open_y, 260, parts10, 4, open_cur);
}

static void page_display10(struct ui_win *w, cairo_t *cr, int x, int y) {
	if (n_screens > 1) {
		draw_text(cr, "Segoe UI Semibold 13", "Rearrange your displays", x, y, 0, 0x000000, 0, 0, 0, false);
		y += 30;
		int x0, y0;
		double k;
		struct ui_rect area = {.x = x, .y = y, .w = 520, .h = 110};
		ui_fill(cr, x, y, 520, 110, 0xf2f2f2);
		layout_scale(area, &x0, &y0, &k);
		for (int i = 0; i < n_screens; i++) {
			struct screen *sc = &screens[i];
			int dx = x + (int)((sc->x - x0) * k), dy = y + (int)((sc->y - y0) * k);
			ui_fill(cr, dx, dy, (int)(sc->w * k), (int)(sc->h * k), i == screen_sel ? ui_accent : 0x767676);
			char num[4];
			snprintf(num, sizeof(num), "%d", i + 1);
			draw_text(cr, "Segoe UI Light 20", num, dx + (int)(sc->w * k) / 2 - 7, dy + (int)(sc->h * k) / 2 - 16, 0,
				0xffffff, 0, 0, 0, false);
			ui_reg(w, ID_SCREEN + i, dx, dy, (int)(sc->w * k), (int)(sc->h * k));
		}
		y += 130;
	}
	draw_text(cr, "Segoe UI Semibold 13", "Scale and layout", x, y, 0, 0x000000, 0, 0, 0, false);
	y += 34;
	int si = 0;
	for (int i = 0; i < 6; i++)
		if (cfg.scale_pct == scale_pcts10[i])
			si = i;
	draw_text(cr, "Segoe UI 10", "Change the size of text, apps, and other items", x, y, 0, 0x000000, 0, 0, 0, false);
	dd10(w, cr, DD_SCALE, x, y + 26, 260, scales10[si]);
	int y_scale = y + 26;
	y += 76;
	static const char *modes[64];
	struct screen *sc = n_screens ? &screens[screen_sel] : NULL;
	int ci = sc ? out_index(sc->name, false) : -1, mi = 0, nm = sc ? sc->n_modes : 0;
	const char *want = sc ? (ci >= 0 && cfg.out[ci].mode[0] ? cfg.out[ci].mode : sc->cur) : "";
	for (int i = 0; i < nm; i++) {
		modes[i] = sc->modes[i];
		if (!strcmp(sc->modes[i], want))
			mi = i;
	}
	draw_text(cr, "Segoe UI 10", "Display resolution", x, y, 0, 0x000000, 0, 0, 0, false);
	dd10(w, cr, DD_RES, x, y + 26, 260, nm ? modes[mi] : "(unknown)");
	int y_res = y + 26;
	y += 76;
	int rot = ci >= 0 ? cfg.out[ci].rotation / 90 : 0;
	draw_text(cr, "Segoe UI 10", "Display orientation", x, y, 0, 0x000000, 0, 0, 0, false);
	dd10(w, cr, DD_ROT, x, y + 26, 260, rots10[rot & 3]);
	int y_rot = y + 26;
	y += 76;
	int y_multi = 0;
	if (n_screens > 1) {
		draw_text(cr, "Segoe UI Semibold 13", "Multiple displays", x, y, 0, 0x000000, 0, 0, 0, false);
		dd10(w, cr, DD_MULTI, x, y + 34, 260, multi10[cfg.duplicate]);
		y_multi = y + 34;
		ui_check(w, cr, ID_PRIMARY, x, y + 84, sc && (ci >= 0 ? cfg.out[ci].primary : sc->primary), false,
			"Make this my main display");
	}
	dd10_options(DD_SCALE, x, y_scale, 260, scales10, 6, si);
	dd10_options(DD_RES, x, y_res, 260, modes, nm, mi);
	dd10_options(DD_ROT, x, y_rot, 260, rots10, 4, rot & 3);
	if (y_multi)
		dd10_options(DD_MULTI, x, y_multi, 260, multi10, 2, cfg.duplicate);
}

/* a dropdown choice was made */
static void dd10_pick(int id, int i) {
	switch (id) {
	case DD_FIT: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "%s", fit_values[i]); break;
	case DD_MODE: cfg.dark = i == 1; break;
	case DD_SCALE: cfg.scale_pct = scale_pcts10[i]; break;
	case DD_RES: {
		struct screen *sc = &screens[screen_sel];
		int c = out_index(sc->name, true);
		if (c >= 0 && i < sc->n_modes)
			snprintf(cfg.out[c].mode, sizeof(cfg.out[c].mode), "%s", sc->modes[i]);
		break;
	}
	case DD_ROT: {
		int c = out_index(screens[screen_sel].name, true);
		if (c >= 0)
			cfg.out[c].rotation = i * 90;
		break;
	}
	case DD_MULTI: cfg.duplicate = i == 1; break;
	default:
		if (id >= DD_PART0 && id < DD_PART0 + 7) {
			static const char *vals[4] = {"auto", "xp", "7", "10"};
			snprintf(part_val(id - DD_PART0), 16, "%s", vals[i]);
		}
	}
}

/* pages drawn the Windows 10 way rather than from the classic applet */
static bool native_page10(struct ui_win *w, cairo_t *cr, const struct page10 *pg, int x, int y) {
	if (pg->kind == K_DISPLAY && pg->tab == 0)
		page_background10(w, cr, x, y);
	else if (pg->kind == K_PERSONALIZE && pg->tab == 0)
		page_colors10(w, cr, x, y);
	else if (pg->kind == K_PERSONALIZE && pg->tab == 1)
		page_themes10(w, cr, x, y);
	else if (pg->kind == K_DISPLAY && pg->tab == 2)
		page_display10(w, cr, x, y);
	else
		return false;
	return true;
}

static void paint_page10(struct ui_win *w, cairo_t *cr) {
	const struct page10 *pg = &pages10[page10];
	/* the navigation pane: Home, search, the category's pages */
	ui_fill(cr, 0, 0, W10_NAV, w->h, 0xe6e6e6);
	bool hot = w->hot == ID10_HOME;
	if (hot)
		ui_fill(cr, 0, 12, W10_NAV, 40, 0xd4d4d4);
	glyph10(cr, "", 11, 30, 32, 0x000000);
	draw_text(cr, "Segoe UI 10", "Home", 58, 23, 0, 0x000000, 0, 0, 0, false);
	ui_reg(w, ID10_HOME, 0, 12, W10_NAV, 40);
	draw_text(cr, "Segoe UI Semibold 10", cats10[pg->cat].name, 20, 112, 0, 0x000000, 0, 0, 0, false);
	int y = 148;
	for (int i = 0; i < N_PAGES10; i++) {
		if (pages10[i].cat != pg->cat)
			continue;
		bool sel = i == page10, h = w->hot == ID10_PAGE + i;
		if (sel || h)
			ui_fill(cr, 0, y, W10_NAV, 40, sel ? 0xcccccc : 0xd4d4d4);
		if (sel)
			ui_fill(cr, 0, y + 8, 4, 24, ui_accent);
		glyph10(cr, pages10[i].glyph, 11, 30, y + 20, 0x000000);
		draw_text(cr, "Segoe UI 10", pages10[i].title, 58, y + 11, W10_NAV - 66, 0x000000, 0, 0, 0, false);
		ui_reg(w, ID10_PAGE + i, 0, y, W10_NAV, 40);
		y += 40;
	}
	/* the page */
	draw_text(cr, "Segoe UI Light 24", pg->title, W10_NAV + 40, 26, 0, 0x000000, 0, 0, 0, false);
	dd_list.id = 0;
	ui_w10_look = true; /* switches, in the native pages too */
	bool native = native_page10(w, cr, pg, W10_NAV + 40, 92);
	ui_w10_look = false;
	if (!native)
		paint_content10(w, cr, W10_NAV + 40, 86);
	search_box10(w, cr, 20, 64, W10_NAV - 40);
	dd10_draw(w, cr);
}

static void w10_paint(struct ui_win *w, cairo_t *cr) {
	ui_fill(cr, 0, 0, w->w, w->h, 0xffffff);
	theme_use_fonts(&ui_theme);
	if (page10 < 0)
		paint_home10(w, cr);
	else
		paint_page10(w, cr);
}

/* Windows 10 applies a setting the moment it changes */
static void w10_apply(void) {
	if (memcmp(&cfg, &saved, sizeof(cfg)))
		config_save();
	redraw_all();
}

static void w10_button(struct ui_win *w, int id, int button, bool pressed, double x, double y, int clicks, int mods) {
	if (button == UI_LEFT && pressed && id != ID10_SEARCH && id < ID10_HOME)
		search10_focus = false;
	if (id >= ID10_HOME) {
		if (pressed || button != UI_LEFT || id != w->pressed)
			return;
		if (id >= ID10_DD_OPT && id < ID10_DD_OPT + 64) {
			dd10_pick(dd_open, id - ID10_DD_OPT);
			dd_open = 0;
			w10_apply();
			return;
		}
		if (id >= ID10_DD && id < ID10_THUMB) {
			dd_open = dd_open == id ? 0 : id;
			redraw_all();
			return;
		}
		dd_open = 0;
		if (id >= ID10_THUMB && id < ID10_THUMB + n_wallpapers) {
			snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", wallpapers[id - ID10_THUMB].value);
			cfg.wall_set = true;
			w10_apply();
			return;
		}
		if (id >= ID10_STYLE && id < ID10_STYLE + 3) {
			static const char *v[3] = {"xp", "7", "10"};
			snprintf(cfg.style, sizeof(cfg.style), "%s", v[id - ID10_STYLE]);
			cfg.wall_set = cfg.wall_set && strcmp(cfg.wallpaper, "Bliss.jpg") && strcmp(cfg.wallpaper, "Windows 7.jpg") &&
				strcmp(cfg.wallpaper, "Windows 10.jpg");
			w10_apply();
			return;
		}
		if (id >= ID10_ACCENT && id < ID10_ACCENT + N_ACCENTS) {
			snprintf(cfg.accent, sizeof(cfg.accent), "%s", accents[id - ID10_ACCENT].hex);
			ui_accent = accents[id - ID10_ACCENT].rgb;
			w10_apply();
			return;
		}
		if (id == ID10_TRANSP || id == ID10_ACCENT_BARS) {
			if (id == ID10_TRANSP)
				cfg.transparency = !cfg.transparency;
			else
				cfg.accent_titlebars = !cfg.accent_titlebars;
			w10_apply();
			return;
		}
		if (id == ID10_HOME) {
			open_page10(-1);
		} else if (id == ID10_SEARCH) {
			search10_focus = true;
			redraw_all();
		} else if (id >= ID10_RESULT && id < ID10_RESULT + n_results10) {
			open_page10(results10[id - ID10_RESULT]);
		} else if (id >= ID10_PAGE && id < ID10_PAGE + N_PAGES10) {
			open_page10(id - ID10_PAGE);
		} else if (id >= ID10_CAT && id < ID10_CAT + N_CATS10) {
			for (int i = 0; i < N_PAGES10; i++)
				if (pages10[i].cat == id - ID10_CAT) {
					open_page10(i);
					break;
				}
		}
		return;
	}
	if (page10 < 0)
		return;
	if (pressed && dd_open) {
		dd_open = 0;
		redraw_all();
	}
	/* everything else is the page's own control: the classic applet handles it */
	sheet_button(w, id, button, pressed, x, y, clicks, mods);
	if (!pressed)
		w10_apply();
}

static void w10_motion(struct ui_win *w, double x, double y, bool held) {
	if (page10 >= 0)
		sheet_motion(w, x, y, held);
}

static void w10_scroll(struct ui_win *w, double dy, double x, double y) {
	if (page10 >= 0)
		sheet_scroll(w, dy, x, y);
}

static void w10_key(struct ui_win *w, xkb_keysym_t sym, const char *utf8, int mods) {
	if (search10_focus) {
		if (sym == XKB_KEY_Escape) {
			search10_focus = false;
			search10[0] = 0;
			n_results10 = 0;
		} else if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && n_results10) {
			open_page10(results10[0]);
			return;
		} else {
			ui_edit(search10, sizeof(search10), &search10_caret, sym, utf8, mods);
			search10_update();
		}
		redraw_all();
		return;
	}
	if (sym == XKB_KEY_Escape) {
		if (page10 >= 0)
			open_page10(-1);
		else
			ui_quit();
		return;
	}
	if (page10 >= 0) {
		enum kind k = w10_sheet.kind;
		/* Enter means OK on a classic dialog (it would close this window): only pages
		 * with a text box of their own get it */
		if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && k != K_PROGRAMS && k != K_NETWORK &&
				k != K_BLUETOOTH && k != K_KEYBOARD)
			return;
		sheet_key(w, sym, utf8, mods);
		w10_apply();
	} else if (utf8 && (unsigned char)utf8[0] >= 0x20) {
		/* typing on the home page searches, as in Windows 10 */
		search10_focus = true;
		ui_edit(search10, sizeof(search10), &search10_caret, sym, utf8, mods);
		search10_update();
		redraw_all();
	}
}

static void w10_close(struct ui_win *w) {
	ui_destroy(w);
	w10_win = NULL;
}

static void settings10_open(enum kind start) {
	ui_accent = (uint32_t)strtoul(cfg.accent, NULL, 16);
	w10_win = ui_window("Settings", "expede-settings", W10_W, W10_H, true);
	w10_win->data = &w10_sheet;
	w10_win->paint = w10_paint;
	w10_win->button = w10_button;
	w10_win->motion = w10_motion;
	w10_win->scroll = w10_scroll;
	w10_win->key = w10_key;
	w10_win->close = w10_close;
	int p = -1;
	for (int i = 0; i < N_PAGES10 && start != K_CP; i++)
		if (pages10[i].kind == start) {
			p = i;
			break;
		}
	open_page10(p);
}
