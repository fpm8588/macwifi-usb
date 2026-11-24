// guard.h — crash guard (original code, MIT). The service leaves a marker file while it runs and removes it when it
// stops cleanly. A start that finds the marker still there follows a crash, a power loss or a kernel panic, so it starts
// with less switched on, and each further unclean start goes one level lower. Nothing returns to normal by itself:
// the person's own action (joining a network, switching Wi-Fi on) does.

#ifndef GUARD_H
#define GUARD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUARD_NORMAL 0       // everything on
#define GUARD_NET_PAUSED 1   // no automatic joining: the radio and Bluetooth run, nothing is bridged to macOS
#define GUARD_OFF 2          // the radio is off too (so Bluetooth as well) until the person switches Wi-Fi on

// The level stored in the file's text; 0 for nothing, garbage or an out-of-range number.
int guard_parse(const char *text, size_t len);
// The level for this run, given the stored level and whether the marker was still there.
int guard_after_start(int level, int marker_found);
// The person switched Wi-Fi on / joined a network by hand.
int guard_after_power_on(int level);
int guard_after_join(int level);

#ifdef __cplusplus
}
#endif
#endif
