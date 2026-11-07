/* Master volume through whatever the system runs: PipeWire (wpctl), then
 * PulseAudio (pactl), then plain ALSA (amixer). Reads block briefly on the
 * helper's output; writes run in the background. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"

enum backend { B_UNKNOWN, B_WPCTL, B_PACTL, B_AMIXER, B_NONE };
static enum backend backend = B_UNKNOWN;

/* first line of a command's output; false if it failed or printed nothing */
static bool run(const char *cmd, char *out, size_t len) {
	FILE *p = popen(cmd, "r");
	if (!p)
		return false;
	out[0] = 0;
	size_t n = fread(out, 1, len - 1, p);
	out[n] = 0;
	return pclose(p) == 0 && n > 0;
}

static bool read_wpctl(int *pct, bool *muted) {
	char buf[256];
	/* "Volume: 0.40" or "Volume: 0.40 [MUTED]" */
	if (!run("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof(buf)))
		return false;
	const char *v = strstr(buf, "Volume:");
	if (!v)
		return false;
	*pct = (int)(strtod(v + 7, NULL) * 100 + 0.5);
	*muted = strstr(buf, "MUTED") != NULL;
	return true;
}

static bool read_pactl(int *pct, bool *muted) {
	char buf[512];
	if (!run("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", buf, sizeof(buf)))
		return false;
	const char *pc = strchr(buf, '%');
	if (!pc)
		return false;
	while (pc > buf && pc[-1] >= '0' && pc[-1] <= '9')
		pc--;
	*pct = atoi(pc);
	*muted = run("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", buf, sizeof(buf)) && strstr(buf, "yes");
	return true;
}

static bool read_amixer(int *pct, bool *muted) {
	char buf[1024];
	/* "... Playback 46 [72%] [on]" */
	if (!run("amixer -M get Master 2>/dev/null", buf, sizeof(buf)))
		return false;
	const char *b = strchr(buf, '[');
	if (!b)
		return false;
	*pct = atoi(b + 1);
	*muted = strstr(buf, "[off]") != NULL;
	return true;
}

/* Pick the tool by what is installed, not by whether it answered: at login
 * PipeWire may still be starting, and a failed first read must not leave us
 * stuck on a worse fallback for the whole session. */
static void detect(void) {
	char buf[256];
	if (run("command -v wpctl", buf, sizeof(buf)))
		backend = B_WPCTL;
	else if (run("command -v pactl", buf, sizeof(buf)))
		backend = B_PACTL;
	else if (run("command -v amixer", buf, sizeof(buf)))
		backend = B_AMIXER;
	else
		backend = B_NONE;
}

bool volume_read(int *pct, bool *muted) {
	if (backend == B_UNKNOWN)
		detect();
	int p = *pct;
	bool m = *muted;
	bool ok = false;
	switch (backend) {
	case B_WPCTL: ok = read_wpctl(&p, &m); break;
	case B_PACTL: ok = read_pactl(&p, &m); break;
	case B_AMIXER: ok = read_amixer(&p, &m); break;
	default: break;
	}
	if (ok) {
		*pct = p;
		*muted = m;
	}
	return ok; /* on failure the caller keeps its last known values */
}

void volume_write(int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	char cmd[128];
	switch (backend) {
	case B_WPCTL: snprintf(cmd, sizeof(cmd), "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%", pct); break;
	case B_PACTL: snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %d%%", pct); break;
	case B_AMIXER: snprintf(cmd, sizeof(cmd), "amixer -q -M set Master %d%%", pct); break;
	default: return;
	}
	spawn(cmd);
}

/* relative change, capped at 100%; runs synchronously so a read straight after sees it */
void volume_step(int delta) {
	char cmd[160], buf[64];
	int d = delta < 0 ? -delta : delta;
	char sign = delta < 0 ? '-' : '+';
	switch (backend) {
	case B_WPCTL: snprintf(cmd, sizeof(cmd), "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ %d%%%c", d, sign); break;
	case B_PACTL: snprintf(cmd, sizeof(cmd), "pactl set-sink-volume @DEFAULT_SINK@ %c%d%%", sign, d); break;
	case B_AMIXER: snprintf(cmd, sizeof(cmd), "amixer -q -M set Master %d%%%c", d, sign); break;
	default: return;
	}
	strcat(cmd, " >/dev/null 2>&1; echo");
	run(cmd, buf, sizeof(buf));
	if (backend == B_PACTL) {
		/* pactl has no cap of its own */
		int p;
		bool m;
		if (read_pactl(&p, &m) && p > 100)
			volume_write(100);
	}
}

void volume_mute(bool muted) {
	const char *cmd;
	switch (backend) {
	case B_WPCTL: cmd = muted ? "wpctl set-mute @DEFAULT_AUDIO_SINK@ 1" : "wpctl set-mute @DEFAULT_AUDIO_SINK@ 0"; break;
	case B_PACTL: cmd = muted ? "pactl set-sink-mute @DEFAULT_SINK@ 1" : "pactl set-sink-mute @DEFAULT_SINK@ 0"; break;
	case B_AMIXER: cmd = muted ? "amixer -q set Master mute" : "amixer -q set Master unmute"; break;
	default: return;
	}
	spawn(cmd);
}

bool volume_available(void) {
	if (backend == B_UNKNOWN)
		detect();
	return backend != B_NONE;
}

bool volume_device_name(char *out, int len) {
	if (backend == B_UNKNOWN)
		detect();
	char buf[1024];
	if (backend == B_WPCTL && run("wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null", buf, sizeof(buf))) {
		char *d = strstr(buf, "node.description = \"");
		if (d) {
			d += 20;
			char *q = strchr(d, '"');
			if (q)
				*q = 0;
			snprintf(out, len, "%s", d);
			return true;
		}
	}
	snprintf(out, len, "%s", backend == B_NONE ? "No audio device" : "Default sound device");
	return backend != B_NONE;
}
