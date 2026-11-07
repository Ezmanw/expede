#ifndef EXPEDE_VOLUME_H
#define EXPEDE_VOLUME_H

/* Master volume through wpctl, pactl or amixer, whichever is installed.
 * Shared by the compositor and expede-settings; both provide spawn(). */
#include <stdbool.h>

bool volume_read(int *pct, bool *muted);
void volume_write(int pct);
void volume_mute(bool muted);
void volume_step(int delta); /* relative, in percent */
bool volume_available(void);
/* the output device's name, e.g. "Ryzen HD Audio Controller Analog Stereo" */
bool volume_device_name(char *out, int len);

void spawn(const char *cmd);

#endif
