/* expede-settings: Expede's Control Panel, laid out like XP's.
 *
 * Category view -> a category page ("or pick a Control Panel icon") -> each
 * applet as its own property sheet with tabs and OK / Cancel / Apply. Classic
 * View lists every applet. Built on ui.c with the theme bitmaps.
 *
 * Desktop options go to ~/.config/expede/expede.conf and reach the running
 * compositor through SIGHUP. System things go straight to the system tools:
 * nmcli (network), bluetoothctl, wpctl (sound), powerprofilesctl, xdg-mime,
 * timedatectl.
 *
 *   expede-settings [display|taskbar|network|programs|defaults|sounds|system|
 *                    power|mouse|bluetooth|users|datetime] */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ui.h"
#include "volume.h"

static struct icon_set *icon(const char *name);

/* ================================================================== options */

struct config {
	bool snap, tap, natural, sounds, clock24, glass;
	int scale_pct, speed, screen_off, glass_intensity;
	char wallpaper[256], wall_style[16], picture[64], keyboard[64];
	char style[16], glass_color[32], frames[16], taskbar[16], start_menu[16], logon[16], sound_scheme[16], pointers[16], icons[16];
	struct { char name[32], mode[32]; bool disabled; } out[8];
	int n_out;
};
static struct config cfg, saved;

static void conf_path(char *out, size_t len) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		snprintf(out, len, "%s/expede/expede.conf", xdg);
	else
		snprintf(out, len, "%s/.config/expede/expede.conf", getenv("HOME"));
}

static bool truthy(const char *v) {
	return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1");
}

static int out_index(const char *name, bool create) {
	for (int i = 0; i < cfg.n_out; i++)
		if (!strcmp(cfg.out[i].name, name))
			return i;
	if (!create || cfg.n_out >= 8)
		return -1;
	snprintf(cfg.out[cfg.n_out].name, sizeof(cfg.out[0].name), "%s", name);
	return cfg.n_out++;
}

static void config_load(void) {
	memset(&cfg, 0, sizeof(cfg)); /* zeroed padding keeps the memcmp() "changed?" test honest */
	cfg.tap = cfg.natural = cfg.sounds = cfg.glass = true;
	cfg.screen_off = 10;
	cfg.glass_intensity = 45;
	snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "Bliss.jpg");
	snprintf(cfg.wall_style, sizeof(cfg.wall_style), "auto");
	snprintf(cfg.style, sizeof(cfg.style), "xp");
	snprintf(cfg.glass_color, sizeof(cfg.glass_color), "sky");
	snprintf(cfg.frames, sizeof(cfg.frames), "auto");
	snprintf(cfg.taskbar, sizeof(cfg.taskbar), "auto");
	snprintf(cfg.start_menu, sizeof(cfg.start_menu), "auto");
	snprintf(cfg.logon, sizeof(cfg.logon), "auto");
	snprintf(cfg.sound_scheme, sizeof(cfg.sound_scheme), "auto");
	snprintf(cfg.pointers, sizeof(cfg.pointers), "auto");
	snprintf(cfg.icons, sizeof(cfg.icons), "auto");
	char path[512], line[512], key[96], val[256];
	conf_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq || sscanf(line, " %95[^= ]", key) != 1)
			continue;
		char *v = eq + 1;
		while (*v == ' ' || *v == '\t')
			v++;
		size_t n = strcspn(v, "\r\n");
		while (n && (v[n - 1] == ' ' || v[n - 1] == '\t'))
			n--;
		snprintf(val, sizeof(val), "%.*s", (int)n, v);
		if (!strcmp(key, "snap")) cfg.snap = truthy(val);
		else if (!strcmp(key, "tap_to_click")) cfg.tap = truthy(val);
		else if (!strcmp(key, "natural_scroll")) cfg.natural = truthy(val);
		else if (!strcmp(key, "sounds")) cfg.sounds = truthy(val);
		else if (!strcmp(key, "clock")) cfg.clock24 = !strcmp(val, "24");
		else if (!strcmp(key, "scale")) cfg.scale_pct = !strcasecmp(val, "auto") ? 0 : atoi(val);
		else if (!strcmp(key, "pointer_speed")) cfg.speed = atoi(val);
		else if (!strcmp(key, "screen_off")) cfg.screen_off = atoi(val);
		else if (!strcmp(key, "wallpaper")) snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", !strcasecmp(val, "none") ? "" : val);
		else if (!strcmp(key, "wallpaper_style")) snprintf(cfg.wall_style, sizeof(cfg.wall_style), "%s", val);
		else if (!strcmp(key, "user_picture")) snprintf(cfg.picture, sizeof(cfg.picture), "%s", val);
		else if (!strcmp(key, "keyboard_layout")) snprintf(cfg.keyboard, sizeof(cfg.keyboard), "%s", val);
		else if (!strcmp(key, "style")) snprintf(cfg.style, sizeof(cfg.style), "%s", val);
		else if (!strcmp(key, "glass")) cfg.glass = truthy(val);
		else if (!strcmp(key, "glass_color")) snprintf(cfg.glass_color, sizeof(cfg.glass_color), "%s", val);
		else if (!strcmp(key, "glass_intensity")) cfg.glass_intensity = atoi(val) < 0 ? 0 : atoi(val) > 100 ? 100 : atoi(val);
		else if (!strcmp(key, "frames")) snprintf(cfg.frames, sizeof(cfg.frames), "%s", val);
		else if (!strcmp(key, "taskbar")) snprintf(cfg.taskbar, sizeof(cfg.taskbar), "%s", val);
		else if (!strcmp(key, "start_menu")) snprintf(cfg.start_menu, sizeof(cfg.start_menu), "%s", val);
		else if (!strcmp(key, "logon")) snprintf(cfg.logon, sizeof(cfg.logon), "%s", val);
		else if (!strcmp(key, "sound_scheme")) snprintf(cfg.sound_scheme, sizeof(cfg.sound_scheme), "%s", val);
		else if (!strcmp(key, "pointers")) snprintf(cfg.pointers, sizeof(cfg.pointers), "%s", val);
		else if (!strcmp(key, "icons")) snprintf(cfg.icons, sizeof(cfg.icons), "%s", val);
		else if (!strncmp(key, "output.", 7) && strrchr(key, '.') > key + 7) {
			const char *dot = strrchr(key, '.');
			char name[32];
			snprintf(name, sizeof(name), "%.*s", (int)(dot - key - 7), key + 7);
			int i = out_index(name, true);
			if (i >= 0 && !strcmp(dot, ".mode"))
				snprintf(cfg.out[i].mode, sizeof(cfg.out[i].mode), "%s", val);
			else if (i >= 0 && !strcmp(dot, ".enabled"))
				cfg.out[i].disabled = !truthy(val);
		}
	}
	if (f)
		fclose(f);
	saved = cfg;
}

static void config_save(void) {
	char path[512];
	conf_path(path, sizeof(path));
	char *dir = strdup(path);
	*strrchr(dir, '/') = 0;
	mkdir(dir, 0755);
	free(dir);
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "expede-settings: cannot write %s: %s\n", path, strerror(errno));
		return;
	}
	fprintf(f, "# Expede settings (written by the Control Panel)\n");
	fprintf(f, "style = %s\n", cfg.style);
	fprintf(f, "glass = %s\n", cfg.glass ? "true" : "false");
	fprintf(f, "glass_color = %s\n", cfg.glass_color);
	fprintf(f, "glass_intensity = %d\n", cfg.glass_intensity);
	if (strcmp(cfg.frames, "auto")) fprintf(f, "frames = %s\n", cfg.frames);
	if (strcmp(cfg.taskbar, "auto")) fprintf(f, "taskbar = %s\n", cfg.taskbar);
	if (strcmp(cfg.start_menu, "auto")) fprintf(f, "start_menu = %s\n", cfg.start_menu);
	if (strcmp(cfg.logon, "auto")) fprintf(f, "logon = %s\n", cfg.logon);
	if (strcmp(cfg.sound_scheme, "auto")) fprintf(f, "sound_scheme = %s\n", cfg.sound_scheme);
	if (strcmp(cfg.pointers, "auto")) fprintf(f, "pointers = %s\n", cfg.pointers);
	if (strcmp(cfg.icons, "auto")) fprintf(f, "icons = %s\n", cfg.icons);
	fprintf(f, "snap = %s\n", cfg.snap ? "true" : "false");
	fprintf(f, "tap_to_click = %s\n", cfg.tap ? "true" : "false");
	fprintf(f, "natural_scroll = %s\n", cfg.natural ? "true" : "false");
	fprintf(f, "pointer_speed = %d\n", cfg.speed);
	fprintf(f, "sounds = %s\n", cfg.sounds ? "true" : "false");
	fprintf(f, "clock = %s\n", cfg.clock24 ? "24" : "12");
	fprintf(f, "screen_off = %d\n", cfg.screen_off);
	if (cfg.scale_pct)
		fprintf(f, "scale = %d\n", cfg.scale_pct);
	else
		fprintf(f, "scale = auto\n");
	fprintf(f, "wallpaper = %s\n", cfg.wallpaper[0] ? cfg.wallpaper : "none");
	fprintf(f, "wallpaper_style = %s\n", cfg.wall_style);
	if (cfg.picture[0])
		fprintf(f, "user_picture = %s\n", cfg.picture);
	if (cfg.keyboard[0])
		fprintf(f, "keyboard_layout = %s\n", cfg.keyboard);
	for (int i = 0; i < cfg.n_out; i++) {
		if (cfg.out[i].mode[0])
			fprintf(f, "output.%s.mode = %s\n", cfg.out[i].name, cfg.out[i].mode);
		if (cfg.out[i].disabled)
			fprintf(f, "output.%s.enabled = false\n", cfg.out[i].name);
	}
	fclose(f);
	saved = cfg;
	spawn("pkill -HUP -x expede"); /* the running desktop rereads it */
}

/* ================================================================== system helpers */

/* run a command and collect its output (stderr dropped) */
static char *capture(const char *cmd) {
	char full[2048];
	snprintf(full, sizeof(full), "%s 2>/dev/null", cmd);
	FILE *p = popen(full, "r");
	if (!p)
		return strdup("");
	size_t cap = 4096, n = 0, r;
	char *buf = malloc(cap);
	while ((r = fread(buf + n, 1, cap - n - 1, p)) > 0) {
		n += r;
		if (n + 1 >= cap)
			buf = realloc(buf, cap *= 2);
	}
	buf[n] = 0;
	pclose(p);
	return buf;
}

static bool have(const char *prog) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null", prog);
	return system(cmd) == 0;
}

static int run(const char *cmd) {
	int r = system(cmd);
	return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

/* nmcli -t fields: ':' separated, with '\:' for a literal colon */
static int split_terse(char *s, char **f, int max) {
	int n = 0;
	f[n++] = s;
	for (char *p = s; *p && n < max; p++) {
		if (*p == '\\' && p[1]) {
			memmove(p, p + 1, strlen(p));
			continue;
		}
		if (*p == ':') {
			*p = 0;
			f[n++] = p + 1;
		}
	}
	return n;
}

/* ---- network (NetworkManager) ---- */

struct wifi { char ssid[64], security[32]; int signal; bool active; };
static struct wifi wifis[64];
static int n_wifi, wifi_sel = -1, wifi_pw_caret;
static bool wifi_on, have_nm;
static char net_status[1024], wifi_password[128], net_message[256];

static void net_refresh(bool rescan) {
	have_nm = have("nmcli");
	n_wifi = 0;
	net_status[0] = 0;
	if (!have_nm)
		return;
	char *r = capture("nmcli -t -f WIFI radio");
	wifi_on = strstr(r, "enabled") != NULL;
	free(r);
	char cmd[160];
	snprintf(cmd, sizeof(cmd), "nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan %s", rescan ? "yes" : "auto");
	char *out = capture(cmd), *save;
	for (char *line = strtok_r(out, "\n", &save); line && n_wifi < 64; line = strtok_r(NULL, "\n", &save)) {
		char *f[4];
		if (split_terse(line, f, 4) < 4 || !f[1][0])
			continue;
		bool dup = false;
		for (int i = 0; i < n_wifi; i++)
			dup |= !strcmp(wifis[i].ssid, f[1]);
		if (dup)
			continue;
		struct wifi *w = &wifis[n_wifi++];
		snprintf(w->ssid, sizeof(w->ssid), "%s", f[1]);
		snprintf(w->security, sizeof(w->security), "%s", f[3]);
		w->signal = atoi(f[2]);
		w->active = f[0][0] == '*';
	}
	free(out);
	out = capture("nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev");
	size_t o = 0;
	for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[4];
		if (split_terse(line, f, 4) < 4 || !strcmp(f[1], "loopback"))
			continue;
		o += snprintf(net_status + o, sizeof(net_status) - o, "%s (%s): %s%s%s\n", f[0], f[1], f[2], f[3][0] ? " to " : "", f[3]);
		if (o >= sizeof(net_status))
			break;
	}
	free(out);
}

/* ---- bluetooth (bluez) ---- */

struct bt { char mac[24], name[64]; bool paired, connected; };
static struct bt bts[32];
static int n_bt, bt_sel = -1;
static bool bt_powered, have_bt;

static void bt_refresh(void) {
	have_bt = have("bluetoothctl");
	n_bt = 0;
	if (!have_bt)
		return;
	char *show = capture("bluetoothctl show");
	bt_powered = strstr(show, "Powered: yes") != NULL;
	have_bt = strstr(show, "Controller") != NULL;
	free(show);
	char *out = capture("bluetoothctl devices"), *save;
	for (char *line = strtok_r(out, "\n", &save); line && n_bt < 32; line = strtok_r(NULL, "\n", &save)) {
		char mac[24], name[64] = "";
		if (sscanf(line, "Device %23s %63[^\n]", mac, name) < 1)
			continue;
		struct bt *b = &bts[n_bt++];
		snprintf(b->mac, sizeof(b->mac), "%s", mac);
		snprintf(b->name, sizeof(b->name), "%s", name[0] ? name : mac);
	}
	free(out);
	for (int i = 0; i < n_bt; i++) {
		char cmd[96];
		snprintf(cmd, sizeof(cmd), "bluetoothctl info %s", bts[i].mac);
		char *info = capture(cmd);
		bts[i].paired = strstr(info, "Paired: yes") != NULL;
		bts[i].connected = strstr(info, "Connected: yes") != NULL;
		free(info);
	}
}

/* ---- sound devices (wpctl) ---- */

struct snd { int id; char name[96]; bool def, input; };
static struct snd snds[32];
static int n_snd, vol, mic_vol;
static bool muted, have_vol;
static char device[160];

static void sound_refresh(void) {
	have_vol = volume_available() && volume_read(&vol, &muted);
	volume_device_name(device, sizeof(device));
	n_snd = 0;
	if (!have("wpctl"))
		return;
	char *out = capture("wpctl status"), *save;
	bool audio = false, inputs = false, in_list = false;
	for (char *line = strtok_r(out, "\n", &save); line && n_snd < 32; line = strtok_r(NULL, "\n", &save)) {
		if (strstr(line, "Audio")) audio = true;
		else if (strstr(line, "Video")) audio = false;
		if (!audio)
			continue;
		if (strstr(line, "Sinks:")) { in_list = true; inputs = false; continue; }
		if (strstr(line, "Sources:")) { in_list = true; inputs = true; continue; }
		if (strstr(line, "Filters:") || strstr(line, "Streams:") || strstr(line, "Devices:")) { in_list = false; continue; }
		if (!in_list)
			continue;
		bool def = strchr(line, '*') != NULL;
		char *p = line;
		while (*p && (*p < '0' || *p > '9'))
			p++;
		char *dot = *p ? strchr(p, '.') : NULL;
		if (!dot)
			continue;
		char *name = dot + 1;
		while (*name == ' ')
			name++;
		char *br = strstr(name, " [vol:");
		if (br)
			*br = 0;
		struct snd *s = &snds[n_snd++];
		*s = (struct snd){atoi(p), "", def, inputs};
		snprintf(s->name, sizeof(s->name), "%s", name);
	}
	free(out);
	char *m = capture("wpctl get-volume @DEFAULT_AUDIO_SOURCE@");
	char *v = strstr(m, "Volume:");
	mic_vol = v ? (int)(strtod(v + 7, NULL) * 100 + 0.5) : 0;
	free(m);
}

/* ---- power ---- */

static char battery_pct[16], battery_state[64], profile[32];
static bool have_battery, have_profiles;

static void power_refresh(void) {
	have_battery = false;
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (strncmp(e->d_name, "BAT", 3))
			continue;
		char p[300];
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/capacity", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) { if (!fgets(battery_pct, sizeof(battery_pct), f)) battery_pct[0] = 0; fclose(f); }
		snprintf(p, sizeof(p), "/sys/class/power_supply/%s/status", e->d_name);
		f = fopen(p, "r");
		if (f) { if (!fgets(battery_state, sizeof(battery_state), f)) battery_state[0] = 0; fclose(f); }
		battery_pct[strcspn(battery_pct, "\n")] = 0;
		battery_state[strcspn(battery_state, "\n")] = 0;
		have_battery = true;
	}
	if (d)
		closedir(d);
	have_profiles = have("powerprofilesctl");
	if (have_profiles) {
		char *p = capture("powerprofilesctl get");
		p[strcspn(p, "\n")] = 0;
		snprintf(profile, sizeof(profile), "%s", p);
		free(p);
	}
}

/* ---- displays, from the compositor's ~/.cache/expede/outputs ---- */

struct screen { char name[32], desc[96], cur[32], modes[64][24]; int n_modes; bool enabled; };
static struct screen screens[8];
static int n_screens, screen_sel;

static void screens_refresh(void) {
	char path[512];
	const char *xdg = getenv("XDG_CACHE_HOME");
	if (xdg && *xdg)
		snprintf(path, sizeof(path), "%s/expede/outputs", xdg);
	else
		snprintf(path, sizeof(path), "%s/.cache/expede/outputs", getenv("HOME"));
	n_screens = 0;
	FILE *f = fopen(path, "r");
	char line[8192];
	while (f && n_screens < 8 && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *fld[7];
		int n = 0;
		fld[n++] = line;
		for (char *p = line; *p && n < 7; p++)
			if (*p == '\t') { *p = 0; fld[n++] = p + 1; }
		if (n < 7)
			continue;
		struct screen *s = &screens[n_screens++];
		snprintf(s->name, sizeof(s->name), "%s", fld[0]);
		snprintf(s->desc, sizeof(s->desc), "%s", fld[1]);
		snprintf(s->cur, sizeof(s->cur), "%s@%d", fld[2], (atoi(fld[3]) + 500) / 1000);
		s->enabled = atoi(fld[5]);
		s->n_modes = 0;
		char *save;
		for (char *m = strtok_r(fld[6], ",", &save); m && s->n_modes < 64; m = strtok_r(NULL, ",", &save)) {
			int mw, mh, r;
			if (sscanf(m, "%dx%d@%d", &mw, &mh, &r) != 3)
				continue;
			char lbl[24];
			snprintf(lbl, sizeof(lbl), "%dx%d@%d", mw, mh, (r + 500) / 1000);
			bool dup = false;
			for (int i = 0; i < s->n_modes; i++)
				dup |= !strcmp(s->modes[i], lbl);
			if (!dup)
				snprintf(s->modes[s->n_modes++], 24, "%s", lbl);
		}
	}
	if (f)
		fclose(f);
	if (screen_sel >= n_screens)
		screen_sel = 0;
}

/* ---- installed programs and defaults ---- */

struct prog { char *name, *exec, *icon_name, *file, *mimes; cairo_surface_t *icon; bool icon_tried; };
static struct prog *progs;
static int n_progs, prog_sel = -1;

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t')
		s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = 0;
	return s;
}

static int by_name(const void *a, const void *b) {
	return strcasecmp(((const struct prog *)a)->name, ((const struct prog *)b)->name);
}

static void progs_scan_dir(const char *dir) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n < 9 || strcmp(e->d_name + n - 8, ".desktop"))
			continue;
		bool dup = false;
		for (int i = 0; i < n_progs && !dup; i++)
			dup = !strcmp(strrchr(progs[i].file, '/') + 1, e->d_name);
		if (dup)
			continue;
		char path[1024], line[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char *name = NULL, *exec = NULL, *icn = NULL, *mimes = NULL;
		bool entry = false, hidden = false, app = false;
		while (fgets(line, sizeof(line), f)) {
			char *l = trim(line);
			if (*l == '[') { entry = !strcmp(l, "[Desktop Entry]"); continue; }
			char *eq = strchr(l, '=');
			if (!entry || !eq)
				continue;
			*eq = 0;
			char *k = trim(l), *v = trim(eq + 1);
			if (!strcmp(k, "Name") && !name) name = strdup(v);
			else if (!strcmp(k, "Exec") && !exec) exec = strdup(v);
			else if (!strcmp(k, "Icon") && !icn) icn = strdup(v);
			else if (!strcmp(k, "MimeType") && !mimes) mimes = strdup(v);
			else if (!strcmp(k, "Type")) app = !strcmp(v, "Application");
			else if ((!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden")) && !strcmp(v, "true")) hidden = true;
		}
		fclose(f);
		if (!app || hidden || !name) {
			free(name); free(exec); free(icn); free(mimes);
			continue;
		}
		progs = realloc(progs, (n_progs + 1) * sizeof(*progs));
		progs[n_progs++] = (struct prog){name, exec, icn, strdup(path), mimes, NULL, false};
	}
	if (d)
		closedir(d);
}

static void progs_refresh(void) {
	for (int i = 0; i < n_progs; i++) {
		free(progs[i].name); free(progs[i].exec); free(progs[i].icon_name); free(progs[i].file); free(progs[i].mimes);
		if (progs[i].icon)
			cairo_surface_destroy(progs[i].icon);
	}
	n_progs = 0;
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/.local/share/applications", getenv("HOME"));
	progs_scan_dir(dir);
	progs_scan_dir("/usr/local/share/applications");
	progs_scan_dir("/usr/share/applications");
	progs_scan_dir("/var/lib/flatpak/exports/share/applications");
	qsort(progs, n_progs, sizeof(*progs), by_name);
}

static cairo_surface_t *load_small_icon(const char *path) {
	GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(path, 24, 24, NULL);
	if (!pb)
		return NULL;
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
	g_object_unref(pb);
	return s;
}

static cairo_surface_t *prog_icon(struct prog *p) {
	if (p->icon_tried)
		return p->icon;
	p->icon_tried = true;
	if (!p->icon_name)
		return NULL;
	if (p->icon_name[0] == '/')
		return p->icon = load_small_icon(p->icon_name);
	static const char *dirs[] = {"/usr/share/icons/hicolor/32x32/apps", "/usr/share/icons/hicolor/48x48/apps",
		"/usr/share/icons/hicolor/scalable/apps", "/usr/share/icons/hicolor/64x64/apps", "/usr/share/pixmaps"};
	static const char *exts[] = {"png", "svg", "xpm"};
	char path[1024];
	for (size_t d = 0; d < 5 && !p->icon; d++)
		for (size_t x = 0; x < 3 && !p->icon; x++) {
			snprintf(path, sizeof(path), "%s/%s.%s", dirs[d], p->icon_name, exts[x]);
			if (access(path, R_OK) == 0)
				p->icon = load_small_icon(path);
		}
	return p->icon;
}

/* drop the %f / %U style codes from a .desktop Exec line */
static void exec_line(const char *exec, char *out, size_t len) {
	size_t o = 0;
	for (const char *p = exec; *p && o + 1 < len; p++) {
		if (*p == '%' && p[1]) {
			p++;
			continue;
		}
		out[o++] = *p;
	}
	out[o] = 0;
}

/* "Set Program Access and Defaults": a kind of file -> the mime types it covers */
static const struct { const char *label, *mimes[4]; } kinds[] = {
	{"Web browser", {"x-scheme-handler/https", "x-scheme-handler/http", "text/html", NULL}},
	{"E-mail", {"x-scheme-handler/mailto", NULL}},
	{"Text documents", {"text/plain", NULL}},
	{"Pictures", {"image/png", "image/jpeg", "image/gif", NULL}},
	{"Music", {"audio/mpeg", "audio/x-wav", "audio/flac", NULL}},
	{"Video", {"video/mp4", "video/x-matroska", "video/webm", NULL}},
	{"Folders", {"inode/directory", NULL}},
	{"PDF documents", {"application/pdf", NULL}},
};
#define N_KINDS ((int)(sizeof(kinds) / sizeof(*kinds)))
static char kind_default[N_KINDS][128];
static int kind_sel;

static void defaults_refresh(void) {
	for (int k = 0; k < N_KINDS; k++) {
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "xdg-mime query default %s", kinds[k].mimes[0]);
		char *r = capture(cmd);
		r[strcspn(r, "\n")] = 0;
		snprintf(kind_default[k], sizeof(kind_default[k]), "%s", r);
		free(r);
	}
}

static bool prog_handles(struct prog *p, int k) {
	if (!p->mimes)
		return false;
	for (int i = 0; i < 4 && kinds[k].mimes[i]; i++)
		if (strstr(p->mimes, kinds[k].mimes[i]))
			return true;
	return false;
}

/* ---- system ---- */

static char sys_os[128], sys_cpu[200], sys_ram[64], sys_host[128], sys_kernel[128];

static void system_refresh(void) {
	char line[512];
	FILE *f = fopen("/etc/os-release", "r");
	snprintf(sys_os, sizeof(sys_os), "Linux");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "PRETTY_NAME=", 12)) {
			char *v = line + 12;
			v[strcspn(v, "\n")] = 0;
			if (*v == '"') {
				v++;
				v[strcspn(v, "\"")] = 0;
			}
			snprintf(sys_os, sizeof(sys_os), "%s", v);
		}
	if (f)
		fclose(f);
	char model[160] = "";
	f = fopen("/proc/cpuinfo", "r");
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, "model name", 10) && !model[0]) {
			char *v = strchr(line, ':');
			snprintf(model, sizeof(model), "%s", v ? trim(v + 1) : "");
		}
	if (f)
		fclose(f);
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	snprintf(sys_cpu, sizeof(sys_cpu), cpus > 1 ? "%s (%ld threads)" : "%s", model, cpus);
	long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
	snprintf(sys_ram, sizeof(sys_ram), "%.1f GB of RAM", pages * (double)psize / 1073741824.0);
	gethostname(sys_host, sizeof(sys_host));
	struct utsname u;
	if (uname(&u) == 0)
		snprintf(sys_kernel, sizeof(sys_kernel), "Linux %s (%s)", u.release, u.machine);
}

/* ---- time zones ---- */

static char **zones;
static int n_zones, zone_sel = -1;
static char cur_zone[128];

static int cmp_str(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void zones_refresh(void) {
	if (!n_zones) {
		char *out = capture("timedatectl list-timezones"), *save;
		for (char *l = strtok_r(out, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			zones = realloc(zones, (n_zones + 1) * sizeof(char *));
			zones[n_zones++] = strdup(l);
		}
		free(out);
		qsort(zones, n_zones, sizeof(char *), cmp_str);
	}
	char link[256] = "";
	ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
	if (n > 0) {
		link[n] = 0;
		char *z = strstr(link, "zoneinfo/");
		snprintf(cur_zone, sizeof(cur_zone), "%s", z ? z + 9 : link);
	}
	zone_sel = -1;
	for (int i = 0; i < n_zones; i++)
		if (!strcmp(zones[i], cur_zone))
			zone_sel = i;
}

/* ================================================================== wallpapers, pictures, layouts */

struct choice { char *value, *label; cairo_surface_t *img; };
static struct choice *wallpapers, *pictures;
static int n_wallpapers, n_pictures;

static const struct { const char *value, *label; } layouts[] = {
	{"", "System default"}, {"us", "English (US)"}, {"gb", "English (UK)"}, {"us(intl)", "English (US, international)"},
	{"ie", "Irish"}, {"de", "German"}, {"fr", "French"}, {"es", "Spanish"}, {"it", "Italian"}, {"pt", "Portuguese"},
	{"br", "Portuguese (Brazil)"}, {"nl", "Dutch"}, {"be", "Belgian"}, {"ch", "Swiss"}, {"se", "Swedish"},
	{"no", "Norwegian"}, {"dk", "Danish"}, {"fi", "Finnish"}, {"pl", "Polish"}, {"cz", "Czech"}, {"hu", "Hungarian"},
	{"ru", "Russian"}, {"ua", "Ukrainian"}, {"gr", "Greek"}, {"tr", "Turkish"}, {"jp", "Japanese"}, {"kr", "Korean"},
	{"us(dvorak)", "English (Dvorak)"}, {"us(colemak)", "English (Colemak)"},
};
#define N_LAYOUTS ((int)(sizeof(layouts) / sizeof(*layouts)))

static int by_label(const void *a, const void *b) {
	return strcasecmp(((const struct choice *)a)->label, ((const struct choice *)b)->label);
}

static void list_dir(const char *dir, struct choice **out, int *n, bool wallpaper) {
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		*out = realloc(*out, (*n + 1) * sizeof(**out));
		char *label = strdup(e->d_name);
		char *dot = strstr(label, wallpaper ? ".tile." : ".png");
		if (!dot)
			dot = strrchr(label, '.');
		if (dot)
			*dot = 0;
		(*out)[(*n)++] = (struct choice){strdup(e->d_name), label, NULL};
	}
	if (d)
		closedir(d);
	qsort(*out, *n, sizeof(**out), by_label);
}

/* ================================================================== applets and categories */

enum kind {
	K_CP, K_DISPLAY, K_PERSONALIZE, K_TASKBAR, K_NETWORK, K_PROGRAMS, K_DEFAULTS, K_SOUNDS, K_SYSTEM, K_POWER,
	K_MOUSE, K_BLUETOOTH, K_USERS, K_DATETIME, K_COUNT
};

static const struct applet {
	const char *arg, *name, *title, *icon, *tabs[4];
} applets[K_COUNT] = {
	[K_CP] = {NULL, "Control Panel", "Control Panel", "control-panel", {NULL}},
	[K_DISPLAY] = {"display", "Display", "Display Properties", "cp-display", {"Desktop", "Appearance", "Settings", NULL}},
	[K_PERSONALIZE] = {"personalize", "Personalization", "Personalization", "cp-appearance", {"Window Color", "Advanced", NULL}},
	[K_TASKBAR] = {"taskbar", "Taskbar and Start Menu", "Taskbar and Start Menu Properties", "cp-taskbar", {"Taskbar", "Start Menu", NULL}},
	[K_NETWORK] = {"network", "Network Connections", "Network Connections", "cp-network", {"Wireless Networks", "Connections", NULL}},
	[K_PROGRAMS] = {"programs", "Add or Remove Programs", "Add or Remove Programs", "cp-programs", {NULL}},
	[K_DEFAULTS] = {"defaults", "Program Access and Defaults", "Set Program Access and Defaults", "cp-system", {"Defaults", NULL}},
	[K_SOUNDS] = {"sounds", "Sounds and Audio Devices", "Sounds and Audio Devices Properties", "cp-sounds", {"Volume", "Sounds", "Audio", NULL}},
	[K_SYSTEM] = {"system", "System", "System Properties", "computer", {"General", NULL}},
	[K_POWER] = {"power", "Power Options", "Power Options Properties", "cp-power", {"Power Schemes", "Power Meter", NULL}},
	[K_MOUSE] = {"mouse", "Mouse", "Mouse Properties", "cp-hardware", {"Pointer Options", "Touchpad", NULL}},
	[K_BLUETOOTH] = {"bluetooth", "Bluetooth Devices", "Bluetooth Devices", "network", {"Devices", NULL}},
	[K_USERS] = {"users", "User Accounts", "User Accounts", "cp-users", {"Picture", NULL}},
	[K_DATETIME] = {"datetime", "Date, Time and Keyboard", "Date and Time Properties", "cp-datetime", {"Date & Time", "Time Zone", "Keyboard", NULL}},
};

/* XP's Control Panel categories and the applets under each */
static const struct category { const char *name, *icon, *blurb; enum kind items[4]; } categories[] = {
	{"Appearance and Themes", "cp-appearance", "Change the desktop background, Windows XP / 7 style, glass colour, or the taskbar and Start menu.", {K_PERSONALIZE, K_DISPLAY, K_TASKBAR}},
	{"Network and Internet\nConnections", "cp-network", "Connect to wireless networks and see your connections.", {K_NETWORK}},
	{"Add or Remove Programs", "cp-programs", "Remove programs, or choose which program opens what.", {K_PROGRAMS, K_DEFAULTS}},
	{"Sounds, Speech, and Audio\nDevices", "cp-sounds", "Change the volume, sound scheme or speakers.", {K_SOUNDS}},
	{"Performance and\nMaintenance", "cp-performance", "See information about your computer, and power settings.", {K_SYSTEM, K_POWER}},
	{"Printers and Other\nHardware", "cp-hardware", "Change mouse and touchpad settings, or add Bluetooth devices.", {K_MOUSE, K_BLUETOOTH}},
	{"User Accounts", "cp-users", "Change your account picture.", {K_USERS}},
	{"Date, Time, Language, and\nRegional Options", "cp-datetime", "Change the clock, time zone or keyboard layout.", {K_DATETIME}},
};
#define N_CATS ((int)(sizeof(categories) / sizeof(*categories)))

#define SHEET_W 420
#define SHEET_H 470

enum {
	ID_OK = 1, ID_CANCEL, ID_APPLY, ID_VIEW_TOGGLE, ID_HOME,
	ID_SNAP, ID_TAP, ID_NATURAL, ID_SPEED, ID_VOLUME, ID_MUTE, ID_MIC, ID_SCHEME_DEFAULT, ID_SCHEME_NONE,
	ID_TEST_SOUND, ID_CLOCK12, ID_CLOCK24, ID_STRETCH, ID_CENTER, ID_TILE, ID_WIFI_ON, ID_WIFI_REFRESH,
	ID_WIFI_CONNECT, ID_WIFI_DISCONNECT, ID_WIFI_PW, ID_BT_POWER, ID_BT_SCAN, ID_BT_CONNECT, ID_BT_DISCONNECT,
	ID_BT_REMOVE, ID_PROFILE_SAVER, ID_PROFILE_BALANCED, ID_PROFILE_PERF, ID_SCREEN_ENABLED, ID_REMOVE_PROG,
	ID_CHANGE_PROGS, ID_ADD_PROGS, ID_SEARCH_PKGS, ID_INSTALL_PKG, ID_PKG_TERM, ID_PKG_QUERY,
	ID_SET_ZONE, ID_XP_STYLE, ID_W7_STYLE, ID_GLASS_ON, ID_GLASS_INTENSITY,
	ID_START_XP, ID_START_SEARCH, ID_START_W7, ID_OPEN_PERSONALIZE,
	ID_TAB = 50,          /* + tab */
	ID_SCALE = 60,        /* + 0..5 */
	ID_SCREEN_OFF = 70,   /* + choice */
	ID_SCREEN = 80,       /* + screen */
	ID_CATEGORY = 100,    /* + category */
	ID_APPLET = 200,      /* + kind */
	ID_SWATCH = 300,      /* + 0..14 */
	ID_PART_OVR = 400,    /* + part * 3 + (0=auto, 1=xp, 2=7) */
	ID_LIST = 1000,       /* + row */
	ID_LIST2 = 5000,      /* + row, a second list */
};

struct sheet {
	enum kind kind;
	struct ui_win *win;
	int tab, scroll, scroll2;
	struct ui_rect list_box, list2_box;
	bool dragging;
};

static struct sheet sheets[K_COUNT];
static struct icon_set cat_icons[N_CATS];
static int cp_category = -1; /* -1: the category view */
static bool classic_view;
static cairo_surface_t *preview;
static char preview_for[256];

struct icon_cache { char name[48]; struct icon_set s; };
static struct icon_cache icache[64];

static struct icon_set *icon(const char *name) {
	for (int i = 0; i < 64; i++) {
		if (!icache[i].name[0]) {
			snprintf(icache[i].name, sizeof(icache[i].name), "%s", name);
			icache[i].s = theme_icon(&ui_theme, name);
			return &icache[i].s;
		}
		if (!strcmp(icache[i].name, name))
			return &icache[i].s;
	}
	return &icache[0].s;
}

static void redraw_all(void) {
	for (int k = 0; k < K_COUNT; k++)
		if (sheets[k].win && sheets[k].win->alive)
			ui_redraw(sheets[k].win);
}

/* ---------------------------------------------------------------- shared painting */

static void hint(cairo_t *cr, int x, int y, const char *s) {
	ui_text(cr, "Tahoma 8", s, x, y, SHEET_W - x - 24, 0x5a5a5a);
}

/* horizontal Luna trackbar, value 0..100 */
static void slider(struct ui_win *w, cairo_t *cr, int id, int x, int y, int sw, int value, bool dragging) {
	draw_image(cr, &ui_theme.slider_track, 0, x, y + 8, sw, 4);
	for (int i = 0; i <= 10; i++)
		ui_fill(cr, x + 5 + i * (sw - 11) / 10, y + 22, 1, 3, 0x000000);
	int tx = x + value * (sw - 11) / 100;
	draw_image(cr, &ui_theme.slider_thumb_h, dragging ? 2 : ui_state(w, id) == 1 ? 1 : 0, tx, y, 11, 21);
	ui_reg(w, id, x - 4, y - 2, sw + 8, 26);
}

static int slider_value(struct ui_win *w, int id, double px) {
	for (int i = 0; i < w->n_rects; i++)
		if (w->rects[i].id == id) {
			struct ui_rect r = w->rects[i];
			int v = (int)((px - (r.x + 4) - 5) * 100 / (r.w - 8 - 11) + 0.5);
			return v < 0 ? 0 : v > 100 ? 100 : v;
		}
	return 0;
}

/* a white list box; rows registered as base + index */
static void listbox(struct ui_win *w, cairo_t *cr, int base, int x, int y, int lw, int lh, int n, int sel, int *scroll,
		struct ui_rect *box, const char *(*label)(int i), cairo_surface_t *(*ic)(int i), int row_h) {
	ui_fill(cr, x, y, lw, lh, 0x7f9db9);
	ui_fill(cr, x + 1, y + 1, lw - 2, lh - 2, 0xffffff);
	*box = (struct ui_rect){0, x, y, lw, lh};
	int rows = (lh - 4) / row_h;
	if (*scroll > n - rows)
		*scroll = n - rows;
	if (*scroll < 0)
		*scroll = 0;
	cairo_save(cr);
	cairo_rectangle(cr, x + 2, y + 2, lw - 4, lh - 4);
	cairo_clip(cr);
	for (int r = 0; r < rows + 1 && *scroll + r < n; r++) {
		int i = *scroll + r, ry = y + 2 + r * row_h;
		if (i == sel)
			ui_fill(cr, x + 2, ry, lw - 4 - (n > rows ? 6 : 0), row_h, 0x316ac5);
		int tx = x + 5;
		if (ic) {
			cairo_surface_t *s = ic(i);
			if (s)
				ui_icon(cr, s, x + 4, ry + (row_h - 16) / 2, 16);
			tx += 22;
		}
		ui_text(cr, "Tahoma 8", label(i), tx, ry + (row_h - 13) / 2, lw - (tx - x) - 12, i == sel ? 0xffffff : 0x000000);
		ui_reg(w, base + i, x + 2, ry, lw - 4, row_h);
	}
	cairo_restore(cr);
	if (n > rows) {
		double frac = (double)rows / n, pos = (double)*scroll / n;
		ui_fill(cr, x + lw - 7, y + 2, 5, lh - 4, 0xeef2fb);
		ui_fill(cr, x + lw - 7, y + 2 + pos * (lh - 4), 5, frac * (lh - 4), 0xa8bce8);
	}
}

static bool is_live(enum kind k) {
	/* these act at once (network, Bluetooth...), so they have Close rather than OK/Cancel/Apply */
	return k == K_NETWORK || k == K_BLUETOOTH || k == K_SYSTEM || k == K_DEFAULTS;
}

static void tabs_and_buttons(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	const struct applet *a = &applets[s->kind];
	ui_fill(cr, 0, 0, w->w, w->h, 0xece9d8); /* XP's dialog face colour */
	if (a->tabs[0]) {
		draw_image(cr, &ui_theme.tab_pane, 0, 6, 26, w->w - 12, w->h - 72);
		int x = 8;
		for (int i = 0; i < 4 && a->tabs[i]; i++) {
			int tw = text_width("Tahoma 8", a->tabs[i]) + 20;
			bool sel = s->tab == i;
			/* tab frames: normal hot selected disabled focused; the selected one stands taller */
			draw_image(cr, &ui_theme.tab_item, sel ? 2 : ui_state(w, ID_TAB + i) == 1 ? 1 : 0, x, sel ? 6 : 8, tw, sel ? 22 : 20);
			ui_text(cr, "Tahoma 8", a->tabs[i], x + 10, sel ? 10 : 12, 0, 0x000000);
			ui_reg(w, ID_TAB + i, x, 6, tw, 22);
			x += tw;
		}
	}
	if (is_live(s->kind)) {
		ui_button(w, cr, ID_CANCEL, w->w - 87, w->h - 36, 75, "Close", false, true);
		return;
	}
	bool dirty = memcmp(&cfg, &saved, sizeof(cfg)) != 0;
	ui_button(w, cr, ID_OK, w->w - 249, w->h - 36, 75, "OK", false, true);
	ui_button(w, cr, ID_CANCEL, w->w - 168, w->h - 36, 75, "Cancel", false, false);
	ui_button(w, cr, ID_APPLY, w->w - 87, w->h - 36, 75, "Apply", !dirty, false);
}

/* ---------------------------------------------------------------- Display */

static const char *wallpaper_label(int i) { return wallpapers[i].label; }

static int wallpaper_index(void) {
	for (int i = 0; i < n_wallpapers; i++)
		if (!strcmp(wallpapers[i].value, cfg.wallpaper))
			return i;
	return 0;
}

static void paint_monitor(cairo_t *cr, int x, int y) {
	/* a little beige XP monitor showing the chosen wallpaper */
	ui_fill(cr, x, y, 190, 140, 0x8c8a82);
	ui_fill(cr, x + 1, y + 1, 188, 138, 0xe9e7df);
	ui_fill(cr, x + 14, y + 12, 162, 114, 0x404040);
	int sx = x + 16, sy = y + 14, sw = 158, sh = 110;
	ui_fill(cr, sx, sy, sw, sh, 0x004e98);
	if (strcmp(preview_for, cfg.wallpaper)) {
		if (preview)
			cairo_surface_destroy(preview);
		preview = NULL;
		snprintf(preview_for, sizeof(preview_for), "%s", cfg.wallpaper);
		if (cfg.wallpaper[0]) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/wallpapers/%s", ui_theme.dir, cfg.wallpaper);
			preview = theme_load_image(path);
		}
	}
	if (preview) {
		double iw = cairo_image_surface_get_width(preview), ih = cairo_image_surface_get_height(preview);
		bool tiled = strstr(cfg.wallpaper, ".tile.") != NULL;
		const char *st = cfg.wall_style;
		int style = !strcmp(st, "center") ? WALL_CENTER : !strcmp(st, "tile") ? WALL_TILE :
			!strcmp(st, "stretch") ? WALL_STRETCH : tiled ? WALL_TILE : WALL_STRETCH;
		double k = sw / 1280.0;
		cairo_save(cr);
		cairo_rectangle(cr, sx, sy, sw, sh);
		cairo_clip(cr);
		cairo_translate(cr, sx, sy);
		if (style == WALL_STRETCH) {
			cairo_scale(cr, sw / iw, sh / ih);
			cairo_set_source_surface(cr, preview, 0, 0);
		} else {
			cairo_scale(cr, k, k);
			cairo_set_source_surface(cr, preview, style == WALL_CENTER ? (sw / k - iw) / 2 : 0, style == WALL_CENTER ? (sh / k - ih) / 2 : 0);
			if (style == WALL_TILE)
				cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_REPEAT);
		}
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	ui_fill(cr, x + 80, y + 140, 30, 8, 0xc9c6bb);
	ui_fill(cr, x + 60, y + 148, 70, 6, 0xd9d6cc);
}

static const char *mode_label(int i) { return screens[screen_sel].modes[i]; }

static void paint_display(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		paint_monitor(cr, (SHEET_W - 190) / 2, 40);
		ui_text(cr, "Tahoma 8", "Background:", 18, 206, 0, 0x000000);
		listbox(w, cr, ID_LIST, 18, 222, 250, 170, n_wallpapers, wallpaper_index(), &s->scroll, &s->list_box, wallpaper_label, NULL, 17);
		ui_text(cr, "Tahoma 8", "Position:", 284, 222, 0, 0x000000);
		bool tiled = strstr(cfg.wallpaper, ".tile.") != NULL, autos = !strcmp(cfg.wall_style, "auto");
		ui_check(w, cr, ID_STRETCH, 284, 240, !strcmp(cfg.wall_style, "stretch") || (autos && !tiled), true, "Stretch");
		ui_check(w, cr, ID_CENTER, 284, 262, !strcmp(cfg.wall_style, "center"), true, "Center");
		ui_check(w, cr, ID_TILE, 284, 284, !strcmp(cfg.wall_style, "tile") || (autos && tiled), true, "Tile");
	} else if (s->tab == 1) {
		ui_group(cr, 16, 40, SHEET_W - 32, 108, "Size of text and items");
		static const char *labels[6] = {"Automatic", "100%", "125%", "150%", "175%", "200%"};
		static const int pcts[6] = {0, 100, 125, 150, 175, 200};
		for (int i = 0; i < 6; i++)
			ui_check(w, cr, ID_SCALE + i, 30 + (i % 3) * 120, 64 + (i / 3) * 24, cfg.scale_pct == pcts[i], true, labels[i]);
		hint(cr, 30, 116, "Automatic picks a size from your screen's sharpness,\nlike Windows does.");
		bool is7 = !strcasecmp(cfg.style, "7") || !strcasecmp(cfg.style, "win7") || !strcasecmp(cfg.style, "windows7");
		ui_group(cr, 16, 160, SHEET_W - 32, 104, "Windows and buttons");
		ui_check(w, cr, ID_XP_STYLE, 30, 184, !is7, true, "Windows XP style (Luna, Default blue)");
		ui_check(w, cr, ID_W7_STYLE, 30, 208, is7, true, "Windows 7 style (Aero)");
		ui_button(w, cr, ID_OPEN_PERSONALIZE, 30, 232, 140, "Personalization...", false, false);
	} else {
		if (!n_screens) {
			hint(cr, 24, 50, "No screen information yet: this tab needs Expede running.");
			return;
		}
		/* the screens as numbered monitors, like XP's Settings tab */
		int mx = 30, my = 44;
		for (int i = 0; i < n_screens; i++) {
			ui_fill(cr, mx, my, 70, 52, i == screen_sel ? 0x316ac5 : 0x6a6a6a);
			ui_fill(cr, mx + 3, my + 3, 64, 46, screens[i].enabled ? 0x3a6ea5 : 0x9a9a9a);
			char num[8];
			snprintf(num, sizeof(num), "%d", i + 1);
			ui_text(cr, "Trebuchet MS, Bold 16", num, mx + 28, my + 12, 0, 0xffffff);
			ui_reg(w, ID_SCREEN + i, mx, my, 70, 52);
			mx += 80;
		}
		struct screen *sc = &screens[screen_sel];
		char d[160];
		snprintf(d, sizeof(d), "Display:  %d. %s (%s)", screen_sel + 1, sc->desc, sc->name);
		ui_text(cr, "Tahoma 8", d, 18, 110, SHEET_W - 36, 0x000000);
		ui_group(cr, 16, 130, SHEET_W - 32, 230, "Screen resolution and refresh rate");
		int ci = out_index(sc->name, false);
		const char *want = ci >= 0 && cfg.out[ci].mode[0] ? cfg.out[ci].mode : sc->cur;
		int sel = -1;
		for (int i = 0; i < sc->n_modes; i++)
			if (!strcmp(sc->modes[i], want))
				sel = i;
		listbox(w, cr, ID_LIST, 28, 152, SHEET_W - 56, 160, sc->n_modes, sel, &s->scroll, &s->list_box, mode_label, NULL, 17);
		if (n_screens > 1)
			ui_check(w, cr, ID_SCREEN_ENABLED, 28, 324, !(ci >= 0 && cfg.out[ci].disabled), false, "Use this device");
		else
			hint(cr, 28, 324, "Your only screen is always on.");
	}
}

/* ---------------------------------------------------------------- Personalization */

static const struct { const char *name, *label; uint32_t rgb; } swatches[] = {
	{"sky", "Sky", 0x74b8fc}, {"twilight", "Twilight", 0x3a6fd8}, {"sea", "Sea", 0x3fb6c9},
	{"leaf", "Leaf", 0x5fc35f}, {"lime", "Lime", 0xb3d736}, {"sun", "Sun", 0xf6d33c},
	{"pumpkin", "Pumpkin", 0xf89e38}, {"ruby", "Ruby", 0xd02f3b}, {"blush", "Blush", 0xef6a8e},
	{"violet", "Violet", 0xc160d3}, {"lavender", "Lavender", 0xa69bd9}, {"taupe", "Taupe", 0xa89f8a},
	{"chocolate", "Chocolate", 0x7a5737}, {"slate", "Slate", 0x6f7b88}, {"frost", "Frost", 0xe4eaf0},
};
#define N_SWATCHES ((int)(sizeof(swatches) / sizeof(*swatches)))

static char *part_val(int p) {
	switch (p) {
	case 0: return cfg.frames;
	case 1: return cfg.taskbar;
	case 2: return cfg.start_menu;
	case 3: return cfg.logon;
	case 4: return cfg.sound_scheme;
	case 5: return cfg.pointers;
	default: return cfg.icons;
	}
}

static void paint_personalize(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		bool is7 = !strcasecmp(cfg.style, "7") || !strcasecmp(cfg.style, "win7") || !strcasecmp(cfg.style, "windows7");
		ui_group(cr, 16, 40, SHEET_W - 32, 74, "Windows style");
		ui_check(w, cr, ID_XP_STYLE, 30, 62, !is7, true, "Windows XP (Luna)");
		ui_check(w, cr, ID_W7_STYLE, 30, 86, is7, true, "Windows 7 (Aero)");

		ui_group(cr, 16, 124, SHEET_W - 32, 266, "Window color and appearance");
		ui_text(cr, "Tahoma 8", "Change the color of your window borders, Start menu, and taskbar:", 30, 146, SHEET_W - 60, 0x000000);
		for (int i = 0; i < N_SWATCHES; i++) {
			int col = i % 8, row = i / 8;
			int sx = 30 + col * 44, sy = 172 + row * 44;
			bool sel = !strcasecmp(cfg.glass_color, swatches[i].name);
			bool hot = w->hot == ID_SWATCH + i;
			if (sel || hot) {
				ui_fill(cr, sx - 3, sy - 3, 38, 38, sel ? 0x3399ff : 0x9bb4e5);
				ui_fill(cr, sx - 1, sy - 1, 34, 34, 0xffffff);
			}
			ui_fill(cr, sx, sy, 32, 32, 0x505050);
			ui_fill(cr, sx + 1, sy + 1, 30, 30, swatches[i].rgb);
			/* diagonal glass shine on the swatch */
			cairo_save(cr);
			cairo_rectangle(cr, sx + 1, sy + 1, 30, 14);
			cairo_clip(cr);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
			cairo_paint(cr);
			cairo_restore(cr);
			ui_reg(w, ID_SWATCH + i, sx - 2, sy - 2, 36, 36);
		}
		ui_check(w, cr, ID_GLASS_ON, 30, 272, cfg.glass, false, "Enable transparency");
		ui_text(cr, "Tahoma 8", "Color intensity:", 30, 308, 0, 0x000000);
		slider(w, cr, ID_GLASS_INTENSITY, 126, 302, SHEET_W - 164, cfg.glass_intensity,
			s->dragging && w->pressed == ID_GLASS_INTENSITY);
	} else {
		ui_group(cr, 16, 40, SHEET_W - 32, 340, "Per-part style overrides");
		hint(cr, 30, 62, "Mix Windows XP and Windows 7 parts, or leave on Auto\nto follow the main Windows style.");
		static const char *names[7] = {
			"Window frames", "Taskbar", "Start menu", "Welcome / logon screen",
			"Sound scheme", "Mouse pointers", "Desktop & shell icons"
		};
		for (int p = 0; p < 7; p++) {
			int y = 104 + p * 36;
			const char *v = part_val(p);
			int cur = (!strcasecmp(v, "7") || !strcasecmp(v, "win7") || !strcasecmp(v, "windows7")) ? 2 :
				(!strcasecmp(v, "xp") || !strcasecmp(v, "classic") || !strcasecmp(v, "modern") || !strcasecmp(v, "search")) ? 1 : 0;
			ui_text(cr, "Tahoma 8", names[p], 30, y + 2, 160, 0x000000);
			ui_check(w, cr, ID_PART_OVR + p * 3 + 0, 195, y, cur == 0, true, "Auto");
			ui_check(w, cr, ID_PART_OVR + p * 3 + 1, 260, y, cur == 1, true, "XP");
			ui_check(w, cr, ID_PART_OVR + p * 3 + 2, 315, y, cur == 2, true, "Windows 7");
		}
	}
}

/* ---------------------------------------------------------------- Taskbar */

static void paint_taskbar(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		ui_group(cr, 16, 40, SHEET_W - 32, 84, "Window behavior");
		ui_check(w, cr, ID_SNAP, 28, 64, cfg.snap, false, "Snap windows to the edges of the screen");
		hint(cr, 47, 86, "Drag a window to the top to maximise it, or to a side\nto fill half the screen. Not part of Windows XP.");
		ui_group(cr, 16, 136, SHEET_W - 32, 64, "Notification area");
		hint(cr, 30, 160, "Shows the volume icon and the clock. Set the clock\nformat in Date and Time.");
	} else {
		bool is_w7 = !strcasecmp(cfg.start_menu, "7") || (!strcasecmp(cfg.start_menu, "auto") && !strcasecmp(cfg.style, "7"));
		bool is_search = !strcasecmp(cfg.start_menu, "modern") || !strcasecmp(cfg.start_menu, "search");
		bool is_xp = !is_w7 && !is_search;
		ui_group(cr, 16, 40, SHEET_W - 32, 180, "Start menu style");
		ui_check(w, cr, ID_START_XP, 28, 64, is_xp, true, "Start menu (Windows XP)");
		hint(cr, 47, 84, "All Programs opens XP's cascading list of every app.");
		ui_check(w, cr, ID_START_SEARCH, 28, 110, is_search, true, "Start menu with search (Windows XP)");
		hint(cr, 47, 130, "Start typing as soon as the menu opens; All Programs scrolls in place.");
		ui_check(w, cr, ID_START_W7, 28, 156, is_w7, true, "Start menu (Windows 7)");
		hint(cr, 47, 176, "White program list, glass places column, and Search programs and files.");
	}
}

/* ---------------------------------------------------------------- Network */

static const char *wifi_label(int i) {
	static char buf[128];
	snprintf(buf, sizeof(buf), "%s%s", wifis[i].ssid, wifis[i].active ? "   (Connected)" : "");
	return buf;
}

static void signal_bars(cairo_t *cr, int x, int y, int signal, bool locked) {
	for (int b = 0; b < 4; b++) {
		int h = 4 + b * 3;
		ui_fill(cr, x + b * 5, y + 13 - h, 4, h, signal >= 20 + b * 20 ? 0x2a9a2a : 0xc8c8c8);
	}
	if (locked) { /* a small padlock */
		ui_fill(cr, x + 22, y + 6, 7, 6, 0xc8a24a);
		cairo_set_source_rgb(cr, 0.55, 0.45, 0.2);
		cairo_set_line_width(cr, 1.2);
		cairo_arc(cr, x + 25.5, y + 6, 2.4, 3.14159, 0);
		cairo_stroke(cr);
	}
}

static bool secured(struct wifi *wf) {
	return wf->security[0] && strcmp(wf->security, "--");
}

static void paint_network(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (!have_nm) {
		hint(cr, 24, 50, "NetworkManager (nmcli) isn't installed, so networks\ncan't be managed here.");
		return;
	}
	if (s->tab == 0) {
		ui_check(w, cr, ID_WIFI_ON, 18, 40, wifi_on, false, "Use Wireless");
		ui_button(w, cr, ID_WIFI_REFRESH, SHEET_W - 146, 36, 128, "Refresh network list", !wifi_on, false);
		ui_text(cr, "Tahoma 8", "Choose a wireless network:", 18, 70, 0, 0x000000);
		listbox(w, cr, ID_LIST, 18, 88, SHEET_W - 36, 180, n_wifi, wifi_sel, &s->scroll, &s->list_box, wifi_label, NULL, 22);
		int rows = (180 - 4) / 22;
		for (int r = 0; r < rows && s->scroll + r < n_wifi; r++)
			signal_bars(cr, SHEET_W - 80, 92 + r * 22, wifis[s->scroll + r].signal, secured(&wifis[s->scroll + r]));
		if (!n_wifi)
			hint(cr, 28, 100, wifi_on ? "No wireless networks found." : "Wireless is turned off.");
		if (wifi_sel >= 0 && wifi_sel < n_wifi) {
			struct wifi *wf = &wifis[wifi_sel];
			if (wf->active) {
				ui_button(w, cr, ID_WIFI_DISCONNECT, SHEET_W - 96, 280, 78, "Disconnect", false, true);
			} else {
				if (secured(wf)) {
					ui_text(cr, "Tahoma 8", "Network key:", 18, 284, 0, 0x000000);
					char dots[128];
					size_t n = strlen(wifi_password);
					if (n > 127)
						n = 127;
					memset(dots, '*', n);
					dots[n] = 0;
					ui_field(w, cr, ID_WIFI_PW, 96, 280, 210, 22, dots, true, n);
				}
				ui_button(w, cr, ID_WIFI_CONNECT, SHEET_W - 96, 280, 78, "Connect", secured(wf) && !wifi_password[0], true);
			}
			char info[96];
			snprintf(info, sizeof(info), "Signal: %d%%   Security: %s", wf->signal, secured(wf) ? wf->security : "Unsecured");
			hint(cr, 18, 312, info);
		}
		if (net_message[0])
			ui_text(cr, "Tahoma 8", net_message, 18, 334, SHEET_W - 36, 0xa00000);
	} else {
		ui_group(cr, 16, 40, SHEET_W - 32, 200, "Network connections");
		ui_text(cr, "Tahoma 8", net_status[0] ? net_status : "No network devices.", 30, 62, SHEET_W - 60, 0x000000);
	}
}

/* ---------------------------------------------------------------- Programs and Defaults */

static const char *prog_label(int i) { return progs[i].name; }
static cairo_surface_t *prog_icon_at(int i) { return prog_icon(&progs[i]); }

struct pkg_hit { char name[64], desc[160], label[232]; };
static struct pkg_hit pkg_hits[200];
static int n_pkg_hits, pkg_sel = -1, pkg_caret;
static char pkg_query[128];

static const char *pkg_label(int i) { return pkg_hits[i].label; }

static void pkg_search(void) {
	n_pkg_hits = 0;
	pkg_sel = -1;
	if (!pkg_query[0])
		return;
	char q[300], cmd[512];
	snprintf(cmd, sizeof(cmd), "apt-cache search --names-only %s | sort | head -n 200", ui_shell_quote(pkg_query, q, sizeof(q)));
	char *out = capture(cmd), *save;
	for (char *line = strtok_r(out, "\n", &save); line && n_pkg_hits < 200; line = strtok_r(NULL, "\n", &save)) {
		char *sep = strstr(line, " - ");
		if (!sep)
			continue;
		*sep = 0;
		struct pkg_hit *h = &pkg_hits[n_pkg_hits++];
		snprintf(h->name, sizeof(h->name), "%s", line);
		snprintf(h->desc, sizeof(h->desc), "%s", sep + 3);
		snprintf(h->label, sizeof(h->label), "%s  —  %s", line, sep + 3);
	}
	free(out);
	if (n_pkg_hits)
		pkg_sel = 0;
}

static void pkg_install(void) {
	if (pkg_sel < 0 || pkg_sel >= n_pkg_hits)
		return;
	char q[160], cmd[512];
	ui_shell_quote(pkg_hits[pkg_sel].name, q, sizeof(q));
	snprintf(cmd, sizeof(cmd), "foot -T 'Add New Programs' sh -c \"sudo apt install %s; echo; read -p 'Press Enter to close.' x\"", q);
	spawn(cmd);
}

static void paint_programs(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	ui_fill(cr, 0, 0, w->w, w->h, 0xece9d8);
	/* XP's bar of big buttons down the left */
	ui_fill(cr, 0, 0, 96, w->h, 0x7a96df);
	static const struct { int id; const char *label, *icon; } side[] = {
		{ID_CHANGE_PROGS, "Change or\nRemove\nPrograms", "cp-programs"}, {ID_ADD_PROGS, "Add New\nPrograms", "cp-programs"},
		{ID_APPLET + K_DEFAULTS, "Set Program\nAccess and\nDefaults", "cp-system"},
	};
	for (int i = 0; i < 3; i++) {
		int y = 10 + i * 96;
		bool cur = (i == 0 && s->tab == 0) || (i == 1 && s->tab == 1), hot = w->hot == side[i].id;
		if (cur || hot)
			ui_fill(cr, 4, y, 88, 88, cur ? 0x9db2ec : 0x8aa5e8);
		ui_icon(cr, icon(side[i].icon)->s32, 32, y + 6, 32);
		draw_label(cr, "Tahoma 8", side[i].label, 48, y + 42, 86, 0xffffff, 0x000000, -1);
		ui_reg(w, side[i].id, 4, y, 88, 88);
	}
	if (s->tab == 1) {
		ui_text(cr, "Tahoma Bold 8", "Add a program from Debian:", 108, 12, 0, 0x000000);
		ui_field(w, cr, ID_PKG_QUERY, 106, 32, w->w - 286, 22, pkg_query, true, pkg_caret);
		ui_button(w, cr, ID_SEARCH_PKGS, w->w - 172, 31, 74, "Search", false, true);
		ui_button(w, cr, ID_PKG_TERM, w->w - 92, 31, 80, "Terminal...", false, false);
		listbox(w, cr, ID_LIST, 106, 62, w->w - 118, w->h - 154, n_pkg_hits, pkg_sel, &s->scroll, &s->list_box, pkg_label, NULL, 20);
		if (!n_pkg_hits)
			hint(cr, 116, 74, "Type a program or package name above and click Search (or press Enter).");
		if (pkg_sel >= 0 && pkg_sel < n_pkg_hits) {
			ui_text(cr, "Tahoma 8", "To install this program on your computer, click Add.", 108, w->h - 82, w->w - 210, 0x000000);
			ui_button(w, cr, ID_INSTALL_PKG, w->w - 92, w->h - 86, 80, "Add", false, false);
		}
	} else {
		ui_text(cr, "Tahoma Bold 8", "Currently installed programs:", 108, 12, 0, 0x000000);
		listbox(w, cr, ID_LIST, 106, 32, w->w - 118, w->h - 124, n_progs, prog_sel, &s->scroll, &s->list_box, prog_label, prog_icon_at, 24);
		if (prog_sel >= 0 && prog_sel < n_progs) {
			ui_text(cr, "Tahoma 8", "To remove this program from your computer, click Remove.", 108, w->h - 82, w->w - 210, 0x000000);
			ui_button(w, cr, ID_REMOVE_PROG, w->w - 92, w->h - 86, 80, "Remove", false, true);
		}
	}
	ui_button(w, cr, ID_CANCEL, w->w - 87, w->h - 36, 75, "Close", false, false);
}

static const char *kind_label(int i) {
	static char buf[200];
	char name[128] = "(none)";
	for (int p = 0; p < n_progs; p++)
		if (!strcmp(strrchr(progs[p].file, '/') + 1, kind_default[i]))
			snprintf(name, sizeof(name), "%s", progs[p].name);
	snprintf(buf, sizeof(buf), "%s:  %s", kinds[i].label, name);
	return buf;
}

static int cand[256], n_cand;
static const char *cand_label(int i) { return progs[cand[i]].name; }
static cairo_surface_t *cand_icon(int i) { return prog_icon(&progs[cand[i]]); }

static void paint_defaults(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	ui_text(cr, "Tahoma 8", "Choose the program that opens each kind of file:", 18, 40, 0, 0x000000);
	listbox(w, cr, ID_LIST, 18, 58, SHEET_W - 36, 150, N_KINDS, kind_sel, &s->scroll, &s->list_box, kind_label, NULL, 17);
	n_cand = 0;
	int cur = -1;
	for (int p = 0; p < n_progs && n_cand < 256; p++)
		if (prog_handles(&progs[p], kind_sel)) {
			if (!strcmp(strrchr(progs[p].file, '/') + 1, kind_default[kind_sel]))
				cur = n_cand;
			cand[n_cand++] = p;
		}
	char t[96];
	snprintf(t, sizeof(t), "Programs that can open %s:", kinds[kind_sel].label);
	ui_text(cr, "Tahoma 8", t, 18, 220, 0, 0x000000);
	listbox(w, cr, ID_LIST2, 18, 238, SHEET_W - 36, 140, n_cand, cur, &s->scroll2, &s->list2_box, cand_label, cand_icon, 22);
	hint(cr, 18, 384, "Click a program to make it the default.");
}

/* ---------------------------------------------------------------- Sounds */

static int out_ids[32], in_ids[32], n_out_snd, n_in_snd;
static const char *out_snd_label(int i) { return snds[out_ids[i]].name; }
static const char *in_snd_label(int i) { return snds[in_ids[i]].name; }

static void paint_sounds(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		ui_icon(cr, icon("cp-sounds")->s32, 24, 44, 32);
		ui_text(cr, "Tahoma 8", device, 66, 54, SHEET_W - 90, 0x000000);
		ui_group(cr, 16, 92, SHEET_W - 32, 118, "Device volume");
		if (have_vol) {
			ui_text(cr, "Tahoma 8", "Low", 30, 122, 0, 0x000000);
			ui_text(cr, "Tahoma 8", "High", SHEET_W - 60, 122, 0, 0x000000);
			slider(w, cr, ID_VOLUME, 60, 118, SHEET_W - 130, vol > 100 ? 100 : vol, s->dragging && w->pressed == ID_VOLUME);
			ui_check(w, cr, ID_MUTE, 30, 168, muted, false, "Mute");
		} else {
			hint(cr, 30, 122, "No volume control found (wpctl, pactl or amixer).");
		}
	} else if (s->tab == 1) {
		ui_group(cr, 16, 40, SHEET_W - 32, 100, "Sound scheme");
		ui_check(w, cr, ID_SCHEME_DEFAULT, 28, 64, cfg.sounds, true, "Windows Default");
		ui_check(w, cr, ID_SCHEME_NONE, 28, 88, !cfg.sounds, true, "No Sounds");
		ui_button(w, cr, ID_TEST_SOUND, SHEET_W - 112, 62, 80, "Test", false, false);
		hint(cr, 47, 112, "Startup, log on and off, shutdown, minimize and restore.");
	} else {
		n_out_snd = n_in_snd = 0;
		int out_sel = -1, in_sel = -1;
		for (int i = 0; i < n_snd; i++) {
			if (snds[i].input) {
				if (snds[i].def) in_sel = n_in_snd;
				in_ids[n_in_snd++] = i;
			} else {
				if (snds[i].def) out_sel = n_out_snd;
				out_ids[n_out_snd++] = i;
			}
		}
		if (!n_snd) {
			hint(cr, 24, 50, "Choosing devices needs PipeWire (wpctl).");
			return;
		}
		ui_group(cr, 16, 40, SHEET_W - 32, 128, "Sound playback: default device");
		listbox(w, cr, ID_LIST, 28, 62, SHEET_W - 56, 94, n_out_snd, out_sel, &s->scroll, &s->list_box, out_snd_label, NULL, 18);
		ui_group(cr, 16, 180, SHEET_W - 32, 170, "Sound recording: default device");
		listbox(w, cr, ID_LIST2, 28, 202, SHEET_W - 56, 76, n_in_snd, in_sel, &s->scroll2, &s->list2_box, in_snd_label, NULL, 18);
		ui_text(cr, "Tahoma 8", "Volume:", 28, 296, 0, 0x000000);
		slider(w, cr, ID_MIC, 80, 292, SHEET_W - 120, mic_vol > 100 ? 100 : mic_vol, s->dragging && w->pressed == ID_MIC);
	}
}

/* ---------------------------------------------------------------- System, Power */

static void paint_system(struct sheet *s, cairo_t *cr) {
	cairo_surface_t *logo = theme_png(&ui_theme, "logon/logo.png");
	if (logo) { /* the Windows logo the Welcome screen uses, on a blue tile */
		ui_fill(cr, 24, 48, 160, 110, 0x5a7edc);
		ui_icon(cr, logo, 36, 60, 137);
		cairo_surface_destroy(logo);
	}
	int x = 206, y = 48, mw = SHEET_W - x - 24;
	ui_text(cr, "Tahoma 8", "System:", x, y, 0, 0x000000);
	ui_text(cr, "Tahoma 8", sys_os, x + 14, y + 16, mw, 0x000000);
	ui_text(cr, "Tahoma 8", sys_kernel, x + 14, y + 32, mw, 0x000000);
	ui_text(cr, "Tahoma 8", "Expede desktop", x + 14, y + 48, mw, 0x000000);
	ui_text(cr, "Tahoma 8", "Registered to:", x, y + 80, 0, 0x000000);
	ui_text(cr, "Tahoma 8", getenv("USER") ? getenv("USER") : "", x + 14, y + 96, 0, 0x000000);
	ui_text(cr, "Tahoma 8", "Computer:", x, y + 128, 0, 0x000000);
	ui_text(cr, "Tahoma 8", sys_cpu, x + 14, y + 144, mw, 0x000000);
	ui_text(cr, "Tahoma 8", sys_ram, x + 14, y + 176, 0, 0x000000);
	ui_text(cr, "Tahoma 8", "Computer name:", x, y + 208, 0, 0x000000);
	ui_text(cr, "Tahoma 8", sys_host, x + 14, y + 224, 0, 0x000000);
	(void)s;
}

static void paint_power(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		ui_group(cr, 16, 40, SHEET_W - 32, 110, "Turn off monitor");
		static const char *lbl[] = {"After 1 min", "After 5 mins", "After 10 mins", "After 15 mins", "After 30 mins", "Never"};
		static const int mins[] = {1, 5, 10, 15, 30, 0};
		for (int i = 0; i < 6; i++)
			ui_check(w, cr, ID_SCREEN_OFF + i, 30 + (i % 2) * 180, 64 + (i / 2) * 24, cfg.screen_off == mins[i], true, lbl[i]);
		ui_group(cr, 16, 162, SHEET_W - 32, 112, "Power scheme");
		if (have_profiles) {
			ui_check(w, cr, ID_PROFILE_SAVER, 30, 186, !strcmp(profile, "power-saver"), true, "Max Battery");
			ui_check(w, cr, ID_PROFILE_BALANCED, 30, 210, !strcmp(profile, "balanced"), true, "Portable/Laptop (balanced)");
			ui_check(w, cr, ID_PROFILE_PERF, 30, 234, !strcmp(profile, "performance"), true, "Always On (performance)");
		} else {
			hint(cr, 30, 186, "Install power-profiles-daemon to switch power schemes.");
		}
		hint(cr, 18, 288, "Videos that are playing keep the screen on.");
	} else {
		if (!have_battery) {
			hint(cr, 24, 50, "This computer has no battery: it runs on AC power.");
			return;
		}
		char pct[24];
		snprintf(pct, sizeof(pct), "%s%%", battery_pct);
		ui_icon(cr, icon("cp-power")->s32, 30, 50, 32);
		ui_text(cr, "Trebuchet MS, Bold 14", pct, 76, 52, 0, 0x0a246a);
		ui_text(cr, "Tahoma 8", battery_state, 78, 80, 0, 0x000000);
		/* XP's green block meter */
		int p = atoi(battery_pct);
		struct image track = {.s = theme_png(&ui_theme, "luna/progresstrack.png"), .count = 1, .m = {4, 4, 3, 3}};
		draw_image(cr, &track, 0, 30, 110, SHEET_W - 60, 18);
		if (track.s)
			cairo_surface_destroy(track.s);
		cairo_surface_t *chunk = theme_png(&ui_theme, "luna/progresschunk.png");
		for (int x = 33; chunk && x + 8 < 30 + (SHEET_W - 66) * p / 100; x += 10) {
			cairo_set_source_surface(cr, chunk, x, 113);
			cairo_paint(cr);
		}
		if (chunk)
			cairo_surface_destroy(chunk);
	}
}

/* ---------------------------------------------------------------- Mouse, Bluetooth */

static void paint_mouse(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		ui_group(cr, 16, 40, SHEET_W - 32, 90, "Motion");
		ui_text(cr, "Tahoma 8", "Select a pointer speed:", 30, 62, 0, 0x000000);
		ui_text(cr, "Tahoma 8", "Slow", 30, 92, 0, 0x000000);
		ui_text(cr, "Tahoma 8", "Fast", SHEET_W - 60, 92, 0, 0x000000);
		slider(w, cr, ID_SPEED, 64, 88, SHEET_W - 136, (cfg.speed + 100) / 2, s->dragging && w->pressed == ID_SPEED);
	} else {
		ui_group(cr, 16, 40, SHEET_W - 32, 132, "Touchpad");
		ui_check(w, cr, ID_TAP, 28, 64, cfg.tap, false, "Tap the touchpad to click");
		ui_check(w, cr, ID_NATURAL, 28, 100, cfg.natural, false, "Natural scrolling");
		hint(cr, 47, 120, "Content follows your fingers, like a phone. Mouse wheels\nalways scroll the usual way.");
	}
}

static const char *bt_label(int i) {
	static char buf[128];
	snprintf(buf, sizeof(buf), "%s%s", bts[i].name, bts[i].connected ? "   (Connected)" : bts[i].paired ? "   (Paired)" : "");
	return buf;
}

static void paint_bluetooth(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (!have_bt) {
		hint(cr, 24, 50, "No Bluetooth adapter found (or BlueZ isn't installed).");
		return;
	}
	ui_check(w, cr, ID_BT_POWER, 18, 40, bt_powered, false, "Turn on Bluetooth");
	listbox(w, cr, ID_LIST, 18, 66, SHEET_W - 36, 230, n_bt, bt_sel, &s->scroll, &s->list_box, bt_label, NULL, 22);
	bool sel = bt_sel >= 0 && bt_sel < n_bt;
	ui_button(w, cr, ID_BT_SCAN, 18, 306, 80, "Add...", !bt_powered, false);
	ui_button(w, cr, sel && bts[bt_sel].connected ? ID_BT_DISCONNECT : ID_BT_CONNECT, 104, 306, 80,
		sel && bts[bt_sel].connected ? "Disconnect" : "Connect", !sel || !bt_powered, false);
	ui_button(w, cr, ID_BT_REMOVE, 190, 306, 80, "Remove", !sel, false);
	hint(cr, 18, 340, "Put a device in pairing mode, then click Add to search\nfor it for a few seconds.");
}

/* ---------------------------------------------------------------- Users, Date and time */

static void paint_users(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	const char *user = getenv("USER") ? getenv("USER") : "User";
	cairo_surface_t *cur = NULL;
	size_t pl = strlen(cfg.picture);
	for (int i = 0; i < n_pictures; i++)
		if (pl && !strncmp(pictures[i].value, cfg.picture, pl) && pictures[i].value[pl] == '.')
			cur = pictures[i].img;
	draw_image(cr, &ui_theme.user_tile, 0, 22, 40, 56, 56);
	ui_icon(cr, cur ? cur : ui_theme.user_pic, 26, 44, 48);
	ui_text(cr, "Trebuchet MS, Bold 12", user, 90, 50, 0, 0x0a246a);
	hint(cr, 90, 74, "Pick a new picture for your account.");
	for (int i = 0; i < n_pictures; i++) {
		int col = i % 6, row = i / 6, x = 24 + col * 64, y = 112 + row * 62;
		bool sel = cur == pictures[i].img, h = w->hot == ID_LIST + i;
		if (sel || h)
			ui_fill(cr, x - 3, y - 3, 54, 54, sel ? 0x316ac5 : 0x9bb4e5);
		ui_icon(cr, pictures[i].img, x, y, 48);
		ui_reg(w, ID_LIST + i, x - 3, y - 3, 54, 54);
	}
}

static const char *layout_label(int i) { return layouts[i].label; }
static const char *zone_label(int i) { return zones[i]; }

static int layout_index(void) {
	for (int i = 0; i < N_LAYOUTS; i++)
		if (!strcmp(layouts[i].value, cfg.keyboard))
			return i;
	return 0;
}

static void paint_datetime(struct sheet *s, cairo_t *cr) {
	struct ui_win *w = s->win;
	if (s->tab == 0) {
		time_t now = time(NULL);
		struct tm tm;
		localtime_r(&now, &tm);
		char big[32], date[64];
		strftime(big, sizeof(big), cfg.clock24 ? "%H:%M" : "%-I:%M %p", &tm);
		strftime(date, sizeof(date), "%A, %B %-d, %Y", &tm);
		ui_text(cr, "Trebuchet MS, Bold 20", big, 30, 46, 0, 0x0a246a);
		ui_text(cr, "Tahoma 8", date, 32, 84, 0, 0x000000);
		ui_group(cr, 16, 112, SHEET_W - 32, 80, "Clock in the notification area");
		ui_check(w, cr, ID_CLOCK12, 28, 136, !cfg.clock24, true, "12-hour (3:05 PM)");
		ui_check(w, cr, ID_CLOCK24, 28, 160, cfg.clock24, true, "24-hour (15:05)");
		hint(cr, 18, 206, "The time is kept right automatically over the internet.");
	} else if (s->tab == 1) {
		char cur[160];
		snprintf(cur, sizeof(cur), "Current time zone:  %s", cur_zone);
		ui_text(cr, "Tahoma 8", cur, 18, 40, 0, 0x000000);
		listbox(w, cr, ID_LIST, 18, 60, SHEET_W - 36, 270, n_zones, zone_sel, &s->scroll, &s->list_box, zone_label, NULL, 17);
		ui_button(w, cr, ID_SET_ZONE, SHEET_W - 136, 340, 118, "Set Time Zone", zone_sel < 0, false);
		hint(cr, 18, 344, "Needs your password.");
	} else {
		ui_text(cr, "Tahoma 8", "Keyboard layout:", 18, 44, 0, 0x000000);
		listbox(w, cr, ID_LIST, 18, 62, SHEET_W - 36, 250, N_LAYOUTS, layout_index(), &s->scroll, &s->list_box, layout_label, NULL, 17);
		hint(cr, 18, 322, "Takes effect as soon as you click Apply.");
	}
}

/* ================================================================== the Control Panel window */

static void task_link(struct ui_win *w, cairo_t *cr, int id, int x, int y, const char *ic, const char *label) {
	bool hot = w->hot == id;
	int tw = text_width("Tahoma 8", label);
	ui_icon(cr, icon(ic)->s16, x, y, 16);
	ui_text(cr, "Tahoma 8", label, x + 22, y + 1, 150, hot ? 0x428eff : 0x215dc6);
	if (hot)
		ui_fill(cr, x + 22, y + 14, tw < 150 ? tw : 150, 1, 0x428eff);
	ui_reg(w, id, x - 4, y - 3, 176, 22);
}

static void paint_cp(struct ui_win *w, cairo_t *cr) {
	ui_fill(cr, 0, 0, w->w, w->h, 0xffffff);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, w->h);
	cairo_pattern_add_color_stop_rgb(g, 0, 0x7b / 255.0, 0xa2 / 255.0, 0xe7 / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, 0x63 / 255.0, 0x75 / 255.0, 0xd6 / 255.0);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, 0, 0, 200, w->h);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	int bx = 12, by = 12, bw = 176;
	g = cairo_pattern_create_linear(bx, 0, bx + bw, 0);
	cairo_pattern_add_color_stop_rgb(g, 0, 1, 1, 1);
	cairo_pattern_add_color_stop_rgb(g, 1, 0xc6 / 255.0, 0xd3 / 255.0, 0xf7 / 255.0);
	cairo_set_source(cr, g);
	cairo_rectangle(cr, bx, by, bw, 24);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	ui_icon(cr, icon("control-panel")->s32, bx + 4, by + 2, 20);
	ui_text(cr, "Tahoma Bold 8", "Control Panel", bx + 30, by + 6, 0, 0x215dc6);
	ui_fill(cr, bx, by + 24, bw, cp_category >= 0 ? 56 : 34, 0xd6dff7);
	task_link(w, cr, ID_VIEW_TOGGLE, bx + 8, by + 34, "cp-display", classic_view ? "Switch to Category View" : "Switch to Classic View");
	if (cp_category >= 0)
		task_link(w, cr, ID_HOME, bx + 8, by + 56, "control-panel", "Back to categories");

	int cx = 220;
	if (classic_view) {
		for (int k = K_DISPLAY; k < K_COUNT; k++) {
			int i = k - K_DISPLAY, col = i % 4, row = i / 4, x = cx + col * 105, y = 20 + row * 100;
			if (w->hot == ID_APPLET + k)
				ui_fill(cr, x, y, 96, 84, 0xe8eefa);
			ui_icon(cr, icon(applets[k].icon)->s32, x + 32, y + 6, 32);
			draw_label(cr, "Tahoma 8", applets[k].name, x + 48, y + 44, 92, 0x000000, 0xffffff, -1);
			ui_reg(w, ID_APPLET + k, x, y, 96, 84);
		}
		return;
	}
	if (cp_category < 0) {
		ui_text(cr, "Trebuchet MS, Bold 14", "Pick a category", cx, 24, 0, 0x4d6185);
		ui_fill(cr, cx, 52, w->w - cx - 20, 1, 0xd6dff7);
		for (int c = 0; c < N_CATS; c++) {
			int x = cx + (c % 2) * 215, y = 70 + (c / 2) * 64;
			bool h = w->hot == ID_CATEGORY + c;
			ui_icon(cr, cat_icons[c].s32, x, y, 32);
			/* long names break onto a second line, as in XP */
			const char *nm = categories[c].name;
			ui_text(cr, "Tahoma 8", nm, x + 42, y + (strchr(nm, '\n') ? 2 : 9), 0, h ? 0x428eff : 0x215dc6);
			ui_reg(w, ID_CATEGORY + c, x, y, 205, 40);
		}
		return;
	}
	const struct category *cat = &categories[cp_category];
	ui_icon(cr, cat_icons[cp_category].s32, cx, 18, 32);
	char title[96];
	snprintf(title, sizeof(title), "%s", cat->name);
	for (char *nl = strchr(title, '\n'); nl; nl = strchr(nl, '\n'))
		*nl = ' ';
	ui_text(cr, "Trebuchet MS, Bold 14", title, cx + 42, 22, w->w - cx - 60, 0x4d6185);
	ui_fill(cr, cx, 56, w->w - cx - 20, 1, 0xd6dff7);
	ui_text(cr, "Tahoma 8", cat->blurb, cx, 66, w->w - cx - 30, 0x000000);
	ui_text(cr, "Trebuchet MS, Bold 10", "or pick a Control Panel icon", cx, 100, 0, 0x4d6185);
	for (int i = 0; i < 4 && cat->items[i]; i++) {
		enum kind k = cat->items[i];
		int x = cx + (i % 2) * 215, y = 130 + (i / 2) * 50;
		bool h = w->hot == (int)(ID_APPLET + k);
		ui_icon(cr, icon(applets[k].icon)->s32, x, y, 32);
		ui_text(cr, "Tahoma 8", applets[k].name, x + 42, y + 9, 160, h ? 0x428eff : 0x215dc6);
		ui_reg(w, ID_APPLET + k, x, y, 205, 36);
	}
}

/* ================================================================== events */

static void open_applet(enum kind k);

static void sheet_paint(struct ui_win *w, cairo_t *cr) {
	struct sheet *s = w->data;
	if (s->kind == K_PROGRAMS) {
		paint_programs(s, cr);
		return;
	}
	tabs_and_buttons(s, cr);
	switch (s->kind) {
	case K_DISPLAY: paint_display(s, cr); break;
	case K_PERSONALIZE: paint_personalize(s, cr); break;
	case K_TASKBAR: paint_taskbar(s, cr); break;
	case K_NETWORK: paint_network(s, cr); break;
	case K_DEFAULTS: paint_defaults(s, cr); break;
	case K_SOUNDS: paint_sounds(s, cr); break;
	case K_SYSTEM: paint_system(s, cr); break;
	case K_POWER: paint_power(s, cr); break;
	case K_MOUSE: paint_mouse(s, cr); break;
	case K_BLUETOOTH: paint_bluetooth(s, cr); break;
	case K_USERS: paint_users(s, cr); break;
	case K_DATETIME: paint_datetime(s, cr); break;
	default: break;
	}
}

static void close_sheet(struct sheet *s, bool cancel) {
	if (cancel)
		cfg = saved;
	ui_destroy(s->win);
	s->win = NULL;
}

static void sheet_close(struct ui_win *w) {
	close_sheet(w->data, true); /* the title bar's X is Cancel */
}

static void list_pick(struct sheet *s, int i, bool second, int clicks) {
	switch (s->kind) {
	case K_DISPLAY:
		if (s->tab == 0 && i < n_wallpapers) {
			snprintf(cfg.wallpaper, sizeof(cfg.wallpaper), "%s", wallpapers[i].value);
		} else if (s->tab == 2 && n_screens) {
			struct screen *sc = &screens[screen_sel];
			int c = out_index(sc->name, true);
			if (c >= 0 && i < sc->n_modes)
				snprintf(cfg.out[c].mode, sizeof(cfg.out[c].mode), "%s", sc->modes[i]);
		}
		break;
	case K_NETWORK:
		wifi_sel = i;
		memset(wifi_password, 0, sizeof(wifi_password));
		wifi_pw_caret = 0;
		net_message[0] = 0;
		break;
	case K_PROGRAMS:
		if (s->tab == 1) {
			pkg_sel = i;
			if (clicks == 2)
				pkg_install();
		} else {
			prog_sel = i;
			if (clicks == 2 && progs[i].exec) {
				char cmd[1024];
				exec_line(progs[i].exec, cmd, sizeof(cmd));
				spawn(cmd);
			}
		}
		break;
	case K_DEFAULTS:
		if (!second) {
			kind_sel = i;
			s->scroll2 = 0;
		} else if (i < n_cand) {
			const char *file = strrchr(progs[cand[i]].file, '/') + 1;
			for (int m = 0; m < 4 && kinds[kind_sel].mimes[m]; m++) {
				char cmd[400];
				snprintf(cmd, sizeof(cmd), "xdg-mime default '%s' %s", file, kinds[kind_sel].mimes[m]);
				run(cmd);
			}
			defaults_refresh();
		}
		break;
	case K_SOUNDS:
		if (s->tab == 2) {
			int idx = second ? in_ids[i] : out_ids[i];
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "wpctl set-default %d", snds[idx].id);
			run(cmd);
			sound_refresh();
		}
		break;
	case K_BLUETOOTH: bt_sel = i; break;
	case K_USERS:
		if (i < n_pictures) {
			snprintf(cfg.picture, sizeof(cfg.picture), "%s", pictures[i].value);
			*strrchr(cfg.picture, '.') = 0;
		}
		break;
	case K_DATETIME:
		if (s->tab == 1)
			zone_sel = i;
		else if (s->tab == 2 && i < N_LAYOUTS)
			snprintf(cfg.keyboard, sizeof(cfg.keyboard), "%s", layouts[i].value);
		break;
	default: break;
	}
}

static void wifi_connect(void) {
	if (wifi_sel < 0 || wifi_sel >= n_wifi)
		return;
	char q1[200], q2[300], cmd[600];
	if (wifi_password[0])
		snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s password %s >/dev/null 2>&1",
			ui_shell_quote(wifis[wifi_sel].ssid, q1, sizeof(q1)), ui_shell_quote(wifi_password, q2, sizeof(q2)));
	else
		snprintf(cmd, sizeof(cmd), "nmcli dev wifi connect %s >/dev/null 2>&1", ui_shell_quote(wifis[wifi_sel].ssid, q1, sizeof(q1)));
	int r = run(cmd);
	memset(cmd, 0, sizeof(cmd)); /* the key was in there */
	memset(q2, 0, sizeof(q2));
	snprintf(net_message, sizeof(net_message), "%s", r == 0 ? "" : "Windows was unable to connect to this network. Check the network key.");
	memset(wifi_password, 0, sizeof(wifi_password));
	wifi_pw_caret = 0;
	net_refresh(false);
}

static void bt_cmd(const char *verb) {
	if (bt_sel < 0 || bt_sel >= n_bt)
		return;
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "bluetoothctl %s %s >/dev/null 2>&1", verb, bts[bt_sel].mac);
	run(cmd);
}

static void sheet_button(struct ui_win *w, int id, int button, bool pressed, double x, double y, int clicks, int mods) {
	struct sheet *s = w->data;
	if (button != UI_LEFT)
		return;
	if (pressed) {
		if (id == ID_VOLUME || id == ID_SPEED || id == ID_MIC || id == ID_GLASS_INTENSITY) {
			s->dragging = true;
			w->motion(w, x, y, true);
		}
		if (id >= ID_LIST && clicks == 2) /* double clicks act on press */
			list_pick(s, id >= ID_LIST2 ? id - ID_LIST2 : id - ID_LIST, id >= ID_LIST2, 2);
		return;
	}
	if (s->dragging) {
		s->dragging = false;
		return;
	}
	if (id != w->pressed || !id)
		return;
	if (id >= ID_LIST2) { list_pick(s, id - ID_LIST2, true, 1); redraw_all(); return; }
	if (id >= ID_LIST) { list_pick(s, id - ID_LIST, false, 1); redraw_all(); return; }
	if (id >= ID_PART_OVR && id < ID_PART_OVR + 21) {
		static const char *vals[3] = {"auto", "xp", "7"};
		int p = (id - ID_PART_OVR) / 3, c = (id - ID_PART_OVR) % 3;
		snprintf(part_val(p), 16, "%s", vals[c]);
		redraw_all();
		return;
	}
	if (id >= ID_SWATCH && id < ID_SWATCH + N_SWATCHES) {
		snprintf(cfg.glass_color, sizeof(cfg.glass_color), "%s", swatches[id - ID_SWATCH].name);
		redraw_all();
		return;
	}
	if (id >= ID_APPLET && id < ID_APPLET + K_COUNT) { open_applet(id - ID_APPLET); return; }
	if (id >= ID_SCREEN && id < ID_SCREEN + 8) { screen_sel = id - ID_SCREEN; s->scroll = 0; return; }
	if (id >= ID_SCREEN_OFF && id < ID_SCREEN_OFF + 6) {
		static const int mins[] = {1, 5, 10, 15, 30, 0};
		cfg.screen_off = mins[id - ID_SCREEN_OFF];
		redraw_all();
		return;
	}
	if (id >= ID_SCALE && id < ID_SCALE + 6) {
		static const int pcts[6] = {0, 100, 125, 150, 175, 200};
		cfg.scale_pct = pcts[id - ID_SCALE];
		redraw_all();
		return;
	}
	if (id >= ID_TAB && id < ID_TAB + 4) {
		s->tab = id - ID_TAB;
		s->scroll = s->scroll2 = 0;
		if (s->kind == K_SOUNDS)
			sound_refresh();
		return;
	}
	switch (id) {
	case ID_SNAP: cfg.snap = !cfg.snap; break;
	case ID_XP_STYLE: snprintf(cfg.style, sizeof(cfg.style), "xp"); break;
	case ID_W7_STYLE: snprintf(cfg.style, sizeof(cfg.style), "7"); break;
	case ID_GLASS_ON: cfg.glass = !cfg.glass; break;
	case ID_OPEN_PERSONALIZE: open_applet(K_PERSONALIZE); break;
	case ID_START_XP: snprintf(cfg.start_menu, sizeof(cfg.start_menu), "xp"); break;
	case ID_START_SEARCH: snprintf(cfg.start_menu, sizeof(cfg.start_menu), "modern"); break;
	case ID_START_W7: snprintf(cfg.start_menu, sizeof(cfg.start_menu), "7"); break;
	case ID_TAP: cfg.tap = !cfg.tap; break;
	case ID_NATURAL: cfg.natural = !cfg.natural; break;
	case ID_SCHEME_DEFAULT: cfg.sounds = true; break;
	case ID_SCHEME_NONE: cfg.sounds = false; break;
	case ID_CLOCK12: cfg.clock24 = false; break;
	case ID_CLOCK24: cfg.clock24 = true; break;
	case ID_STRETCH: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "stretch"); break;
	case ID_CENTER: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "center"); break;
	case ID_TILE: snprintf(cfg.wall_style, sizeof(cfg.wall_style), "tile"); break;
	case ID_MUTE: muted = !muted; volume_mute(muted); break;
	case ID_SCREEN_ENABLED: {
		int c = out_index(screens[screen_sel].name, true);
		if (c >= 0)
			cfg.out[c].disabled = !cfg.out[c].disabled;
		break;
	}
	case ID_TEST_SOUND: {
		char cmd[1200];
		snprintf(cmd, sizeof(cmd), "pw-play '%1$s/sounds/ding.wav' 2>/dev/null || paplay '%1$s/sounds/ding.wav' 2>/dev/null || aplay -q '%1$s/sounds/ding.wav'", ui_theme.dir);
		spawn(cmd);
		break;
	}
	case ID_WIFI_ON:
		run(wifi_on ? "nmcli radio wifi off" : "nmcli radio wifi on");
		net_refresh(false);
		break;
	case ID_WIFI_REFRESH: net_refresh(true); break;
	case ID_WIFI_CONNECT: wifi_connect(); break;
	case ID_WIFI_DISCONNECT:
		if (wifi_sel >= 0 && wifi_sel < n_wifi) {
			char q[200], cmd[300];
			snprintf(cmd, sizeof(cmd), "nmcli con down id %s >/dev/null 2>&1", ui_shell_quote(wifis[wifi_sel].ssid, q, sizeof(q)));
			run(cmd);
			net_refresh(false);
		}
		break;
	case ID_BT_POWER:
		run(bt_powered ? "bluetoothctl power off >/dev/null" : "bluetoothctl power on >/dev/null");
		bt_refresh();
		break;
	case ID_BT_SCAN:
		run("bluetoothctl --timeout 8 scan on >/dev/null 2>&1");
		bt_refresh();
		break;
	case ID_BT_CONNECT:
		if (bt_sel >= 0 && bt_sel < n_bt && !bts[bt_sel].paired) {
			bt_cmd("pair");
			bt_cmd("trust");
		}
		bt_cmd("connect");
		bt_refresh();
		break;
	case ID_BT_DISCONNECT: bt_cmd("disconnect"); bt_refresh(); break;
	case ID_BT_REMOVE: bt_cmd("remove"); bt_sel = -1; bt_refresh(); break;
	case ID_PROFILE_SAVER: run("powerprofilesctl set power-saver"); power_refresh(); break;
	case ID_PROFILE_BALANCED: run("powerprofilesctl set balanced"); power_refresh(); break;
	case ID_PROFILE_PERF: run("powerprofilesctl set performance"); power_refresh(); break;
	case ID_SET_ZONE:
		if (zone_sel >= 0) {
			char q[200], cmd[256];
			snprintf(cmd, sizeof(cmd), "timedatectl set-timezone %s", ui_shell_quote(zones[zone_sel], q, sizeof(q)));
			run(cmd);
			zones_refresh();
		}
		break;
	case ID_REMOVE_PROG:
		if (prog_sel >= 0 && prog_sel < n_progs) {
			/* uninstalling needs the package manager and your password: do it in a Command Prompt you can see */
			char q[1100], cmd[1400];
			const char *f = progs[prog_sel].file;
			ui_shell_quote(f, q, sizeof(q));
			if (strstr(f, "/flatpak/"))
				snprintf(cmd, sizeof(cmd), "foot -T 'Remove Program' sh -c \"flatpak uninstall \\$(basename %s .desktop); echo; read -p 'Press Enter to close.' x\"", q);
			else
				snprintf(cmd, sizeof(cmd), "foot -T 'Remove Program' sh -c \"sudo apt remove \\$(dpkg -S %s | cut -d: -f1 | head -1); echo; read -p 'Press Enter to close.' x\"", q);
			spawn(cmd);
		}
		break;
	case ID_CHANGE_PROGS:
		s->tab = 0;
		s->scroll = 0;
		progs_refresh();
		break;
	case ID_ADD_PROGS:
		s->tab = 1;
		s->scroll = 0;
		break;
	case ID_SEARCH_PKGS:
		s->scroll = 0;
		pkg_search();
		break;
	case ID_INSTALL_PKG:
		pkg_install();
		break;
	case ID_PKG_TERM:
		spawn("foot -T 'Add New Programs' sh -c \"echo 'Install programs with:  sudo apt install <name>'; echo; exec bash\"");
		break;
	case ID_APPLY: config_save(); break;
	case ID_OK:
		if (memcmp(&cfg, &saved, sizeof(cfg)))
			config_save();
		close_sheet(s, false);
		return;
	case ID_CANCEL:
		close_sheet(s, !is_live(s->kind));
		return;
	}
	redraw_all();
}

static void sheet_motion(struct ui_win *w, double x, double y, bool held) {
	struct sheet *s = w->data;
	if (!s->dragging || !held)
		return;
	if (w->pressed == ID_VOLUME) {
		int v = slider_value(w, ID_VOLUME, x);
		if (v != vol) {
			vol = v;
			volume_write(v);
		}
	} else if (w->pressed == ID_MIC) {
		int v = slider_value(w, ID_MIC, x);
		if (v != mic_vol) {
			mic_vol = v;
			char cmd[96];
			snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SOURCE@ %d%%", v);
			spawn(cmd);
		}
	} else if (w->pressed == ID_SPEED) {
		cfg.speed = slider_value(w, ID_SPEED, x) * 2 - 100;
	} else if (w->pressed == ID_GLASS_INTENSITY) {
		cfg.glass_intensity = slider_value(w, ID_GLASS_INTENSITY, x);
	}
	redraw_all();
}

static void sheet_scroll(struct ui_win *w, double dy, double x, double y) {
	struct sheet *s = w->data;
	struct ui_rect b = s->list_box, b2 = s->list2_box;
	if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
		s->scroll += dy > 0 ? 3 : -3;
	else if (x >= b2.x && x < b2.x + b2.w && y >= b2.y && y < b2.y + b2.h)
		s->scroll2 += dy > 0 ? 3 : -3;
	else
		return;
	w->hot = 0;
	ui_redraw(w);
}

static void sheet_key(struct ui_win *w, xkb_keysym_t sym, const char *utf8, int mods) {
	struct sheet *s = w->data;
	if (sym == XKB_KEY_Escape) {
		close_sheet(s, !is_live(s->kind));
		return;
	}
	if (s->kind == K_PROGRAMS && s->tab == 1) {
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			s->scroll = 0;
			pkg_search();
		} else {
			ui_edit(pkg_query, sizeof(pkg_query), &pkg_caret, sym, utf8, mods);
		}
		return;
	}
	if (s->kind == K_NETWORK && s->tab == 0 && wifi_sel >= 0) {
		if (sym == XKB_KEY_Return)
			wifi_connect();
		else
			ui_edit(wifi_password, sizeof(wifi_password), &wifi_pw_caret, sym, utf8, mods);
		return;
	}
	if (sym == XKB_KEY_Return && !is_live(s->kind) && s->kind != K_PROGRAMS) {
		if (memcmp(&cfg, &saved, sizeof(cfg)))
			config_save();
		close_sheet(s, false);
	}
}

static void open_applet(enum kind k) {
	struct sheet *s = &sheets[k];
	if (s->win && s->win->alive)
		return;
	*s = (struct sheet){.kind = k};
	switch (k) {
	case K_NETWORK: net_refresh(false); break;
	case K_BLUETOOTH: bt_refresh(); break;
	case K_SOUNDS: sound_refresh(); break;
	case K_POWER: power_refresh(); break;
	case K_SYSTEM: system_refresh(); break;
	case K_DISPLAY: screens_refresh(); break;
	case K_PROGRAMS: case K_DEFAULTS: progs_refresh(); defaults_refresh(); break;
	case K_DATETIME: zones_refresh(); break;
	default: break;
	}
	int ww = k == K_PROGRAMS ? 600 : SHEET_W, hh = SHEET_H;
	s->win = ui_window(applets[k].title, "expede-settings", ww, hh, false);
	s->win->data = s;
	s->win->paint = sheet_paint;
	s->win->button = sheet_button;
	s->win->motion = sheet_motion;
	s->win->scroll = sheet_scroll;
	s->win->key = sheet_key;
	s->win->close = sheet_close;
}

static void cp_button(struct ui_win *w, int id, int button, bool pressed, double x, double y, int clicks, int mods) {
	if (pressed || button != UI_LEFT || id != w->pressed)
		return;
	if (id == ID_VIEW_TOGGLE) {
		classic_view = !classic_view;
		cp_category = -1;
	} else if (id == ID_HOME) {
		cp_category = -1;
	} else if (id >= ID_CATEGORY && id < ID_CATEGORY + N_CATS) {
		cp_category = id - ID_CATEGORY;
		/* a category with one applet opens it straight away */
		if (!categories[cp_category].items[1])
			open_applet(categories[cp_category].items[0]);
	} else if (id >= ID_APPLET && id < ID_APPLET + K_COUNT) {
		open_applet(id - ID_APPLET);
	}
}

static void cp_key(struct ui_win *w, xkb_keysym_t sym, const char *utf8, int mods) {
	if (sym == XKB_KEY_BackSpace || (sym == XKB_KEY_Left && (mods & UI_ALT)))
		cp_category = -1;
}

int main(int argc, char **argv) {
	if (!ui_init("expede-settings"))
		return 1;
	config_load();
	theme_set_user_picture(&ui_theme, cfg.picture);

	char dir[700];
	wallpapers = calloc(1, sizeof(*wallpapers));
	wallpapers[0] = (struct choice){strdup(""), strdup("(None)"), NULL};
	n_wallpapers = 1;
	struct choice *found = NULL;
	int n_found = 0;
	snprintf(dir, sizeof(dir), "%s/wallpapers", ui_theme.dir);
	list_dir(dir, &found, &n_found, true);
	wallpapers = realloc(wallpapers, (1 + n_found) * sizeof(*wallpapers));
	memcpy(wallpapers + 1, found, n_found * sizeof(*found));
	n_wallpapers += n_found;
	free(found);
	snprintf(dir, sizeof(dir), "%s/pictures", ui_theme.dir);
	list_dir(dir, &pictures, &n_pictures, false);
	for (int i = 0; i < n_pictures; i++) {
		char rel[300];
		snprintf(rel, sizeof(rel), "pictures/%s", pictures[i].value);
		pictures[i].img = theme_png(&ui_theme, rel);
	}
	for (int c = 0; c < N_CATS; c++)
		cat_icons[c] = theme_icon(&ui_theme, categories[c].icon);

	enum kind start = K_CP;
	for (int k = 1; argc > 1 && k < K_COUNT; k++)
		if (applets[k].arg && !strcmp(argv[1], applets[k].arg))
			start = k;
	if (start == K_CP) {
		struct ui_win *w = ui_window("Control Panel", "expede-settings", 660, 470, false);
		w->paint = paint_cp;
		w->button = cp_button;
		w->key = cp_key;
	} else {
		open_applet(start);
	}
	return ui_run();
}
