// guard.c — see guard.h. Original code (MIT).

#include "guard.h"

int guard_parse(const char *text, size_t len) {
    if (!text || len == 0 || text[0] < '0' || text[0] > '0' + GUARD_OFF) return GUARD_NORMAL;
    if (len > 1 && text[1] != '\n' && text[1] != '\0') return GUARD_NORMAL;   // "12", "1x": not ours
    return text[0] - '0';
}

int guard_after_start(int level, int marker_found) {
    if (level < GUARD_NORMAL) level = GUARD_NORMAL;
    if (level > GUARD_OFF) level = GUARD_OFF;
    if (marker_found && level < GUARD_OFF) level++;
    return level;
}

int guard_after_power_on(int level) { return level > GUARD_NET_PAUSED ? GUARD_NET_PAUSED : level; }

int guard_after_join(int level) {
    (void)level;
    return GUARD_NORMAL;
}
