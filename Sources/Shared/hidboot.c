// hidboot.c — see hidboot.h. Original code (GPL-2.0).

#include "hidboot.h"

#include <string.h>

static int has(const uint8_t *keys, uint8_t k) {
    for (int i = 0; i < 6; i++) if (keys[i] == k) return 1;
    return 0;
}

int hid_kb_diff(HidKbState *st, const uint8_t *rep, size_t n, HidKeyEvent *out) {
    if (n < 8) return -1;
    const uint8_t mods = rep[0];
    const uint8_t *keys = rep + 2;
    int roll = 1;
    for (int i = 0; i < 6; i++) if (keys[i] != 0x01) roll = 0;
    if (roll) return -1;                                 // ErrorRollOver: too many keys; keep the old state
    int k = 0;
    for (int b = 0; b < 8; b++)                          // modifier releases
        if ((st->mods & (1u << b)) && !(mods & (1u << b))) { out[k].usage = (uint8_t)(0xE0 + b); out[k++].down = 0; }
    for (int i = 0; i < 6; i++)                          // key releases
        if (st->keys[i] > 0x03 && !has(keys, st->keys[i])) { out[k].usage = st->keys[i]; out[k++].down = 0; }
    for (int b = 0; b < 8; b++)                          // modifier presses
        if (!(st->mods & (1u << b)) && (mods & (1u << b))) { out[k].usage = (uint8_t)(0xE0 + b); out[k++].down = 1; }
    for (int i = 0; i < 6; i++)                          // key presses (0 none, 1-3 error codes)
        if (keys[i] > 0x03 && !has(st->keys, keys[i])) {
            int dup = 0;
            for (int j = 0; j < i; j++) if (keys[j] == keys[i]) dup = 1;
            if (!dup) { out[k].usage = keys[i]; out[k++].down = 1; }
        }
    st->mods = mods;
    memcpy(st->keys, keys, 6);
    return k;
}

int hid_mouse_parse(const uint8_t *rep, size_t n, HidMouse *m) {
    if (n < 3) return -1;
    m->buttons = rep[0];
    m->dx = (int8_t)rep[1];
    m->dy = (int8_t)rep[2];
    m->wheel = n >= 4 ? (int8_t)rep[3] : 0;
    return 0;
}

// Keyboard page usage -> kVK_* + 1 (HIToolbox Events.h); 0 = none.
static const uint8_t kMap[0x68] = {
    [0x04] = 0x00 + 1, [0x05] = 0x0B + 1, [0x06] = 0x08 + 1, [0x07] = 0x02 + 1, [0x08] = 0x0E + 1, [0x09] = 0x03 + 1, [0x0A] = 0x05 + 1,
    [0x0B] = 0x04 + 1, [0x0C] = 0x22 + 1, [0x0D] = 0x26 + 1, [0x0E] = 0x28 + 1, [0x0F] = 0x25 + 1, [0x10] = 0x2E + 1, [0x11] = 0x2D + 1,
    [0x12] = 0x1F + 1, [0x13] = 0x23 + 1, [0x14] = 0x0C + 1, [0x15] = 0x0F + 1, [0x16] = 0x01 + 1, [0x17] = 0x11 + 1, [0x18] = 0x20 + 1,
    [0x19] = 0x09 + 1, [0x1A] = 0x0D + 1, [0x1B] = 0x07 + 1, [0x1C] = 0x10 + 1, [0x1D] = 0x06 + 1,                 // a-z
    [0x1E] = 0x12 + 1, [0x1F] = 0x13 + 1, [0x20] = 0x14 + 1, [0x21] = 0x15 + 1, [0x22] = 0x17 + 1, [0x23] = 0x16 + 1, [0x24] = 0x1A + 1,
    [0x25] = 0x1C + 1, [0x26] = 0x19 + 1, [0x27] = 0x1D + 1,                                                 // 1-9, 0
    [0x28] = 0x24 + 1, [0x29] = 0x35 + 1, [0x2A] = 0x33 + 1, [0x2B] = 0x30 + 1, [0x2C] = 0x31 + 1,                  // Return Esc Delete Tab Space
    [0x2D] = 0x1B + 1, [0x2E] = 0x18 + 1, [0x2F] = 0x21 + 1, [0x30] = 0x1E + 1, [0x31] = 0x2A + 1, [0x32] = 0x2A + 1,   // - = [ ] \ #
    [0x33] = 0x29 + 1, [0x34] = 0x27 + 1, [0x35] = 0x32 + 1, [0x36] = 0x2B + 1, [0x37] = 0x2F + 1, [0x38] = 0x2C + 1,   // ; ' ` , . /
    [0x39] = 0x39 + 1,                                                                               // Caps Lock
    [0x3A] = 0x7A + 1, [0x3B] = 0x78 + 1, [0x3C] = 0x63 + 1, [0x3D] = 0x76 + 1, [0x3E] = 0x60 + 1, [0x3F] = 0x61 + 1,   // F1-F6
    [0x40] = 0x62 + 1, [0x41] = 0x64 + 1, [0x42] = 0x65 + 1, [0x43] = 0x6D + 1, [0x44] = 0x67 + 1, [0x45] = 0x6F + 1,   // F7-F12
    [0x46] = 0x69 + 1, [0x47] = 0x6B + 1, [0x48] = 0x71 + 1,                       // Print Screen, Scroll Lock, Pause -> F13-F15
    [0x49] = 0x72 + 1, [0x4A] = 0x73 + 1, [0x4B] = 0x74 + 1, [0x4C] = 0x75 + 1, [0x4D] = 0x77 + 1, [0x4E] = 0x79 + 1,   // Help/Ins Home PgUp Del End PgDn
    [0x4F] = 0x7C + 1, [0x50] = 0x7B + 1, [0x51] = 0x7D + 1, [0x52] = 0x7E + 1,                                 // arrows R L D U
    [0x53] = 0x47 + 1, [0x54] = 0x4B + 1, [0x55] = 0x43 + 1, [0x56] = 0x4E + 1, [0x57] = 0x45 + 1, [0x58] = 0x4C + 1,   // keypad Clear / * - + Enter
    [0x59] = 0x53 + 1, [0x5A] = 0x54 + 1, [0x5B] = 0x55 + 1, [0x5C] = 0x56 + 1, [0x5D] = 0x57 + 1, [0x5E] = 0x58 + 1,   // keypad 1-6
    [0x5F] = 0x59 + 1, [0x60] = 0x5B + 1, [0x61] = 0x5C + 1, [0x62] = 0x52 + 1, [0x63] = 0x41 + 1,                  // keypad 7-9 0 .
    [0x64] = 0x0A + 1, [0x67] = 0x51 + 1,                                                                // ISO section, keypad =
};

int hid_usage_to_mac(uint8_t usage) {
    if (usage < sizeof kMap) return kMap[usage] ? kMap[usage] - 1 : -1;
    switch (usage) {
    case 0x68: return 0x69; case 0x69: return 0x6B; case 0x6A: return 0x71; case 0x6B: return 0x6A;   // F13-F16
    case 0x6C: return 0x40; case 0x6D: return 0x4F; case 0x6E: return 0x50; case 0x6F: return 0x5A;   // F17-F20
    case 0x7F: return 0x4A; case 0x80: return 0x48; case 0x81: return 0x49;                           // mute, vol +/-
    case 0xE0: return 0x3B; case 0xE1: return 0x38; case 0xE2: return 0x3A; case 0xE3: return 0x37;   // left mods
    case 0xE4: return 0x3E; case 0xE5: return 0x3C; case 0xE6: return 0x3D; case 0xE7: return 0x36;   // right mods
    default: return -1;
    }
}
