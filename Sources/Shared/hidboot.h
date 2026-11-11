// hidboot.h — HID boot-protocol keyboard and mouse reports turned into key and pointer events, and USB HID key
// usages mapped to macOS virtual key codes. Original code (GPL-2.0). Sources: Device Class Definition for HID 1.11
// Appendix B (boot reports), HID Usage Tables 1.4 chapter 10 (keyboard page), and the kVK_* constants of macOS's
// HIToolbox Events.h (key codes are hardware positions on an ANSI layout; the user's input source maps them).

#ifndef HIDBOOT_H
#define HIDBOOT_H

#include <stddef.h>
#include <stdint.h>

#define HID_REPORT_KEYBOARD 0x01   // boot protocol report IDs on Bluetooth (HID Profile 3.3.2)
#define HID_REPORT_MOUSE    0x02

typedef struct {
    uint8_t mods;          // bit 0 LCtrl, 1 LShift, 2 LAlt, 3 LGUI, 4 RCtrl, 5 RShift, 6 RAlt, 7 RGUI
    uint8_t keys[6];
} HidKbState;

typedef struct {
    uint8_t usage;         // keyboard page usage (0xE0-0xE7 for modifiers)
    uint8_t down;
} HidKeyEvent;

// Compares a boot keyboard report (8 bytes after the report ID: mods, reserved, 6 key usages) with the previous
// state and lists what changed: modifier releases, key releases, modifier presses, key presses, in that order.
// Returns the number of events (at most 22), or -1 for a report that isn't one (wrong length, or the "rollover"
// error report with every slot 0x01: state is kept). Updates st.
int hid_kb_diff(HidKbState *st, const uint8_t *rep, size_t n, HidKeyEvent *out);

// A boot mouse report (after the report ID): buttons, dx, dy[, wheel]. 0, or -1 if too short.
typedef struct { uint8_t buttons; int8_t dx, dy, wheel; } HidMouse;
int hid_mouse_parse(const uint8_t *rep, size_t n, HidMouse *m);

// macOS virtual key code for a keyboard-page usage, or -1 if there's none.
int hid_usage_to_mac(uint8_t usage);

#endif
