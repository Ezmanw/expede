/* expede-explorer: Windows Explorer for Expede, drawn with the theme bitmaps
 * ripped from the user's Windows disc.
 *
 *   expede-explorer [path | computer: | trash:]
 *
 * Toolbar (Back, Forward, Up, Search, Folders, Views), editable address bar,
 * XP's task pane or a folder tree, Tiles / Icons / List / Details views,
 * right-click menus, cut/copy/paste, rename in place, Recycle Bin (the
 * freedesktop trash, shared with other file managers), Search Companion. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"

#define TB_H 38
#define AB_H 26
#define STATUS_H 22
#define PANE_W 210
#define SB_W 17
#define MAX_SEARCH 2000

enum view { V_TILES, V_ICONS, V_LIST, V_DETAILS };
enum place { P_DIR, P_COMPUTER, P_TRASH, P_SEARCH };

enum {
	ID_BACK = 1, ID_FWD, ID_UP, ID_SEARCH, ID_FOLDERS, ID_VIEWS, ID_ADDR, ID_GO, ID_CONTENT, ID_THUMB,
	ID_SEARCH_FIELD, ID_SEARCH_GO, ID_RENAME, ID_DLG_OK, ID_DLG_CANCEL,
	ID_TASK = 100,    /* + task index */
	ID_PLACE = 200,   /* + place index */
	ID_COL = 300,     /* + details column */
	ID_TREE = 400,    /* + node (label) */
	ID_TREE_EXP = 1400, /* + node (expander) */
	ID_ITEM = 3000,   /* + entry */
	ID_MENU = 20000,  /* + menu item */
};

struct entry {
	char *name, *path;
	bool dir;
	off_t size;
	time_t mtime;
	const char *icon;
	char type[48];
	bool selected;
	/* drives in My Computer */
	unsigned long long total, avail;
	struct ui_rect box; /* in content coordinates, before scrolling */
};

struct node { char *path, *label; const char *icon; int depth; bool expanded, has_children; };

enum menu_kind { M_NONE, M_ITEM, M_BACKGROUND, M_VIEWS };
enum dialog { D_NONE, D_DELETE_FOREVER, D_PROPERTIES, D_ERROR, D_EMPTY_TRASH };

static struct {
	struct ui_win *win;
	enum place place;
	char cur[PATH_MAX];
	char **back, **fwd;
	int n_back, n_fwd;
	struct entry *e;
	int n, anchor;
	enum view view;
	int sort_col;
	bool sort_desc, show_hidden;
	int scroll, content_h, cols;
	bool dragging_thumb;
	double thumb_grab;
	/* fields */
	enum { F_NONE, F_ADDR, F_RENAME, F_SEARCH } focus;
	char addr[PATH_MAX], rename[256], query[256];
	int addr_caret, rename_caret, query_caret, rename_index;
	/* left pane */
	bool tree_mode, search_mode;
	struct node *nodes;
	int n_nodes, tree_scroll;
	/* menu */
	enum menu_kind menu;
	int menu_x, menu_y;
	/* clipboard */
	char **clip;
	int n_clip;
	bool clip_cut;
	/* dialog */
	enum dialog dialog;
	char dialog_text[1024];
	/* search results root */
	char search_root[PATH_MAX];
} ex;

/* ------------------------------------------------------------------ icons */

struct icon_cache { char name[48]; cairo_surface_t *s16, *s32, *s48; };
static struct icon_cache icons[64];
static cairo_surface_t *toolbar, *toolbar_hot;

static struct icon_cache *icon(const char *name) {
	for (int i = 0; i < 64; i++) {
		if (!icons[i].name[0]) {
			snprintf(icons[i].name, sizeof(icons[i].name), "%s", name);
			char rel[96];
			struct icon_set s = theme_icon(&ui_theme, name);
			icons[i].s16 = s.s16;
			icons[i].s32 = s.s32;
			snprintf(rel, sizeof(rel), "icons/%s-48.png", name);
			icons[i].s48 = theme_png(&ui_theme, rel);
			if (!icons[i].s48)
				icons[i].s48 = s.s32;
			return &icons[i];
		}
		if (!strcmp(icons[i].name, name))
			return &icons[i];
	}
	return &icons[0];
}

/* XP's 24px toolbar strip: Back 0, Forward 1, Search 17, Folders 4, Views 23, Up 28 */
static void tb_icon(cairo_t *cr, int index, double x, double y, bool hot, bool disabled) {
	cairo_surface_t *s = hot && toolbar_hot ? toolbar_hot : toolbar;
	if (!s)
		return;
	cairo_save(cr);
	cairo_rectangle(cr, x, y, 24, 24);
	cairo_clip(cr);
	cairo_set_source_surface(cr, s, x - index * 24, y);
	if (disabled) {
		/* XP greys disabled toolbar icons; a wash of the toolbar colour does it */
		cairo_paint_with_alpha(cr, 0.35);
	} else {
		cairo_paint(cr);
	}
	cairo_restore(cr);
}

static const char *ext_of(const char *name) {
	const char *d = strrchr(name, '.');
	return d && d != name ? d + 1 : "";
}

static void classify(struct entry *e) {
	if (e->dir) {
		e->icon = "folder";
		snprintf(e->type, sizeof(e->type), "File Folder");
		return;
	}
	const char *x = ext_of(e->name);
	static const struct { const char *exts, *icon, *type; } kinds[] = {
		{"txt log md nfo", "file-text", "Text Document"},
		{"jpg jpeg", "file-image", "JPEG Image"}, {"png", "file-image", "PNG Image"}, {"gif", "file-image", "GIF Image"},
		{"bmp", "file-image", "Bitmap Image"}, {"webp svg tif tiff ico heic", "file-image", "Image"},
		{"mp3", "file-audio", "MP3 Audio"}, {"wav", "file-audio", "Wave Sound"}, {"ogg flac opus m4a aac wma", "file-audio", "Audio File"},
		{"mp4 mkv avi webm mov wmv m4v", "file-video", "Video File"},
		{"sh py pl rb exe appimage run bin", "file-program", "Application"},
		{"desktop", "file-program", "Shortcut"},
		{"conf ini cfg toml yaml yml json xml", "file-config", "Configuration Settings"},
		{"ttf otf woff fon", "file-font", "Font File"},
		{"doc docx odt rtf pdf xls xlsx ods ppt pptx odp", "file-document", "Document"},
		{"iso img", "drive-cd", "Disc Image"},
		{"zip tar gz xz bz2 7z rar zst deb", "folder", "Compressed Archive"},
	};
	for (size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
		const char *p = kinds[i].exts;
		size_t xl = strlen(x);
		while (*x && *p) {
			size_t l = strcspn(p, " ");
			if (l == xl && !strncasecmp(p, x, l)) {
				e->icon = kinds[i].icon;
				snprintf(e->type, sizeof(e->type), "%s", kinds[i].type);
				return;
			}
			p += l + (p[l] ? 1 : 0);
		}
	}
	e->icon = "file";
	if (*x) {
		char up[16];
		snprintf(up, sizeof(up), "%s", x);
		for (char *c = up; *c; c++)
			*c = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
		snprintf(e->type, sizeof(e->type), "%s File", up);
	} else {
		snprintf(e->type, sizeof(e->type), "File");
	}
}

static void human_size(unsigned long long b, char *out, size_t len) {
	/* Explorer rounds up to whole KB in Details */
	if (b < 1024ULL * 1024)
		snprintf(out, len, "%llu KB", (b + 1023) / 1024);
	else if (b < 1024ULL * 1024 * 1024)
		snprintf(out, len, "%.1f MB", b / 1048576.0);
	else
		snprintf(out, len, "%.2f GB", b / 1073741824.0);
}

/* ------------------------------------------------------------------ places and the trash */

static const char *home(void) {
	return getenv("HOME") ? getenv("HOME") : "/";
}

static void trash_dir(char *out, size_t len, const char *sub) {
	const char *xdg = getenv("XDG_DATA_HOME");
	if (xdg && *xdg)
		snprintf(out, len, "%s/Trash%s%s", xdg, sub ? "/" : "", sub ? sub : "");
	else
		snprintf(out, len, "%s/.local/share/Trash%s%s", home(), sub ? "/" : "", sub ? sub : "");
}

static void display_name(const char *path, enum place pl, char *out, size_t len) {
	if (pl == P_COMPUTER)
		snprintf(out, len, "My Computer");
	else if (pl == P_TRASH)
		snprintf(out, len, "Recycle Bin");
	else if (pl == P_SEARCH)
		snprintf(out, len, "Search Results");
	else if (!strcmp(path, home()))
		snprintf(out, len, "%s", getenv("USER") ? getenv("USER") : "Home");
	else if (!strcmp(path, "/"))
		snprintf(out, len, "Local Disk");
	else {
		const char *b = strrchr(path, '/');
		snprintf(out, len, "%s", b && b[1] ? b + 1 : path);
	}
}

static int cmp_entries(const void *a, const void *b) {
	const struct entry *x = a, *y = b;
	if (x->dir != y->dir)
		return x->dir ? -1 : 1;
	int r = 0;
	switch (ex.sort_col) {
	case 1: r = x->size < y->size ? -1 : x->size > y->size; break;
	case 2: r = strcasecmp(x->type, y->type); break;
	case 3: r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime; break;
	}
	if (!r)
		r = strcasecmp(x->name, y->name);
	return ex.sort_desc ? -r : r;
}

static void free_entries(void) {
	for (int i = 0; i < ex.n; i++) {
		free(ex.e[i].name);
		free(ex.e[i].path);
	}
	free(ex.e);
	ex.e = NULL;
	ex.n = 0;
}

static struct entry *add_entry(const char *name, const char *path) {
	ex.e = realloc(ex.e, (ex.n + 1) * sizeof(*ex.e));
	struct entry *e = &ex.e[ex.n++];
	memset(e, 0, sizeof(*e));
	e->name = strdup(name);
	e->path = strdup(path);
	struct stat st;
	if (stat(path, &st) == 0) {
		e->dir = S_ISDIR(st.st_mode);
		e->size = st.st_size;
		e->mtime = st.st_mtime;
	}
	classify(e);
	return e;
}

static void add_drive(const char *label, const char *mount, const char *icon_name) {
	struct entry *e = add_entry(label, mount);
	e->icon = icon_name;
	snprintf(e->type, sizeof(e->type), "%s", !strcmp(icon_name, "drive-removable") ? "Removable Disk" :
		!strcmp(icon_name, "drive-cd") ? "CD Drive" : !strcmp(icon_name, "drive-network") ? "Network Drive" : "Local Disk");
	struct statvfs vs;
	if (statvfs(mount, &vs) == 0) {
		e->total = (unsigned long long)vs.f_blocks * vs.f_frsize;
		e->avail = (unsigned long long)vs.f_bavail * vs.f_frsize;
	}
}

static void load_computer(void) {
	char label[300];
	snprintf(label, sizeof(label), "%s's Files", getenv("USER") ? getenv("USER") : "My");
	struct entry *h = add_entry(label, home());
	h->icon = "my-documents";
	add_drive("Local Disk (/)", "/", "drive-disk");
	FILE *m = setmntent("/proc/mounts", "r");
	struct mntent *me;
	while (m && (me = getmntent(m))) {
		const char *d = me->mnt_dir;
		bool removable = !strncmp(d, "/media/", 7) || !strncmp(d, "/run/media/", 11);
		bool other = !strncmp(d, "/mnt/", 5) || !strcmp(d, "/home") || !strcmp(d, "/data");
		bool net = !strcmp(me->mnt_type, "nfs") || !strcmp(me->mnt_type, "nfs4") || !strcmp(me->mnt_type, "cifs") ||
			!strcmp(me->mnt_type, "sshfs") || !strcmp(me->mnt_type, "fuse.sshfs");
		if (!removable && !other && !net)
			continue;
		const char *b = strrchr(d, '/');
		char lbl[300];
		bool cd = !strcmp(me->mnt_type, "iso9660") || !strcmp(me->mnt_type, "udf");
		snprintf(lbl, sizeof(lbl), "%s (%s)", b && b[1] ? b + 1 : d, d);
		add_drive(lbl, d, net ? "drive-network" : cd ? "drive-cd" : removable ? "drive-removable" : "drive-disk");
	}
	if (m)
		endmntent(m);
}

static void load_trash(void) {
	char dir[PATH_MAX], path[PATH_MAX * 2];
	trash_dir(dir, sizeof(dir), "files");
	DIR *d = opendir(dir);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		add_entry(de->d_name, path);
	}
	if (d)
		closedir(d);
}

static void load_dir(const char *path) {
	DIR *d = opendir(path);
	if (!d) {
		snprintf(ex.dialog_text, sizeof(ex.dialog_text), "%s is not accessible.\n\n%s", path, strerror(errno));
		ex.dialog = D_ERROR;
		return;
	}
	struct dirent *de;
	char full[PATH_MAX * 2];
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (de->d_name[0] == '.' && !ex.show_hidden)
			continue;
		snprintf(full, sizeof(full), "%s%s%s", path, strcmp(path, "/") ? "/" : "", de->d_name);
		add_entry(de->d_name, full);
	}
	closedir(d);
}

static const char *search_q;
static int search_visit(const char *p, const struct stat *st, int flag, struct FTW *f) {
	const char *b = p + f->base;
	if (b[0] == '.' && f->level > 0 && !ex.show_hidden)
		return flag == FTW_D ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
	if (f->level > 0 && strcasestr(b, search_q))
		add_entry(b, p);
	return ex.n >= MAX_SEARCH ? FTW_STOP : FTW_CONTINUE;
}

static void reload(void) {
	free_entries();
	ex.anchor = -1;
	switch (ex.place) {
	case P_COMPUTER: load_computer(); break;
	case P_TRASH: load_trash(); break;
	case P_SEARCH:
		search_q = ex.query;
		nftw(ex.search_root, search_visit, 32, FTW_PHYS | FTW_ACTIONRETVAL);
		break;
	default: load_dir(ex.cur); break;
	}
	if (ex.place != P_COMPUTER)
		qsort(ex.e, ex.n, sizeof(*ex.e), cmp_entries);
	char title[300];
	display_name(ex.cur, ex.place, title, sizeof(title));
	ui_set_title(ex.win, title);
	if (ex.focus != F_ADDR)
		snprintf(ex.addr, sizeof(ex.addr), "%s", ex.place == P_DIR ? ex.cur : title);
}

static void push(char ***stack, int *n, const char *s) {
	*stack = realloc(*stack, (*n + 1) * sizeof(char *));
	(*stack)[(*n)++] = strdup(s);
}

static const char *place_token(void) {
	return ex.place == P_COMPUTER ? "computer:" : ex.place == P_TRASH ? "trash:" : ex.cur;
}

static void go(const char *where, bool remember) {
	if (remember && ex.place != P_SEARCH) {
		push(&ex.back, &ex.n_back, place_token());
		for (int i = 0; i < ex.n_fwd; i++)
			free(ex.fwd[i]);
		ex.n_fwd = 0;
	}
	if (!strcmp(where, "computer:")) {
		ex.place = P_COMPUTER;
	} else if (!strcmp(where, "trash:")) {
		ex.place = P_TRASH;
	} else {
		char real[PATH_MAX];
		const char *p = where;
		if (where[0] == '~') {
			snprintf(real, sizeof(real), "%s%s", home(), where + 1);
			p = real;
		}
		char resolved[PATH_MAX];
		if (!realpath(p, resolved)) {
			snprintf(ex.dialog_text, sizeof(ex.dialog_text), "Windows cannot find '%s'. Check the spelling and try again.", where);
			ex.dialog = D_ERROR;
			return;
		}
		struct stat st;
		if (stat(resolved, &st) == 0 && !S_ISDIR(st.st_mode)) {
			char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 32];
			snprintf(cmd, sizeof(cmd), "xdg-open %s", ui_shell_quote(resolved, q, sizeof(q)));
			spawn(cmd);
			return;
		}
		snprintf(ex.cur, sizeof(ex.cur), "%s", resolved);
		ex.place = P_DIR;
	}
	ex.scroll = 0;
	ex.search_mode = false;
	reload();
}

static void go_back(void) {
	if (!ex.n_back)
		return;
	push(&ex.fwd, &ex.n_fwd, place_token());
	char *p = ex.back[--ex.n_back];
	go(p, false);
	free(p);
}

static void go_fwd(void) {
	if (!ex.n_fwd)
		return;
	push(&ex.back, &ex.n_back, place_token());
	char *p = ex.fwd[--ex.n_fwd];
	go(p, false);
	free(p);
}

static void go_up(void) {
	if (ex.place == P_COMPUTER)
		return;
	if (ex.place != P_DIR || !strcmp(ex.cur, "/")) {
		go("computer:", true);
		return;
	}
	char up[PATH_MAX];
	snprintf(up, sizeof(up), "%s", ex.cur);
	char *s = strrchr(up, '/');
	if (s == up)
		s[1] = 0;
	else if (s)
		*s = 0;
	go(up, true);
}

static void open_entry(struct entry *e) {
	if (ex.place == P_TRASH)
		return;
	if (e->dir || ex.place == P_COMPUTER) {
		go(e->path, true);
		return;
	}
	char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 32];
	snprintf(cmd, sizeof(cmd), "xdg-open %s", ui_shell_quote(e->path, q, sizeof(q)));
	spawn(cmd);
}

static int n_selected(void) {
	int c = 0;
	for (int i = 0; i < ex.n; i++)
		c += ex.e[i].selected;
	return c;
}

static int first_selected(void) {
	for (int i = 0; i < ex.n; i++)
		if (ex.e[i].selected)
			return i;
	return -1;
}

static void select_only(int i) {
	for (int j = 0; j < ex.n; j++)
		ex.e[j].selected = j == i;
	ex.anchor = i;
}

/* ------------------------------------------------------------------ file operations */

static int run_cmd(const char *cmd) {
	int r = system(cmd);
	return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static void error_box(const char *what) {
	snprintf(ex.dialog_text, sizeof(ex.dialog_text), "%s", what);
	ex.dialog = D_ERROR;
}

/* the freedesktop trash spec, so other file managers see the same Recycle Bin */
static bool move_to_trash(const char *path) {
	char files[PATH_MAX], info[PATH_MAX];
	trash_dir(files, sizeof(files), "files");
	trash_dir(info, sizeof(info), "info");
	char base[PATH_MAX];
	trash_dir(base, sizeof(base), NULL);
	mkdir(base, 0700);
	mkdir(files, 0700);
	mkdir(info, 0700);
	const char *b = strrchr(path, '/');
	b = b ? b + 1 : path;
	char name[NAME_MAX + 16], dst[PATH_MAX * 2], inf[PATH_MAX * 2];
	snprintf(name, sizeof(name), "%s", b);
	for (int k = 2;; k++) {
		snprintf(dst, sizeof(dst), "%s/%s", files, name);
		snprintf(inf, sizeof(inf), "%s/%s.trashinfo", info, name);
		if (access(dst, F_OK) != 0 && access(inf, F_OK) != 0)
			break;
		snprintf(name, sizeof(name), "%s.%d", b, k);
	}
	FILE *f = fopen(inf, "w");
	if (!f)
		return false;
	char when[32];
	time_t now = time(NULL);
	strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S", localtime(&now));
	fprintf(f, "[Trash Info]\nPath=");
	for (const unsigned char *c = (const unsigned char *)path; *c; c++)
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("/-_.~", *c))
			fputc(*c, f);
		else
			fprintf(f, "%%%02X", *c);
	fprintf(f, "\nDeletionDate=%s\n", when);
	fclose(f);
	if (rename(path, dst) == 0)
		return true;
	if (errno == EXDEV) { /* another disk: move it the slow way */
		char q1[PATH_MAX * 2], q2[PATH_MAX * 2], cmd[PATH_MAX * 4 + 16];
		snprintf(cmd, sizeof(cmd), "mv -- %s %s", ui_shell_quote(path, q1, sizeof(q1)), ui_shell_quote(dst, q2, sizeof(q2)));
		if (run_cmd(cmd) == 0)
			return true;
	}
	unlink(inf);
	return false;
}

static void restore_from_trash(struct entry *e) {
	char info[PATH_MAX], inf[PATH_MAX * 2], line[PATH_MAX * 2], orig[PATH_MAX] = "";
	trash_dir(info, sizeof(info), "info");
	snprintf(inf, sizeof(inf), "%s/%s.trashinfo", info, e->name);
	FILE *f = fopen(inf, "r");
	while (f && fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "Path=", 5)) {
			int o = 0;
			for (const char *c = line + 5; *c && *c != '\n' && o < PATH_MAX - 1; c++) {
				unsigned v;
				if (*c == '%' && sscanf(c + 1, "%2x", &v) == 1) {
					orig[o++] = (char)v;
					c += 2;
				} else {
					orig[o++] = *c;
				}
			}
			orig[o] = 0;
		}
	}
	if (f)
		fclose(f);
	if (!orig[0] || access(orig, F_OK) == 0 || rename(e->path, orig) != 0) {
		error_box("Cannot restore this item: its original location is missing or already in use.");
		return;
	}
	unlink(inf);
}

static void empty_trash(void) {
	char files[PATH_MAX], info[PATH_MAX], q1[PATH_MAX * 2], q2[PATH_MAX * 2], cmd[PATH_MAX * 4 + 64];
	trash_dir(files, sizeof(files), "files");
	trash_dir(info, sizeof(info), "info");
	snprintf(cmd, sizeof(cmd), "rm -rf -- %s/* %s/* %s/.[!.]* 2>/dev/null",
		ui_shell_quote(files, q1, sizeof(q1)), ui_shell_quote(info, q2, sizeof(q2)), q1);
	run_cmd(cmd);
}

static void delete_selected(bool forever) {
	for (int i = 0; i < ex.n; i++) {
		if (!ex.e[i].selected)
			continue;
		if (forever || ex.place == P_TRASH) {
			char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 16];
			snprintf(cmd, sizeof(cmd), "rm -rf -- %s", ui_shell_quote(ex.e[i].path, q, sizeof(q)));
			run_cmd(cmd);
			if (ex.place == P_TRASH) {
				char info[PATH_MAX], inf[PATH_MAX * 2];
				trash_dir(info, sizeof(info), "info");
				snprintf(inf, sizeof(inf), "%s/%s.trashinfo", info, ex.e[i].name);
				unlink(inf);
			}
		} else if (!move_to_trash(ex.e[i].path)) {
			char msg[PATH_MAX + 64];
			snprintf(msg, sizeof(msg), "Cannot delete %s: %s", ex.e[i].name, strerror(errno));
			error_box(msg);
			break;
		}
	}
	reload();
}

static void clear_clipboard(void) {
	for (int i = 0; i < ex.n_clip; i++)
		free(ex.clip[i]);
	free(ex.clip);
	ex.clip = NULL;
	ex.n_clip = 0;
}

static void set_clipboard(bool cut) {
	clear_clipboard();
	for (int i = 0; i < ex.n; i++)
		if (ex.e[i].selected)
			push(&ex.clip, &ex.n_clip, ex.e[i].path);
	ex.clip_cut = cut;
}

/* a free name in dir: Explorer's "Copy of x", "Copy (2) of x" for copies, "x (2)" otherwise */
static void unique_dest(const char *dir, const char *name, bool copy_of, char *out, size_t len) {
	snprintf(out, len, "%s/%s", dir, name);
	for (int k = 1; access(out, F_OK) == 0; k++) {
		if (copy_of && k == 1)
			snprintf(out, len, "%s/Copy of %s", dir, name);
		else if (copy_of)
			snprintf(out, len, "%s/Copy (%d) of %s", dir, k, name);
		else
			snprintf(out, len, "%s/%s (%d)", dir, name, k + 1);
	}
}

static void paste(void) {
	if (ex.place != P_DIR || !ex.n_clip)
		return;
	for (int i = 0; i < ex.n_clip; i++) {
		const char *src = ex.clip[i];
		const char *b = strrchr(src, '/');
		b = b ? b + 1 : src;
		char dst[PATH_MAX * 2], q1[PATH_MAX * 2], q2[PATH_MAX * 2], cmd[PATH_MAX * 4 + 32];
		char srcdir[PATH_MAX];
		snprintf(srcdir, sizeof(srcdir), "%.*s", (int)(b - src - 1), src);
		bool same_dir = !strcmp(srcdir, ex.cur);
		if (ex.clip_cut && same_dir)
			continue;
		unique_dest(ex.cur, b, !ex.clip_cut, dst, sizeof(dst));
		if (ex.clip_cut) {
			if (rename(src, dst) != 0) {
				snprintf(cmd, sizeof(cmd), "mv -- %s %s", ui_shell_quote(src, q1, sizeof(q1)), ui_shell_quote(dst, q2, sizeof(q2)));
				run_cmd(cmd);
			}
		} else {
			snprintf(cmd, sizeof(cmd), "cp -a -- %s %s", ui_shell_quote(src, q1, sizeof(q1)), ui_shell_quote(dst, q2, sizeof(q2)));
			if (run_cmd(cmd) != 0) {
				char msg[PATH_MAX + 64];
				snprintf(msg, sizeof(msg), "Cannot copy %s.", b);
				error_box(msg);
			}
		}
	}
	if (ex.clip_cut)
		clear_clipboard(); /* a cut pastes once */
	reload();
}

static void begin_rename(int i) {
	if (i < 0 || ex.place != P_DIR)
		return;
	select_only(i);
	ex.focus = F_RENAME;
	ex.rename_index = i;
	snprintf(ex.rename, sizeof(ex.rename), "%s", ex.e[i].name);
	ex.rename_caret = strlen(ex.rename);
}

static void commit_rename(void) {
	ex.focus = F_NONE;
	int i = ex.rename_index;
	if (i < 0 || i >= ex.n || !ex.rename[0] || !strcmp(ex.rename, ex.e[i].name))
		return;
	if (strchr(ex.rename, '/')) {
		error_box("A file name can't contain a slash (/).");
		return;
	}
	char dst[PATH_MAX * 2];
	snprintf(dst, sizeof(dst), "%s/%s", ex.cur, ex.rename);
	if (access(dst, F_OK) == 0) {
		char msg[600];
		snprintf(msg, sizeof(msg), "Cannot rename %s: a file with the name you specified already exists.", ex.e[i].name);
		error_box(msg);
		return;
	}
	if (rename(ex.e[i].path, dst) != 0) {
		char msg[600];
		snprintf(msg, sizeof(msg), "Cannot rename %s: %s", ex.e[i].name, strerror(errno));
		error_box(msg);
		return;
	}
	char keep[256];
	snprintf(keep, sizeof(keep), "%s", ex.rename);
	reload();
	for (int j = 0; j < ex.n; j++)
		if (!strcmp(ex.e[j].name, keep))
			select_only(j);
}

static void new_folder(void) {
	if (ex.place != P_DIR)
		return;
	char dst[PATH_MAX * 2];
	unique_dest(ex.cur, "New Folder", false, dst, sizeof(dst));
	if (mkdir(dst, 0755) != 0) {
		error_box("Unable to create the folder: you may not have permission here.");
		return;
	}
	reload();
	const char *b = strrchr(dst, '/') + 1;
	for (int j = 0; j < ex.n; j++)
		if (!strcmp(ex.e[j].name, b))
			begin_rename(j);
}

static void properties(void) {
	int i = first_selected();
	char size[32] = "", when[64] = "";
	if (i < 0) {
		char name[300];
		display_name(ex.cur, ex.place, name, sizeof(name));
		snprintf(ex.dialog_text, sizeof(ex.dialog_text), "%s\n\nType:  File Folder\nLocation:  %s\nContains:  %d items",
			name, ex.cur, ex.n);
	} else {
		struct entry *e = &ex.e[i];
		human_size(e->total ? e->total : (unsigned long long)e->size, size, sizeof(size));
		strftime(when, sizeof(when), "%A, %B %d, %Y, %I:%M:%S %p", localtime(&e->mtime));
		if (e->total) {
			char fr[32];
			human_size(e->avail, fr, sizeof(fr));
			snprintf(ex.dialog_text, sizeof(ex.dialog_text), "%s\n\nType:  %s\nFree space:  %s\nCapacity:  %s", e->name, e->type, fr, size);
		} else {
			snprintf(ex.dialog_text, sizeof(ex.dialog_text), "%s\n\nType:  %s\nLocation:  %s\nSize:  %s\nModified:  %s",
				e->name, e->type, ex.place == P_SEARCH ? e->path : ex.cur, e->dir ? "(folder)" : size, when);
		}
	}
	ex.dialog = D_PROPERTIES;
}

/* ------------------------------------------------------------------ the folder tree */

static bool dir_has_subdirs(const char *path) {
	DIR *d = opendir(path);
	if (!d)
		return false;
	struct dirent *de;
	bool any = false;
	while (!any && (de = readdir(d)))
		if (de->d_name[0] != '.' && de->d_type == DT_DIR)
			any = true;
	closedir(d);
	return any;
}

static void tree_add(const char *path, const char *label, const char *ic, int depth) {
	ex.nodes = realloc(ex.nodes, (ex.n_nodes + 1) * sizeof(*ex.nodes));
	ex.nodes[ex.n_nodes++] = (struct node){strdup(path), strdup(label), ic, depth, false, dir_has_subdirs(path)};
}

static int cmp_str(const void *a, const void *b) {
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void tree_toggle(int i) {
	struct node *nd = &ex.nodes[i];
	if (nd->expanded) {
		int j = i + 1;
		while (j < ex.n_nodes && ex.nodes[j].depth > nd->depth) {
			free(ex.nodes[j].path);
			free(ex.nodes[j].label);
			j++;
		}
		memmove(ex.nodes + i + 1, ex.nodes + j, (ex.n_nodes - j) * sizeof(*ex.nodes));
		ex.n_nodes -= j - i - 1;
		ex.nodes[i].expanded = false;
		return;
	}
	DIR *d = opendir(nd->path);
	if (!d)
		return;
	char **names = NULL;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)))
		if (de->d_name[0] != '.' && de->d_type == DT_DIR) {
			names = realloc(names, (n + 1) * sizeof(char *));
			names[n++] = strdup(de->d_name);
		}
	closedir(d);
	qsort(names, n, sizeof(char *), cmp_str);
	/* make room right after the node */
	int depth = nd->depth + 1;
	char *parent = strdup(nd->path);
	struct node *tail = malloc((ex.n_nodes - i - 1) * sizeof(*tail) + 1);
	int n_tail = ex.n_nodes - i - 1;
	memcpy(tail, ex.nodes + i + 1, n_tail * sizeof(*tail));
	ex.n_nodes = i + 1;
	ex.nodes[i].expanded = true;
	for (int k = 0; k < n; k++) {
		char p[PATH_MAX * 2];
		snprintf(p, sizeof(p), "%s%s%s", parent, strcmp(parent, "/") ? "/" : "", names[k]);
		tree_add(p, names[k], "folder", depth);
		free(names[k]);
	}
	free(names);
	ex.nodes = realloc(ex.nodes, (ex.n_nodes + n_tail) * sizeof(*ex.nodes));
	memcpy(ex.nodes + ex.n_nodes, tail, n_tail * sizeof(*tail));
	ex.n_nodes += n_tail;
	free(tail);
	free(parent);
}

static void tree_init(void) {
	char desk[PATH_MAX];
	snprintf(desk, sizeof(desk), "%s/Desktop", home());
	if (access(desk, F_OK) == 0)
		tree_add(desk, "Desktop", "desktop", 0);
	tree_add(home(), getenv("USER") ? getenv("USER") : "Home", "my-documents", 0);
	tree_add("/", "Local Disk (/)", "drive-disk", 0);
}

/* ------------------------------------------------------------------ painting */

static int content_x(void) {
	return PANE_W; /* the task pane, the folder tree or Search Companion is always there */
}

static struct ui_rect content_box(void) {
	return (struct ui_rect){0, content_x(), TB_H + AB_H, ex.win->w - content_x(), ex.win->h - TB_H - AB_H - STATUS_H};
}

static void layout(void) {
	struct ui_rect c = content_box();
	int cw = c.w - SB_W - 8, y = 8;
	ex.cols = 1;
	switch (ex.view) {
	case V_TILES: {
		int tw = 250, th = 58;
		ex.cols = cw / tw > 0 ? cw / tw : 1;
		for (int i = 0; i < ex.n; i++)
			ex.e[i].box = (struct ui_rect){0, 8 + (i % ex.cols) * tw, y + (i / ex.cols) * (th + 4), tw - 8, th};
		ex.content_h = y + ((ex.n + ex.cols - 1) / ex.cols) * (th + 4) + 8;
		break;
	}
	case V_ICONS: {
		int tw = 80, th = 76;
		ex.cols = cw / tw > 0 ? cw / tw : 1;
		for (int i = 0; i < ex.n; i++)
			ex.e[i].box = (struct ui_rect){0, 8 + (i % ex.cols) * tw, y + (i / ex.cols) * th, tw - 4, th - 4};
		ex.content_h = y + ((ex.n + ex.cols - 1) / ex.cols) * th + 8;
		break;
	}
	case V_LIST:
		for (int i = 0; i < ex.n; i++)
			ex.e[i].box = (struct ui_rect){0, 8, y + i * 18, cw - 8, 18};
		ex.content_h = y + ex.n * 18 + 8;
		break;
	case V_DETAILS:
		y = 24;
		for (int i = 0; i < ex.n; i++)
			ex.e[i].box = (struct ui_rect){0, 0, y + i * 18, cw + 8, 18};
		ex.content_h = y + ex.n * 18 + 8;
		break;
	}
	int max = ex.content_h - c.h;
	if (ex.scroll > max)
		ex.scroll = max;
	if (ex.scroll < 0)
		ex.scroll = 0;
}

static void paint_toolbar(struct ui_win *w, cairo_t *cr) {
	/* the rebar: Luna's toolbar background, a shade of the dialog colour */
	draw_image(cr, &(struct image){.s = theme_png(&ui_theme, "luna/toolbarbackground.png"), .count = 1}, 0, 0, 0, w->w, TB_H);
	struct { int id, icon; const char *label; bool disabled; } b[] = {
		{ID_BACK, 0, "Back", ex.n_back == 0}, {ID_FWD, 1, NULL, ex.n_fwd == 0}, {ID_UP, 28, NULL, ex.place == P_COMPUTER},
		{0, 0, NULL, false},
		{ID_SEARCH, 17, "Search", false}, {ID_FOLDERS, 4, "Folders", false},
		{0, 0, NULL, false},
		{ID_VIEWS, 23, NULL, false},
	};
	int x = 4;
	for (size_t i = 0; i < sizeof(b) / sizeof(*b); i++) {
		if (!b[i].id) {
			ui_fill(cr, x + 3, 6, 1, TB_H - 12, 0xc2bfb3);
			ui_fill(cr, x + 4, 6, 1, TB_H - 12, 0xffffff);
			x += 9;
			continue;
		}
		int bw = 32 + (b[i].label ? text_width("Tahoma 8", b[i].label) + 6 : 0);
		if (b[i].id == ID_VIEWS)
			bw += 10;
		bool checked = (b[i].id == ID_SEARCH && ex.search_mode) || (b[i].id == ID_FOLDERS && ex.tree_mode);
		int st = b[i].disabled ? 0 : ui_state(w, b[i].id);
		if (st || checked) {
			/* XP's flat toolbar button: a pale blue-grey box when hot, pushed in when pressed */
			uint32_t edge = st == 2 || checked ? 0x8f8b7d : 0xcecbbd, face = st == 2 || checked ? 0xe3e0d4 : 0xfbfbf9;
			ui_fill(cr, x, 3, bw, TB_H - 6, edge);
			ui_fill(cr, x + 1, 4, bw - 2, TB_H - 8, face);
		}
		tb_icon(cr, b[i].icon, x + 4, (TB_H - 24) / 2, st == 1, b[i].disabled);
		if (b[i].label)
			ui_text(cr, "Tahoma 8", b[i].label, x + 32, (TB_H - 13) / 2, 0, b[i].disabled ? 0xaca899 : 0x000000);
		if (b[i].id == ID_VIEWS) {
			cairo_set_source_rgb(cr, 0, 0, 0);
			cairo_move_to(cr, x + bw - 10, 17);
			cairo_line_to(cr, x + bw - 4, 17);
			cairo_line_to(cr, x + bw - 7, 20);
			cairo_close_path(cr);
			cairo_fill(cr);
		}
		if (!b[i].disabled)
			ui_reg(w, b[i].id, x, 3, bw, TB_H - 6);
		x += bw + 2;
	}
	ui_fill(cr, 0, TB_H - 1, w->w, 1, 0xd2cfbf);
}

static void paint_address(struct ui_win *w, cairo_t *cr) {
	int y = TB_H;
	ui_fill(cr, 0, y, w->w, AB_H, 0xefedde);
	ui_text(cr, "Tahoma 8", "Address", 8, y + 6, 0, 0x6d6b5f);
	int fx = 58, fw = w->w - fx - 60;
	ui_fill(cr, fx, y + 2, fw, AB_H - 4, 0x7f9db9);
	ui_fill(cr, fx + 1, y + 3, fw - 2, AB_H - 6, 0xffffff);
	const char *ic = ex.place == P_COMPUTER ? "computer" : ex.place == P_TRASH ? "recycle-empty" :
		ex.place == P_SEARCH ? "search" : "folder";
	ui_icon(cr, icon(ic)->s16, fx + 4, y + 5, 16);
	cairo_save(cr);
	cairo_rectangle(cr, fx + 24, y + 3, fw - 28, AB_H - 6);
	cairo_clip(cr);
	ui_text(cr, "Tahoma 8", ex.addr, fx + 24, y + 6, 0, 0x000000);
	if (ex.focus == F_ADDR) {
		char before[PATH_MAX];
		snprintf(before, sizeof(before), "%.*s", ex.addr_caret, ex.addr);
		ui_fill(cr, fx + 24 + (ex.addr_caret ? text_width("Tahoma 8", before) : 0), y + 6, 1, 13, 0x000000);
	}
	cairo_restore(cr);
	ui_reg(w, ID_ADDR, fx, y + 2, fw, AB_H - 4);
	/* Go */
	int gx = fx + fw + 4;
	tb_icon(cr, 1, gx, y + 1, ui_state(w, ID_GO) == 1, false);
	ui_text(cr, "Tahoma 8", "Go", gx + 26, y + 6, 0, 0x000000);
	ui_reg(w, ID_GO, gx, y + 1, 48, 24);
	ui_fill(cr, 0, y + AB_H - 1, w->w, 1, 0xd2cfbf);
}

/* one of XP's task pane boxes; returns the y after it */
static int task_box(struct ui_win *w, cairo_t *cr, int y, const char *title, bool special) {
	int x = 12, bw = PANE_W - 24;
	if (special) {
		cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + bw, 0);
		cairo_pattern_add_color_stop_rgb(g, 0, 0x00 / 255.0, 0x48 / 255.0, 0xd4 / 255.0);
		cairo_pattern_add_color_stop_rgb(g, 1, 0x29 / 255.0, 0x6d / 255.0, 0xe2 / 255.0);
		cairo_set_source(cr, g);
		cairo_rectangle(cr, x, y, bw, 24);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		ui_text(cr, "Tahoma Bold 8", title, x + 12, y + 6, 0, 0xffffff);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + bw, 0);
		cairo_pattern_add_color_stop_rgb(g, 0, 1, 1, 1);
		cairo_pattern_add_color_stop_rgb(g, 1, 0xc6 / 255.0, 0xd3 / 255.0, 0xf7 / 255.0);
		cairo_set_source(cr, g);
		cairo_rectangle(cr, x, y, bw, 24);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		ui_text(cr, "Tahoma Bold 8", title, x + 12, y + 6, 0, 0x215dc6);
	}
	(void)w;
	return y + 24;
}

static int task_link(struct ui_win *w, cairo_t *cr, int id, int y, const char *ic, const char *label) {
	int x = 12;
	bool hot = w->hot == id;
	ui_icon(cr, icon(ic)->s16, x + 10, y + 3, 16);
	ui_text(cr, "Tahoma 8", label, x + 32, y + 4, PANE_W - 24 - 40, hot ? 0x428eff : 0x215dc6);
	if (hot)
		ui_fill(cr, x + 32, y + 16, text_width("Tahoma 8", label) < PANE_W - 64 ? text_width("Tahoma 8", label) : PANE_W - 64, 1, 0x428eff);
	ui_reg(w, id, x + 6, y, PANE_W - 36, 22);
	return y + 22;
}

static int box_body(cairo_t *cr, int y0, int y1, uint32_t bg) {
	ui_fill(cr, 12, y0, PANE_W - 24, y1 - y0, bg);
	ui_fill(cr, 12, y0, 1, y1 - y0, 0xffffff);
	ui_fill(cr, PANE_W - 13, y0, 1, y1 - y0, 0xffffff);
	ui_fill(cr, 12, y1 - 1, PANE_W - 24, 1, 0xffffff);
	return y1 + 12;
}

enum task {
	T_NEW_FOLDER, T_RENAME, T_COPY, T_CUT, T_DELETE, T_PASTE, T_EMPTY, T_RESTORE, T_SETTINGS, T_TERMINAL,
	T_PLACE_DESKTOP, T_PLACE_HOME, T_PLACE_COMPUTER, T_PLACE_TRASH,
};

static void paint_tasks(struct ui_win *w, cairo_t *cr, int top) {
	int y = top + 12;
	int sel = n_selected();
	/* first box: what you can do here */
	const char *title = ex.place == P_COMPUTER ? "System Tasks" : ex.place == P_TRASH ? "Recycle Bin Tasks" : "File and Folder Tasks";
	int y0 = task_box(w, cr, y, title, true);
	cairo_push_group(cr);
	int yy = y0 + 6;
	if (ex.place == P_TRASH) {
		yy = task_link(w, cr, ID_TASK + T_EMPTY, yy, "recycle-empty", "Empty the Recycle Bin");
		if (sel)
			yy = task_link(w, cr, ID_TASK + T_RESTORE, yy, "recycle-full", sel > 1 ? "Restore the selected items" : "Restore this item");
	} else if (ex.place == P_COMPUTER) {
		yy = task_link(w, cr, ID_TASK + T_SETTINGS, yy, "control-panel", "Change a setting");
	} else if (sel == 0) {
		yy = task_link(w, cr, ID_TASK + T_NEW_FOLDER, yy, "folder", "Make a new folder");
		if (ex.n_clip && ex.place == P_DIR)
			yy = task_link(w, cr, ID_TASK + T_PASTE, yy, "file", "Paste here");
		yy = task_link(w, cr, ID_TASK + T_TERMINAL, yy, "cmd", "Open Command Prompt here");
	} else {
		bool folder = sel == 1 && ex.e[first_selected()].dir;
		const char *what = sel > 1 ? "the selected items" : folder ? "this folder" : "this file";
		char l[64];
		if (sel == 1 && ex.place == P_DIR) {
			snprintf(l, sizeof(l), "Rename %s", what);
			yy = task_link(w, cr, ID_TASK + T_RENAME, yy, "file", l);
		}
		snprintf(l, sizeof(l), "Move %s", what);
		yy = task_link(w, cr, ID_TASK + T_CUT, yy, "folder-open", l);
		snprintf(l, sizeof(l), "Copy %s", what);
		yy = task_link(w, cr, ID_TASK + T_COPY, yy, "folder", l);
		snprintf(l, sizeof(l), "Delete %s", what);
		yy = task_link(w, cr, ID_TASK + T_DELETE, yy, "recycle-empty", l);
	}
	cairo_pattern_t *links = cairo_pop_group(cr);
	y = box_body(cr, y0, yy + 6, 0xd6dff7);
	cairo_set_source(cr, links);
	cairo_paint(cr);
	cairo_pattern_destroy(links);

	/* Other Places */
	y0 = task_box(w, cr, y, "Other Places", false);
	cairo_push_group(cr);
	yy = y0 + 6;
	char desk[PATH_MAX];
	snprintf(desk, sizeof(desk), "%s/Desktop", home());
	if (access(desk, F_OK) == 0)
		yy = task_link(w, cr, ID_TASK + T_PLACE_DESKTOP, yy, "desktop", "Desktop");
	if (ex.place != P_DIR || strcmp(ex.cur, home()))
		yy = task_link(w, cr, ID_TASK + T_PLACE_HOME, yy, "my-documents", "My Documents");
	if (ex.place != P_COMPUTER)
		yy = task_link(w, cr, ID_TASK + T_PLACE_COMPUTER, yy, "computer", "My Computer");
	if (ex.place != P_TRASH)
		yy = task_link(w, cr, ID_TASK + T_PLACE_TRASH, yy, "recycle-empty", "Recycle Bin");
	links = cairo_pop_group(cr);
	y = box_body(cr, y0, yy + 6, 0xd6dff7);
	cairo_set_source(cr, links);
	cairo_paint(cr);
	cairo_pattern_destroy(links);

	/* Details */
	y0 = task_box(w, cr, y, "Details", false);
	int i = first_selected();
	char line1[300], line2[64] = "", line3[96] = "";
	if (sel > 1) {
		snprintf(line1, sizeof(line1), "%d items selected.", sel);
	} else if (i >= 0) {
		struct entry *e = &ex.e[i];
		snprintf(line1, sizeof(line1), "%s", e->name);
		snprintf(line2, sizeof(line2), "%s", e->type);
		if (e->total) {
			char fr[32], tot[32];
			human_size(e->avail, fr, sizeof(fr));
			human_size(e->total, tot, sizeof(tot));
			snprintf(line3, sizeof(line3), "Free: %s of %s", fr, tot);
		} else if (!e->dir) {
			char sz[32], when[48];
			human_size(e->size, sz, sizeof(sz));
			strftime(when, sizeof(when), "%d %b %Y, %H:%M", localtime(&e->mtime));
			snprintf(line3, sizeof(line3), "Size: %s\nModified: %s", sz, when);
		}
	} else {
		display_name(ex.cur, ex.place, line1, sizeof(line1));
		snprintf(line2, sizeof(line2), "%s", ex.place == P_COMPUTER ? "System Folder" : "File Folder");
	}
	int by = y0 + 8;
	box_body(cr, y0, y0 + 72, 0xd6dff7);
	ui_text(cr, "Tahoma Bold 8", line1, 22, by, PANE_W - 44, 0x000000);
	ui_text(cr, "Tahoma 8", line2, 22, by + 16, PANE_W - 44, 0x000000);
	ui_text(cr, "Tahoma 8", line3, 22, by + 32, PANE_W - 44, 0x000000);
}

static void paint_tree(struct ui_win *w, cairo_t *cr, int top, int bottom) {
	ui_fill(cr, 0, top, PANE_W, bottom - top, 0xffffff);
	ui_fill(cr, 0, top, PANE_W, 22, 0xece9d8);
	ui_text(cr, "Tahoma 8", "Folders", 8, top + 5, 0, 0x000000);
	cairo_surface_t *exp = theme_png(&ui_theme, "luna/treeexpandcollapse.png");
	cairo_save(cr);
	cairo_rectangle(cr, 0, top + 22, PANE_W, bottom - top - 22);
	cairo_clip(cr);
	for (int i = 0; i < ex.n_nodes; i++) {
		struct node *nd = &ex.nodes[i];
		int y = top + 26 + i * 18 - ex.tree_scroll, x = 6 + nd->depth * 16;
		if (y < top + 4 || y > bottom)
			continue;
		if (nd->has_children) {
			/* the +/- box: frame 0 collapsed (+), frame 1 expanded (-) */
			if (exp)
				draw_frame_true(cr, exp, 2, nd->expanded ? 1 : 0, x, y + 4);
			ui_reg(w, ID_TREE_EXP + i, x - 2, y, 14, 18);
		}
		bool cur = ex.place == P_DIR && !strcmp(nd->path, ex.cur);
		ui_icon(cr, icon(nd->icon)->s16, x + 14, y + 1, 16);
		int tw = text_width("Tahoma 8", nd->label);
		if (cur)
			ui_fill(cr, x + 32, y + 1, tw + 4, 16, 0x316ac5);
		ui_text(cr, "Tahoma 8", nd->label, x + 34, y + 2, 0, cur ? 0xffffff : 0x000000);
		ui_reg(w, ID_TREE + i, x + 14, y, tw + 24, 18);
	}
	cairo_restore(cr);
	if (exp)
		cairo_surface_destroy(exp);
	ui_fill(cr, PANE_W - 1, top, 1, bottom - top, 0xaca899);
}

static void paint_search_pane(struct ui_win *w, cairo_t *cr, int top, int bottom) {
	ui_fill(cr, 0, top, PANE_W, bottom - top, 0xfffffc);
	ui_text(cr, "Trebuchet MS, Bold 10", "Search Companion", 10, top + 10, 0, 0x0a246a);
	ui_text(cr, "Tahoma 8", "All or part of the file name:", 12, top + 44, 0, 0x000000);
	ui_field(w, cr, ID_SEARCH_FIELD, 12, top + 62, PANE_W - 24, 22, ex.query, ex.focus == F_SEARCH, ex.query_caret);
	char where[PATH_MAX + 16];
	char nm[300];
	display_name(ex.place == P_SEARCH ? ex.search_root : ex.cur, P_DIR, nm, sizeof(nm));
	snprintf(where, sizeof(where), "Look in: %s", nm);
	ui_text(cr, "Tahoma 8", where, 12, top + 94, PANE_W - 24, 0x000000);
	ui_button(w, cr, ID_SEARCH_GO, PANE_W - 88, top + 116, 75, "Search", !ex.query[0], true);
	ui_fill(cr, PANE_W - 1, top, 1, bottom - top, 0xaca899);
}

static void paint_item(struct ui_win *w, cairo_t *cr, int i, int ox, int oy) {
	struct entry *e = &ex.e[i];
	struct ui_rect b = e->box;
	int x = ox + b.x, y = oy + b.y;
	bool sel = e->selected, renaming = ex.focus == F_RENAME && ex.rename_index == i;
	struct icon_cache *ic = icon(e->icon);
	uint32_t hl = 0x316ac5;
	switch (ex.view) {
	case V_TILES: {
		ui_icon(cr, ic->s48, x + 4, y + 5, 48);
		char l2[64] = "", l3[96] = "";
		snprintf(l2, sizeof(l2), "%s", e->type);
		if (e->total) {
			char fr[32], tot[32];
			human_size(e->avail, fr, sizeof(fr));
			human_size(e->total, tot, sizeof(tot));
			snprintf(l3, sizeof(l3), "%s free of %s", fr, tot);
		} else if (!e->dir) {
			human_size(e->size, l3, sizeof(l3));
		}
		int tx = x + 60, tw = text_width("Tahoma 8", e->name);
		if (tw > b.w - 64)
			tw = b.w - 64;
		if (sel && !renaming)
			ui_fill(cr, tx - 2, y + 9, tw + 4, 15, hl);
		if (!renaming)
			ui_text(cr, "Tahoma 8", e->name, tx, y + 10, b.w - 64, sel ? 0xffffff : 0x000000);
		ui_text(cr, "Tahoma 8", l2, tx, y + 25, b.w - 64, 0x808080);
		ui_text(cr, "Tahoma 8", l3, tx, y + 39, b.w - 64, 0x808080);
		if (renaming)
			ui_field(w, cr, ID_RENAME, tx - 3, y + 7, b.w - 60, 19, ex.rename, true, ex.rename_caret);
		break;
	}
	case V_ICONS:
		ui_icon(cr, ic->s32, x + (b.w - 32) / 2, y + 4, 32);
		if (renaming)
			ui_field(w, cr, ID_RENAME, x - 6, y + 40, b.w + 12, 19, ex.rename, true, ex.rename_caret);
		else
			draw_label(cr, "Tahoma 8", e->name, x + b.w / 2.0, y + 41, b.w - 2, sel ? 0xffffff : 0x000000, 0xffffff, sel ? (int64_t)hl : -1);
		break;
	case V_LIST:
	case V_DETAILS: {
		ui_icon(cr, ic->s16, x + 4, y + 1, 16);
		int nw = ex.view == V_DETAILS ? 260 : b.w - 30;
		int tw = text_width("Tahoma 8", e->name);
		if (tw > nw)
			tw = nw;
		if (sel && !renaming)
			ui_fill(cr, x + 23, y + 1, tw + 4, 16, hl);
		if (renaming)
			ui_field(w, cr, ID_RENAME, x + 22, y, nw, 18, ex.rename, true, ex.rename_caret);
		else
			ui_text(cr, "Tahoma 8", e->name, x + 25, y + 2, nw, sel ? 0xffffff : 0x000000);
		if (ex.view == V_DETAILS) {
			char sz[32] = "", when[48];
			if (e->total)
				human_size(e->total, sz, sizeof(sz));
			else if (!e->dir)
				human_size(e->size, sz, sizeof(sz));
			strftime(when, sizeof(when), "%d/%m/%Y %H:%M", localtime(&e->mtime));
			int sw = text_width("Tahoma 8", sz);
			ui_text(cr, "Tahoma 8", sz, x + 290 + 80 - sw - 6, y + 2, 0, 0x000000);
			ui_text(cr, "Tahoma 8", e->type, x + 378, y + 2, 140, 0x000000);
			ui_text(cr, "Tahoma 8", ex.place == P_SEARCH ? e->path : when, x + 526, y + 2,
				ex.place == P_SEARCH ? 400 : 140, 0x000000);
		}
		break;
	}
	}
	ui_reg(w, ID_ITEM + i, x, y, ex.view == V_DETAILS ? 290 : b.w, b.h);
}

static void paint_content(struct ui_win *w, cairo_t *cr) {
	struct ui_rect c = content_box();
	layout();
	ui_fill(cr, c.x, c.y, c.w, c.h, 0xffffff);
	ui_reg(w, ID_CONTENT, c.x, c.y, c.w, c.h);
	cairo_save(cr);
	cairo_rectangle(cr, c.x, c.y, c.w - SB_W, c.h);
	cairo_clip(cr);
	int ox = c.x, oy = c.y - ex.scroll;
	if (ex.view == V_DETAILS) {
		/* column headers: Luna's list view header bitmap */
		static const char *cols[] = {"Name", "Size", "Type", "Date Modified"};
		int cx[] = {0, 290, 370, 518, 666}, hy = c.y;
		struct image hdr = {.s = theme_png(&ui_theme, "luna/listviewheader.png"), .count = 3, .vertical = true,
			.m = {3, 3, 3, 3}};
		for (int k = 0; k < 4; k++) {
			int cw = cx[k + 1] - cx[k];
			draw_image(cr, &hdr, ui_state(w, ID_COL + k) == 2 ? 2 : ui_state(w, ID_COL + k), ox + cx[k], hy, cw, 22);
			const char *label = k == 3 && ex.place == P_SEARCH ? "In Folder" : cols[k];
			ui_text(cr, "Tahoma 8", label, ox + cx[k] + 6, hy + 4, cw - 12, 0x000000);
			if (ex.sort_col == k) {
				int ax = ox + cx[k + 1] - 14;
				cairo_set_source_rgb(cr, 0.55, 0.55, 0.5);
				cairo_move_to(cr, ax, ex.sort_desc ? 9 + hy : 13 + hy);
				cairo_line_to(cr, ax + 7, ex.sort_desc ? 9 + hy : 13 + hy);
				cairo_line_to(cr, ax + 3.5, ex.sort_desc ? 13 + hy : 9 + hy);
				cairo_close_path(cr);
				cairo_fill(cr);
			}
			ui_reg(w, ID_COL + k, ox + cx[k], hy, cw, 22);
		}
		if (hdr.s)
			cairo_surface_destroy(hdr.s);
		cairo_rectangle(cr, c.x, c.y + 22, c.w - SB_W, c.h - 22);
		cairo_clip(cr);
	}
	for (int i = 0; i < ex.n; i++) {
		int y = oy + ex.e[i].box.y;
		if (y + ex.e[i].box.h < c.y || y > c.y + c.h)
			continue;
		paint_item(w, cr, i, ox, oy);
	}
	if (!ex.n) {
		const char *msg = ex.place == P_SEARCH ? "Search is complete. There are no results to display." :
			ex.place == P_TRASH ? "The Recycle Bin is empty." : "This folder is empty.";
		ui_text(cr, "Tahoma 8", msg, c.x + 20, c.y + (ex.view == V_DETAILS ? 34 : 14), c.w - 40, 0x808080);
	}
	cairo_restore(cr);

	/* Luna scroll bar */
	int sx = c.x + c.w - SB_W;
	struct image shaft = {.s = theme_png(&ui_theme, "luna/scrollshaftvertical.png"), .count = 4, .vertical = true};
	draw_image(cr, &shaft, 0, sx, c.y, SB_W, c.h);
	if (shaft.s)
		cairo_surface_destroy(shaft.s);
	if (ex.content_h > c.h) {
		double frac = (double)c.h / ex.content_h;
		int th = (int)(c.h * frac);
		if (th < 20)
			th = 20;
		int ty = c.y + (int)((double)ex.scroll / (ex.content_h - c.h) * (c.h - th));
		struct image thumb = {.s = theme_png(&ui_theme, "luna/scrollthumbvertical.png"), .count = 4, .vertical = true,
			.m = {3, 3, 3, 3}};
		draw_image(cr, &thumb, ex.dragging_thumb ? 2 : ui_state(w, ID_THUMB), sx, ty, SB_W, th);
		if (thumb.s)
			cairo_surface_destroy(thumb.s);
		ui_reg(w, ID_THUMB, sx, ty, SB_W, th);
	}
}

static void paint_status(struct ui_win *w, cairo_t *cr) {
	int y = w->h - STATUS_H;
	struct image bg = {.s = theme_png(&ui_theme, "luna/statusbackground.png"), .count = 1};
	draw_image(cr, &bg, 0, 0, y, w->w, STATUS_H);
	if (bg.s)
		cairo_surface_destroy(bg.s);
	ui_fill(cr, 0, y, w->w, 1, 0xd2cfbf);
	char s[128];
	int sel = n_selected();
	if (sel) {
		unsigned long long total = 0;
		for (int i = 0; i < ex.n; i++)
			if (ex.e[i].selected && !ex.e[i].dir)
				total += ex.e[i].size;
		char sz[32];
		human_size(total, sz, sizeof(sz));
		snprintf(s, sizeof(s), "%d object%s selected", sel, sel == 1 ? "" : "s");
		bool any_file = false;
		for (int i = 0; i < ex.n; i++)
			any_file |= ex.e[i].selected && !ex.e[i].dir;
		if (any_file) /* XP shows a size only when files are selected */
			ui_text(cr, "Tahoma 8", sz, w->w - 200, y + 5, 0, 0x000000);
	} else {
		snprintf(s, sizeof(s), "%d object%s", ex.n, ex.n == 1 ? "" : "s");
	}
	ui_text(cr, "Tahoma 8", s, 8, y + 5, w->w - 220, 0x000000);
	ui_icon(cr, icon("computer")->s16, w->w - 110, y + 3, 16);
	ui_text(cr, "Tahoma 8", "My Computer", w->w - 90, y + 5, 0, 0x000000);
}

static const struct ui_menu_item item_menu[] = {
	{"Open", true, false}, {NULL, 0, 0}, {"Cut", 0, 0}, {"Copy", 0, 0}, {NULL, 0, 0},
	{"Delete", 0, 0}, {"Rename", 0, 0}, {NULL, 0, 0}, {"Properties", 0, 0},
};
static const struct ui_menu_item trash_item_menu[] = {
	{"Restore", true, false}, {NULL, 0, 0}, {"Delete", 0, 0}, {NULL, 0, 0}, {"Properties", 0, 0},
};
static const struct ui_menu_item bg_menu[] = {
	{"View", 0, 0}, {"Refresh", 0, 0}, {NULL, 0, 0}, {"Paste", 0, 0}, {NULL, 0, 0},
	{"New Folder", 0, 0}, {"Open Command Prompt Here", 0, 0}, {NULL, 0, 0}, {"Properties", 0, 0},
};
static const struct ui_menu_item views_menu[] = {
	{"Tiles", 0, 0}, {"Icons", 0, 0}, {"List", 0, 0}, {"Details", 0, 0},
};

static void current_menu(const struct ui_menu_item **items, int *n, struct ui_menu_item *buf) {
	switch (ex.menu) {
	case M_ITEM:
		if (ex.place == P_TRASH) {
			*items = trash_item_menu;
			*n = 5;
		} else {
			memcpy(buf, item_menu, sizeof(item_menu));
			*n = 9;
			buf[6].disabled = ex.place != P_DIR || n_selected() != 1;
			buf[2].disabled = buf[5].disabled = ex.place == P_COMPUTER;
			*items = buf;
		}
		break;
	case M_BACKGROUND:
		memcpy(buf, bg_menu, sizeof(bg_menu));
		*n = 9;
		buf[3].disabled = !ex.n_clip || ex.place != P_DIR;
		buf[5].disabled = buf[6].disabled = ex.place != P_DIR;
		*items = buf;
		break;
	case M_VIEWS:
		memcpy(buf, views_menu, sizeof(views_menu));
		*n = 4;
		buf[ex.view].bold = true;
		*items = buf;
		break;
	default:
		*n = 0;
	}
}

static void paint_dialog(struct ui_win *w, cairo_t *cr) {
	cairo_set_source_rgba(cr, 0, 0, 0, 0.12);
	cairo_paint(cr);
	int dw = 360, dh = ex.dialog == D_PROPERTIES ? 220 : 140;
	int x = (w->w - dw) / 2, y = (w->h - dh) / 2;
	ui_reg(w, ID_DLG_CANCEL - 100, 0, 0, w->w, w->h); /* modal: swallow clicks elsewhere */
	/* a Luna dialog: blue caption strip, face colour body */
	draw_image(cr, &ui_theme.caption, 0, x - 4, y - 26, dw + 8, 30);
	const char *title = ex.dialog == D_PROPERTIES ? "Properties" : ex.dialog == D_ERROR ? "Error" : "Confirm Delete";
	draw_text(cr, "Trebuchet MS, Bold 10", title, x + 4, y - 20, 0, 0xffffff, 1, 1, 0x0a1883, false);
	ui_fill(cr, x - 4, y + 4, dw + 8, dh, 0x0831d9);
	ui_fill(cr, x, y + 4, dw, dh - 4, 0xece9d8);
	const char *ic = ex.dialog == D_PROPERTIES ? "properties" : ex.dialog == D_ERROR ? "help" : "recycle-full";
	ui_icon(cr, icon(ic)->s32, x + 16, y + 20, 32);
	ui_text(cr, "Tahoma 8", ex.dialog_text, x + 64, y + 22, dw - 80, 0x000000);
	if (ex.dialog == D_DELETE_FOREVER || ex.dialog == D_EMPTY_TRASH) {
		ui_button(w, cr, ID_DLG_OK, x + dw - 170, y + dh - 34, 75, "Yes", false, true);
		ui_button(w, cr, ID_DLG_CANCEL, x + dw - 88, y + dh - 34, 75, "No", false, false);
	} else {
		ui_button(w, cr, ID_DLG_OK, x + dw - 88, y + dh - 34, 75, "OK", false, true);
	}
}

static void paint(struct ui_win *w, cairo_t *cr) {
	paint_toolbar(w, cr);
	paint_address(w, cr);
	int top = TB_H + AB_H, bottom = w->h - STATUS_H;
	if (ex.search_mode) {
		paint_search_pane(w, cr, top, bottom);
	} else if (ex.tree_mode) {
		paint_tree(w, cr, top, bottom);
	} else {
		/* XP's task pane: a blue gradient behind the boxes */
		cairo_pattern_t *g = cairo_pattern_create_linear(0, top, 0, bottom);
		cairo_pattern_add_color_stop_rgb(g, 0, 0x7b / 255.0, 0xa2 / 255.0, 0xe7 / 255.0);
		cairo_pattern_add_color_stop_rgb(g, 1, 0x63 / 255.0, 0x75 / 255.0, 0xd6 / 255.0);
		cairo_set_source(cr, g);
		cairo_rectangle(cr, 0, top, PANE_W, bottom - top);
		cairo_fill(cr);
		cairo_pattern_destroy(g);
		cairo_save(cr);
		cairo_rectangle(cr, 0, top, PANE_W, bottom - top);
		cairo_clip(cr);
		paint_tasks(w, cr, top);
		cairo_restore(cr);
	}
	paint_content(w, cr);
	paint_status(w, cr);
	if (ex.menu) {
		const struct ui_menu_item *items;
		struct ui_menu_item buf[16];
		int n;
		current_menu(&items, &n, buf);
		ui_menu(w, cr, ID_MENU, ex.menu_x, ex.menu_y, items, n);
	}
	if (ex.dialog)
		paint_dialog(w, cr);
}

/* ------------------------------------------------------------------ input */

static void open_terminal_here(void) {
	char q[PATH_MAX * 2], cmd[PATH_MAX * 2 + 64];
	snprintf(cmd, sizeof(cmd), "foot --working-directory=%s", ui_shell_quote(ex.place == P_DIR ? ex.cur : home(), q, sizeof(q)));
	spawn(cmd);
}

static void run_task(int t) {
	switch (t) {
	case T_NEW_FOLDER: new_folder(); break;
	case T_RENAME: begin_rename(first_selected()); break;
	case T_COPY: set_clipboard(false); break;
	case T_CUT: set_clipboard(true); break;
	case T_PASTE: paste(); break;
	case T_DELETE: delete_selected(false); break;
	case T_EMPTY:
		snprintf(ex.dialog_text, sizeof(ex.dialog_text), "Are you sure you want to delete all of the items in the Recycle Bin?");
		ex.dialog = D_EMPTY_TRASH;
		break;
	case T_RESTORE:
		for (int i = 0; i < ex.n; i++)
			if (ex.e[i].selected)
				restore_from_trash(&ex.e[i]);
		reload();
		break;
	case T_SETTINGS: spawn("expede-settings"); break;
	case T_TERMINAL: open_terminal_here(); break;
	case T_PLACE_DESKTOP: {
		char desk[PATH_MAX];
		snprintf(desk, sizeof(desk), "%s/Desktop", home());
		go(desk, true);
		break;
	}
	case T_PLACE_HOME: go(home(), true); break;
	case T_PLACE_COMPUTER: go("computer:", true); break;
	case T_PLACE_TRASH: go("trash:", true); break;
	}
}

static void menu_pick(int k) {
	enum menu_kind m = ex.menu;
	ex.menu = M_NONE;
	if (m == M_VIEWS) {
		ex.view = k;
		ex.scroll = 0;
		return;
	}
	if (m == M_ITEM && ex.place == P_TRASH) {
		if (k == 0) run_task(T_RESTORE);
		else if (k == 2) delete_selected(true);
		else if (k == 4) properties();
		return;
	}
	if (m == M_ITEM) {
		int i = first_selected();
		switch (k) {
		case 0: if (i >= 0) open_entry(&ex.e[i]); break;
		case 2: set_clipboard(true); break;
		case 3: set_clipboard(false); break;
		case 5: delete_selected(false); break;
		case 6: begin_rename(i); break;
		case 8: properties(); break;
		}
		return;
	}
	switch (k) { /* background */
	case 0: ex.menu = M_VIEWS; break;
	case 1: reload(); break;
	case 3: paste(); break;
	case 5: new_folder(); break;
	case 6: open_terminal_here(); break;
	case 8: properties(); break;
	}
}

static void click_item(int i, int mods, int clicks) {
	if (clicks == 2) {
		open_entry(&ex.e[i]);
		return;
	}
	if (mods & UI_CTRL) {
		ex.e[i].selected = !ex.e[i].selected;
		ex.anchor = i;
	} else if ((mods & UI_SHIFT) && ex.anchor >= 0) {
		int a = ex.anchor < i ? ex.anchor : i, b = ex.anchor < i ? i : ex.anchor;
		for (int j = 0; j < ex.n; j++)
			ex.e[j].selected = j >= a && j <= b;
	} else {
		select_only(i);
	}
}

static void on_button(struct ui_win *w, int id, int button, bool pressed, double x, double y, int clicks, int mods) {
	if (ex.dialog) {
		if (!pressed && id == ID_DLG_OK) {
			enum dialog d = ex.dialog;
			ex.dialog = D_NONE;
			if (d == D_DELETE_FOREVER)
				delete_selected(true);
			else if (d == D_EMPTY_TRASH) {
				empty_trash();
				reload();
			}
		} else if (!pressed && id == ID_DLG_CANCEL) {
			ex.dialog = D_NONE;
		}
		return;
	}
	if (ex.menu) {
		if (!pressed)
			return;
		const struct ui_menu_item *items;
		struct ui_menu_item buf[16];
		int n;
		current_menu(&items, &n, buf);
		if (id >= ID_MENU && id < ID_MENU + n)
			menu_pick(id - ID_MENU);
		else if (id != ID_MENU - 1)
			ex.menu = M_NONE;
		return;
	}
	if (ex.focus == F_RENAME && pressed && id != ID_RENAME)
		commit_rename();
	if (ex.focus == F_ADDR && pressed && id != ID_ADDR)
		ex.focus = F_NONE;
	if (ex.focus == F_SEARCH && pressed && id != ID_SEARCH_FIELD && id != ID_SEARCH_GO)
		ex.focus = F_NONE;

	if (button == UI_RIGHT && pressed) {
		if (id >= ID_ITEM && id < ID_ITEM + ex.n) {
			if (!ex.e[id - ID_ITEM].selected)
				select_only(id - ID_ITEM);
			ex.menu = M_ITEM;
		} else if (id == ID_CONTENT) {
			for (int j = 0; j < ex.n; j++)
				ex.e[j].selected = false;
			ex.menu = M_BACKGROUND;
		}
		ex.menu_x = x;
		ex.menu_y = y;
		return;
	}
	if (button != UI_LEFT)
		return;
	if (pressed) {
		if (id >= ID_ITEM && id < ID_ITEM + ex.n)
			click_item(id - ID_ITEM, mods, clicks);
		else if (id == ID_CONTENT) {
			for (int j = 0; j < ex.n; j++)
				ex.e[j].selected = false;
		} else if (id == ID_THUMB) {
			ex.dragging_thumb = true;
			ex.thumb_grab = y;
		} else if (id == ID_ADDR) {
			ex.focus = F_ADDR;
			ex.addr_caret = strlen(ex.addr);
		} else if (id == ID_SEARCH_FIELD) {
			ex.focus = F_SEARCH;
			ex.query_caret = strlen(ex.query);
		}
		return;
	}
	ex.dragging_thumb = false;
	if (id != w->pressed)
		return;
	if (id >= ID_TASK && id < ID_TASK + 100)
		run_task(id - ID_TASK);
	else if (id >= ID_TREE_EXP && id < ID_TREE_EXP + ex.n_nodes)
		tree_toggle(id - ID_TREE_EXP);
	else if (id >= ID_TREE && id < ID_TREE + ex.n_nodes) {
		char *p = strdup(ex.nodes[id - ID_TREE].path);
		go(p, true);
		free(p);
	} else if (id >= ID_COL && id < ID_COL + 4) {
		if (ex.sort_col == id - ID_COL)
			ex.sort_desc = !ex.sort_desc;
		else
			ex.sort_col = id - ID_COL, ex.sort_desc = false;
		qsort(ex.e, ex.n, sizeof(*ex.e), cmp_entries);
	} else switch (id) {
	case ID_BACK: go_back(); break;
	case ID_FWD: go_fwd(); break;
	case ID_UP: go_up(); break;
	case ID_GO: {
		char a[PATH_MAX];
		snprintf(a, sizeof(a), "%s", ex.addr);
		ex.focus = F_NONE;
		go(!strcmp(a, "My Computer") ? "computer:" : !strcmp(a, "Recycle Bin") ? "trash:" : a, true);
		break;
	}
	case ID_SEARCH:
		ex.search_mode = !ex.search_mode;
		ex.tree_mode = false;
		if (ex.search_mode) {
			ex.focus = F_SEARCH;
			ex.query_caret = strlen(ex.query);
		}
		break;
	case ID_FOLDERS:
		ex.tree_mode = !ex.tree_mode;
		ex.search_mode = false;
		break;
	case ID_VIEWS:
		ex.menu = M_VIEWS;
		ex.menu_x = x - 20;
		ex.menu_y = TB_H - 2;
		break;
	case ID_SEARCH_GO:
		if (ex.query[0]) {
			if (ex.place != P_SEARCH) {
				push(&ex.back, &ex.n_back, place_token());
				snprintf(ex.search_root, sizeof(ex.search_root), "%s", ex.place == P_DIR ? ex.cur : home());
			}
			ex.place = P_SEARCH;
			ex.view = V_DETAILS;
			ex.focus = F_NONE;
			reload();
			ex.search_mode = true;
		}
		break;
	}
}

static void on_motion(struct ui_win *w, double x, double y, bool held) {
	if (!ex.dragging_thumb || !held)
		return;
	struct ui_rect c = content_box();
	double frac = (double)c.h / ex.content_h;
	int th = (int)(c.h * frac);
	if (th < 20)
		th = 20;
	ex.scroll += (int)((y - ex.thumb_grab) * (ex.content_h - c.h) / (double)(c.h - th));
	ex.thumb_grab = y;
	ui_redraw(w);
}

static void on_scroll(struct ui_win *w, double dy, double x, double y) {
	if (ex.tree_mode && x < PANE_W) {
		ex.tree_scroll += dy > 0 ? 54 : -54;
		if (ex.tree_scroll < 0)
			ex.tree_scroll = 0;
	} else {
		ex.scroll += dy > 0 ? 54 : -54;
	}
	ui_redraw(w);
}

static void move_selection(int delta) {
	if (!ex.n)
		return;
	int i = first_selected();
	i = i < 0 ? 0 : i + delta;
	if (i < 0) i = 0;
	if (i >= ex.n) i = ex.n - 1;
	select_only(i);
	/* keep it in view */
	struct ui_rect c = content_box();
	int top = ex.e[i].box.y - (ex.view == V_DETAILS ? 24 : 0), bot = ex.e[i].box.y + ex.e[i].box.h;
	if (top < ex.scroll)
		ex.scroll = top;
	else if (bot > ex.scroll + c.h)
		ex.scroll = bot - c.h;
}

static void on_key(struct ui_win *w, xkb_keysym_t sym, const char *utf8, int mods) {
	if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Control_L || sym == XKB_KEY_Control_R)
		return;
	if (ex.dialog) {
		if (sym == XKB_KEY_Return)
			on_button(w, ID_DLG_OK, UI_LEFT, false, 0, 0, 1, 0);
		else if (sym == XKB_KEY_Escape)
			ex.dialog = D_NONE;
		return;
	}
	if (ex.menu) {
		if (sym == XKB_KEY_Escape)
			ex.menu = M_NONE;
		return;
	}
	if (ex.focus == F_RENAME) {
		if (sym == XKB_KEY_Return) commit_rename();
		else if (sym == XKB_KEY_Escape) ex.focus = F_NONE;
		else ui_edit(ex.rename, sizeof(ex.rename), &ex.rename_caret, sym, utf8, mods);
		return;
	}
	if (ex.focus == F_ADDR) {
		if (sym == XKB_KEY_Return) {
			w->pressed = ID_GO;
			on_button(w, ID_GO, UI_LEFT, false, 0, 0, 1, 0);
		} else if (sym == XKB_KEY_Escape) {
			ex.focus = F_NONE;
			reload();
		} else {
			ui_edit(ex.addr, sizeof(ex.addr), &ex.addr_caret, sym, utf8, mods);
		}
		return;
	}
	if (ex.focus == F_SEARCH) {
		if (sym == XKB_KEY_Return) {
			w->pressed = ID_SEARCH_GO;
			on_button(w, ID_SEARCH_GO, UI_LEFT, false, 0, 0, 1, 0);
		} else if (sym == XKB_KEY_Escape) {
			ex.focus = F_NONE;
		} else {
			ui_edit(ex.query, sizeof(ex.query), &ex.query_caret, sym, utf8, mods);
		}
		return;
	}
	if (mods & UI_ALT) {
		if (sym == XKB_KEY_Left) go_back();
		else if (sym == XKB_KEY_Right) go_fwd();
		else if (sym == XKB_KEY_Up) go_up();
		else if (sym == XKB_KEY_Return) properties();
		return;
	}
	if (mods & UI_CTRL) {
		switch (sym) {
		case XKB_KEY_a: case XKB_KEY_A:
			for (int i = 0; i < ex.n; i++)
				ex.e[i].selected = true;
			break;
		case XKB_KEY_c: case XKB_KEY_C: set_clipboard(false); break;
		case XKB_KEY_x: case XKB_KEY_X: set_clipboard(true); break;
		case XKB_KEY_v: case XKB_KEY_V: paste(); break;
		case XKB_KEY_h: case XKB_KEY_H: ex.show_hidden = !ex.show_hidden; reload(); break;
		case XKB_KEY_l: case XKB_KEY_L:
			ex.focus = F_ADDR;
			if (ex.place != P_DIR)
				snprintf(ex.addr, sizeof(ex.addr), "%s", ex.cur);
			ex.addr_caret = strlen(ex.addr);
			break;
		case XKB_KEY_f: case XKB_KEY_F: case XKB_KEY_e: case XKB_KEY_E:
			ex.search_mode = true;
			ex.tree_mode = false;
			ex.focus = F_SEARCH;
			ex.query_caret = strlen(ex.query);
			break;
		case XKB_KEY_n: case XKB_KEY_N:
			if (mods & UI_SHIFT)
				new_folder();
			else
				spawn("expede-explorer");
			break;
		case XKB_KEY_w: case XKB_KEY_W: ui_destroy(w); break;
		}
		return;
	}
	switch (sym) {
	case XKB_KEY_Return: {
		int i = first_selected();
		if (i >= 0)
			open_entry(&ex.e[i]);
		break;
	}
	case XKB_KEY_BackSpace: go_up(); break;
	case XKB_KEY_F2: begin_rename(first_selected()); break;
	case XKB_KEY_F3:
		ex.search_mode = true;
		ex.focus = F_SEARCH;
		break;
	case XKB_KEY_F4:
		ex.focus = F_ADDR;
		ex.addr_caret = strlen(ex.addr);
		break;
	case XKB_KEY_F5: reload(); break;
	case XKB_KEY_Delete:
		if (!n_selected())
			break;
		if (mods & UI_SHIFT || ex.place == P_TRASH) {
			int c = n_selected();
			if (c == 1)
				snprintf(ex.dialog_text, sizeof(ex.dialog_text), "Are you sure you want to delete '%s'?", ex.e[first_selected()].name);
			else
				snprintf(ex.dialog_text, sizeof(ex.dialog_text), "Are you sure you want to delete these %d items?", c);
			ex.dialog = D_DELETE_FOREVER;
		} else if (ex.place == P_DIR || ex.place == P_SEARCH) {
			delete_selected(false);
		}
		break;
	case XKB_KEY_Escape:
		for (int i = 0; i < ex.n; i++)
			ex.e[i].selected = false;
		break;
	case XKB_KEY_Left: move_selection(ex.view == V_DETAILS || ex.view == V_LIST ? -1 : -1); break;
	case XKB_KEY_Right: move_selection(1); break;
	case XKB_KEY_Up: move_selection(ex.view == V_DETAILS || ex.view == V_LIST ? -1 : -ex.cols); break;
	case XKB_KEY_Down: move_selection(ex.view == V_DETAILS || ex.view == V_LIST ? 1 : ex.cols); break;
	case XKB_KEY_Home: move_selection(-ex.n); break;
	case XKB_KEY_End: move_selection(ex.n); break;
	default:
		/* type to select: jump to the next name starting with that letter */
		if (utf8 && utf8[0] && (unsigned char)utf8[0] > 0x20) {
			int start = first_selected();
			for (int k = 1; k <= ex.n; k++) {
				int j = (start + k + ex.n) % ex.n;
				if (!strncasecmp(ex.e[j].name, utf8, strlen(utf8))) {
					move_selection(j - (start < 0 ? 0 : start));
					break;
				}
			}
		}
	}
}

int main(int argc, char **argv) {
	if (!ui_init("expede-explorer"))
		return 1;
	toolbar = theme_png(&ui_theme, "icons/toolbar.png");
	toolbar_hot = theme_png(&ui_theme, "icons/toolbar-hot.png");
	ex.view = V_TILES;
	ex.anchor = -1;
	ex.win = ui_window("Explorer", "expede-explorer", 780, 540, true);
	ex.win->paint = paint;
	ex.win->button = on_button;
	ex.win->motion = on_motion;
	ex.win->scroll = on_scroll;
	ex.win->key = on_key;
	tree_init();
	const char *start = argc > 1 ? argv[1] : home();
	if (!strncmp(start, "file://", 7))
		start += 7;
	snprintf(ex.cur, sizeof(ex.cur), "%s", home());
	go(start, false);
	return ui_run();
}
