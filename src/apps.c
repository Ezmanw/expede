/* Installed applications, read from freedesktop .desktop files, for All
 * Programs and Start menu search. Icons are looked up in the usual theme
 * directories and loaded the first time they are drawn. */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "expede.h"

static struct app *apps;
static int n_apps, cap_apps;

static char *trim(char *s) {
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

/* "gimp-3.0 %U" -> "gimp-3.0": field codes are for file arguments we never pass */
static char *clean_exec(const char *exec) {
	char *out = malloc(strlen(exec) + 1), *o = out;
	for (const char *p = exec; *p; p++) {
		if (*p == '%' && p[1]) {
			if (p[1] == '%')
				*o++ = '%';
			p++;
			continue;
		}
		*o++ = *p;
	}
	*o = 0;
	char *t = trim(out);
	memmove(out, t, strlen(t) + 1);
	return out;
}

static bool have_app(const char *id) {
	for (int i = 0; i < n_apps; i++)
		if (!strcmp(apps[i].id, id))
			return true;
	return false;
}

static void parse_desktop(const char *path, const char *id) {
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[1024], *name = NULL, *exec = NULL, *icon = NULL, *keywords = NULL;
	bool in_entry = false, hidden = false, terminal = false, app = false;
	while (fgets(line, sizeof(line), f)) {
		char *l = trim(line);
		if (*l == '[') {
			in_entry = !strcmp(l, "[Desktop Entry]");
			continue;
		}
		if (!in_entry)
			continue;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *k = trim(l), *v = trim(eq + 1);
		if (!strcmp(k, "Name") && !name)
			name = strdup(v);
		else if (!strcmp(k, "Exec") && !exec)
			exec = clean_exec(v);
		else if (!strcmp(k, "Icon") && !icon)
			icon = strdup(v);
		else if (!strcmp(k, "Keywords") && !keywords)
			keywords = strdup(v);
		else if (!strcmp(k, "Type"))
			app = !strcmp(v, "Application");
		else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(v, "true"))
			hidden = true;
		else if (!strcmp(k, "Terminal") && !strcmp(v, "true"))
			terminal = true;
		else if (!strcmp(k, "OnlyShowIn") && !strstr(v, "Expede"))
			hidden = true;
	}
	fclose(f);
	if (!app || hidden || !name || !exec) {
		free(name); free(exec); free(icon); free(keywords);
		return;
	}
	if (terminal) {
		char *t;
		if (asprintf(&t, "foot -e %s", exec) >= 0) {
			free(exec);
			exec = t;
		}
	}
	if (n_apps == cap_apps) {
		cap_apps = cap_apps ? cap_apps * 2 : 64;
		apps = realloc(apps, cap_apps * sizeof(*apps));
	}
	apps[n_apps++] = (struct app){.id = strdup(id), .name = name, .exec = exec, .icon_name = icon,
		.keywords = keywords};
}

static void scan_dir(const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 9 || strcmp(e->d_name + n - 8, ".desktop") || have_app(e->d_name))
			continue;
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		parse_desktop(path, e->d_name);
	}
	closedir(d);
}

static int by_name(const void *a, const void *b) {
	return strcasecmp(((const struct app *)a)->name, ((const struct app *)b)->name);
}

static time_t newest_dir_mtime(void) {
	const char *dirs[] = {"/usr/share/applications", "/usr/local/share/applications",
		"/var/lib/flatpak/exports/share/applications"};
	time_t newest = 0;
	struct stat st;
	for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
		if (stat(dirs[i], &st) == 0 && st.st_mtime > newest)
			newest = st.st_mtime;
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/.local/share/applications", getenv("HOME"));
	if (stat(dir, &st) == 0 && st.st_mtime > newest)
		newest = st.st_mtime;
	return newest;
}

void apps_load(void) {
	/* installing or removing an app touches its directory; skip the rescan otherwise */
	static time_t loaded_at = -1;
	time_t m = newest_dir_mtime();
	if (m == loaded_at)
		return;
	loaded_at = m;
	for (int i = 0; i < n_apps; i++) {
		free(apps[i].id); free(apps[i].name); free(apps[i].exec);
		free(apps[i].icon_name); free(apps[i].keywords);
		if (apps[i].icon)
			cairo_surface_destroy(apps[i].icon);
	}
	n_apps = 0;
	char dir[512];
	const char *home = getenv("HOME");
	/* user entries first so they win over system ones with the same id */
	snprintf(dir, sizeof(dir), "%s/.local/share/applications", home);
	scan_dir(dir);
	scan_dir("/usr/local/share/applications");
	scan_dir("/usr/share/applications");
	scan_dir("/var/lib/flatpak/exports/share/applications");
	snprintf(dir, sizeof(dir), "%s/.local/share/flatpak/exports/share/applications", home);
	scan_dir(dir);
	qsort(apps, n_apps, sizeof(*apps), by_name);
}

struct app *apps_all(int *n) {
	*n = n_apps;
	return apps;
}

/* ---- icons ---- */

static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pb) {
	int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
	int n = gdk_pixbuf_get_n_channels(pb), rs = gdk_pixbuf_get_rowstride(pb);
	bool alpha = gdk_pixbuf_get_has_alpha(pb);
	const guchar *src = gdk_pixbuf_read_pixels(pb);
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	unsigned char *dst = cairo_image_surface_get_data(s);
	int ds = cairo_image_surface_get_stride(s);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const guchar *p = src + y * rs + x * n;
			unsigned a = alpha ? p[3] : 255;
			((uint32_t *)(dst + y * ds))[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
		}
	cairo_surface_mark_dirty(s);
	return s;
}

static bool exists(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool find_icon(const char *name, char *out, size_t len) {
	if (name[0] == '/')
		return snprintf(out, len, "%s", name), exists(out);
	static const char *sizes[] = {"32x32", "48x48", "scalable", "64x64", "128x128", "256x256", "24x24", "22x22", "16x16"};
	static const char *themes[] = {"hicolor", "Adwaita", "breeze", "Papirus", "Humanity"};
	static const char *exts[] = {"png", "svg"};
	char bases[2][256];
	snprintf(bases[0], sizeof(bases[0]), "%s/.local/share/icons", getenv("HOME"));
	snprintf(bases[1], sizeof(bases[1]), "/usr/share/icons");
	for (size_t b = 0; b < 2; b++)
		for (size_t t = 0; t < sizeof(themes) / sizeof(*themes); t++)
			for (size_t s = 0; s < sizeof(sizes) / sizeof(*sizes); s++)
				for (size_t e = 0; e < 2; e++) {
					snprintf(out, len, "%s/%s/%s/apps/%s.%s", bases[b], themes[t], sizes[s], name, exts[e]);
					if (exists(out))
						return true;
				}
	for (size_t e = 0; e < 2; e++) {
		snprintf(out, len, "/usr/share/pixmaps/%s.%s", name, exts[e]);
		if (exists(out))
			return true;
	}
	snprintf(out, len, "/usr/share/pixmaps/%s.xpm", name);
	return exists(out);
}

cairo_surface_t *app_icon(struct theme *t, struct app *a) {
	static cairo_surface_t *fallback; /* XP's generic program icon */
	if (!fallback)
		fallback = theme_icon(t, "app").s32;
	if (a->icon || a->icon_tried)
		return a->icon ? a->icon : fallback;
	a->icon_tried = true;
	char path[2048];
	if (a->icon_name && find_icon(a->icon_name, path, sizeof(path))) {
		GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, 32, 32, NULL);
		if (pb) {
			a->icon = surface_from_pixbuf(pb);
			g_object_unref(pb);
		}
	}
	return a->icon ? a->icon : fallback;
}

bool app_matches(struct app *a, const char *query) {
	return !*query || strcasestr(a->name, query) || (a->keywords && strcasestr(a->keywords, query)) ||
		strcasestr(a->exec, query);
}
