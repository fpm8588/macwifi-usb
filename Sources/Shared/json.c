// json.c — see json.h. Original code (MIT).

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void skip_ws(const char *t, size_t n, size_t *i) {
    while (*i < n && (t[*i] == ' ' || t[*i] == '\t' || t[*i] == '\r' || t[*i] == '\n')) (*i)++;
}

static int hex4(const char *t, size_t n, size_t i, unsigned *out) {
    if (i + 4 > n) return -1;
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        char c = t[i + k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int put_utf8(char *dst, size_t cap, size_t *len, unsigned cp) {
    char b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); k = 4;
    }
    if (*len + k >= cap) return -1;
    memcpy(dst + *len, b, k);
    *len += k;
    return 0;
}

// Decodes a string starting after the opening quote into o->buf. Returns 0 and advances *i past the quote.
static int parse_string(const char *t, size_t n, size_t *i, JsonObj *o, char **out, size_t *out_len) {
    size_t start = o->used, len = start;
    char *dst = o->buf;
    size_t cap = sizeof o->buf;
    while (*i < n) {
        unsigned char c = (unsigned char)t[*i];
        if (c == '"') {
            (*i)++;
            if (len >= cap) return -1;
            dst[len] = 0;
            *out = dst + start;
            *out_len = len - start;
            o->used = len + 1;
            return 0;
        }
        if (c < 0x20) return -1;
        if (c != '\\') {
            if (len + 1 >= cap) return -1;
            dst[len++] = (char)c;
            (*i)++;
            continue;
        }
        if (++(*i) >= n) return -1;
        char e = t[(*i)++];
        unsigned cp;
        switch (e) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = 8; break;
            case 'f': cp = 12; break;
            case 'n': cp = 10; break;
            case 'r': cp = 13; break;
            case 't': cp = 9; break;
            case 'u':
                if (hex4(t, n, *i, &cp)) return -1;
                *i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {          // surrogate pair
                    unsigned lo;
                    if (*i + 6 > n || t[*i] != '\\' || t[*i + 1] != 'u' || hex4(t, n, *i + 2, &lo) || lo < 0xDC00 ||
                        lo > 0xDFFF)
                        return -1;
                    *i += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return -1;
                }
                break;
            default: return -1;
        }
        if (put_utf8(dst, cap, &len, cp)) return -1;
    }
    return -1;
}

int json_parse_flat(const char *t, size_t n, JsonObj *o) {
    memset(o, 0, sizeof *o);
    size_t i = 0;
    skip_ws(t, n, &i);
    if (i >= n || t[i] != '{') return -1;
    i++;
    skip_ws(t, n, &i);
    if (i < n && t[i] == '}') { i++; goto tail; }
    for (;;) {
        skip_ws(t, n, &i);
        if (i >= n || t[i] != '"' || o->n >= JSON_MAX_KEYS) return -1;
        i++;
        char *k;
        size_t kl;
        if (parse_string(t, n, &i, o, &k, &kl) || kl >= sizeof o->v[0].key || memchr(k, 0, kl)) return -1;
        JsonVal *v = &o->v[o->n];
        memcpy(v->key, k, kl + 1);
        o->used -= kl + 1;                                   // keys live in the slot, not the buffer
        skip_ws(t, n, &i);
        if (i >= n || t[i] != ':') return -1;
        i++;
        skip_ws(t, n, &i);
        if (i >= n) return -1;
        if (t[i] == '"') {
            i++;
            v->type = JSON_STRING;
            if (parse_string(t, n, &i, o, &v->str, &v->str_len)) return -1;
        } else if (n - i >= 4 && !strncmp(t + i, "true", 4)) {
            v->type = JSON_BOOL; v->num = 1; i += 4;
        } else if (n - i >= 5 && !strncmp(t + i, "false", 5)) {
            v->type = JSON_BOOL; v->num = 0; i += 5;
        } else if (n - i >= 4 && !strncmp(t + i, "null", 4)) {
            v->type = JSON_NULL; i += 4;
        } else if (t[i] == '-' || (t[i] >= '0' && t[i] <= '9')) {
            char num[40];
            size_t k = 0;
            while (i < n && k + 1 < sizeof num && strchr("+-0123456789.eE", t[i])) num[k++] = t[i++];
            num[k] = 0;
            char *end = NULL;
            v->num = strtod(num, &end);
            if (!end || *end) return -1;
            v->type = JSON_NUMBER;
        } else {
            return -1;                                       // objects, arrays, junk
        }
        o->n++;
        skip_ws(t, n, &i);
        if (i < n && t[i] == ',') { i++; continue; }
        if (i < n && t[i] == '}') { i++; break; }
        return -1;
    }
tail:
    skip_ws(t, n, &i);
    return i == n ? 0 : -1;
}

const JsonVal *json_get(const JsonObj *o, const char *key) {
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->v[i].key, key)) return &o->v[i];
    return NULL;
}

const char *json_str(const JsonObj *o, const char *key, size_t *len) {
    const JsonVal *v = json_get(o, key);
    if (!v || v->type != JSON_STRING) return NULL;
    if (len) *len = v->str_len;
    return v->str;
}

void json_wipe(JsonObj *o) {
    volatile char *p = o->buf;
    for (size_t i = 0; i < sizeof o->buf; i++) p[i] = 0;
}

// ---------------------------------------------------------------- output

static void put(JsonOut *j, const char *s, size_t n) {
    if (j->len + n + 2 > j->cap) { j->err = 1; return; }   // keep room for "\n\0"
    memcpy(j->p + j->len, s, n);
    j->len += n;
}
static void puts_(JsonOut *j, const char *s) { put(j, s, strlen(s)); }

static void comma(JsonOut *j) {
    if (j->need_comma[j->depth]) put(j, ",", 1);
    j->need_comma[j->depth] = 1;
}

static void escaped(JsonOut *j, const uint8_t *s, size_t n) {
    put(j, "\"", 1);
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') { char b[2] = {'\\', (char)c}; put(j, b, 2); i++; continue; }
        if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); puts_(j, b); i++; continue; }
        if (c < 0x80) { put(j, (const char *)&c, 1); i++; continue; }
        // validate one UTF-8 sequence; replace anything invalid with U+FFFD
        size_t k = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        int ok = k && i + k <= n;
        for (size_t m = 1; ok && m < k; m++) ok = (s[i + m] & 0xC0) == 0x80;
        if (ok && k == 3) {
            unsigned cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            ok = cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF);
        }
        if (ok && k == 4) {
            unsigned cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            ok = cp >= 0x10000 && cp <= 0x10FFFF;
        }
        if (ok) { put(j, (const char *)s + i, k); i += k; }
        else { put(j, "\xEF\xBF\xBD", 3); i++; }
    }
    put(j, "\"", 1);
}

static void key(JsonOut *j, const char *k) {
    comma(j);
    if (k) {
        escaped(j, (const uint8_t *)k, strlen(k));
        put(j, ":", 1);
    }
}

void jo_init(JsonOut *j, char *buf, size_t cap) {
    memset(j, 0, sizeof *j);
    j->p = buf;
    j->cap = cap;
}
static void open_(JsonOut *j, const char *k, const char *br) {
    key(j, k);
    puts_(j, br);
    if (j->depth + 1 < (int)(sizeof j->need_comma / sizeof j->need_comma[0])) j->depth++;
    else j->err = 1;
    j->need_comma[j->depth] = 0;
}
static void close_(JsonOut *j, const char *br) {
    puts_(j, br);
    if (j->depth > 0) j->depth--;
}
void jo_begin_obj(JsonOut *j) { open_(j, NULL, "{"); }
void jo_end_obj(JsonOut *j) { close_(j, "}"); }
void jo_key_obj(JsonOut *j, const char *k) { open_(j, k, "{"); }
void jo_key_arr(JsonOut *j, const char *k) { open_(j, k, "["); }
void jo_end_arr(JsonOut *j) { close_(j, "]"); }
void jo_str(JsonOut *j, const char *k, const char *s) {
    key(j, k);
    escaped(j, (const uint8_t *)(s ? s : ""), s ? strlen(s) : 0);
}
void jo_strn(JsonOut *j, const char *k, const uint8_t *s, size_t n) { key(j, k); escaped(j, s, n); }
void jo_int(JsonOut *j, const char *k, long long v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", v);
    key(j, k);
    puts_(j, b);
}
void jo_bool(JsonOut *j, const char *k, int v) { key(j, k); puts_(j, v ? "true" : "false"); }

size_t jo_finish(JsonOut *j) {
    if (j->err || j->len + 2 > j->cap) return 0;
    j->p[j->len++] = '\n';
    j->p[j->len] = 0;
    return j->len;
}
