// power.h — system sleep/wake notifications for macwifid (macOS IOKit power management). Original code (MIT).
//
// macwifid takes no power assertions, so it never keeps the Mac awake. It only asks to be told:
//   - before sleep: `will_sleep` runs (on a private queue) and may block while the adapter is shut down cleanly;
//     sleep is allowed as soon as it returns (macOS waits at most 30 s for an answer);
//   - idle-sleep "may I?" questions are always answered yes;
//   - after wake: `did_wake` runs.

#ifndef MW_POWER_H
#define MW_POWER_H

int power_watch_start(void (*will_sleep)(void), void (*did_wake)(void));

#endif
