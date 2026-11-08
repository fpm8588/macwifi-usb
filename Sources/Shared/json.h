// json.h — the small JSON subset used on macwifid's control socket: one flat object per line, values are
// strings, numbers, booleans or null (requests); responses are written with the JsonOut helpers and may nest.
// Original code (MIT).

#ifndef MW_JSON_H
#define MW_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_MAX_KEYS 16

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING } JsonType;

typedef struct {
    char key[32];
    JsonType type;
    double num;          // JSON_NUMBER, JSON_BOOL (0/1)
    char *str;           // JSON_STRING: decoded UTF-8, NUL-terminated, inside JsonObj.buf
    size_t str_len;      // may contain NUL bytes
} JsonVal;

typedef struct {
    JsonVal v[JSON_MAX_KEYS];
    int n;
    char buf[2048];      // decoded strings
    size_t used;
} JsonObj;

// Parses one flat object. Returns 0, or -1 for anything else (nesting, arrays, bad escapes, too long).
int json_parse_flat(const char *text, size_t len, JsonObj *o);
const JsonVal *json_get(const JsonObj *o, const char *key);
// Convenience: string value of `key` (NULL if missing or not a string).
const char *json_str(const JsonObj *o, const char *key, size_t *len);
// Overwrites every decoded string (used after a request that carried a password).
void json_wipe(JsonObj *o);

// Output buffer. Writes are truncated at the capacity (overflow sets `err`).
typedef struct {
    char *p;
    size_t cap, len;
    int err;
    int need_comma[8];
    int depth;
} JsonOut;

void jo_init(JsonOut *j, char *buf, size_t cap);
void jo_begin_obj(JsonOut *j);              // as a value (top level or array element)
void jo_end_obj(JsonOut *j);
void jo_key_obj(JsonOut *j, const char *k); // "k":{
void jo_key_arr(JsonOut *j, const char *k); // "k":[
void jo_end_arr(JsonOut *j);
void jo_str(JsonOut *j, const char *k, const char *s);
void jo_strn(JsonOut *j, const char *k, const uint8_t *s, size_t n);   // bytes; invalid UTF-8 -> U+FFFD
void jo_int(JsonOut *j, const char *k, long long v);
void jo_bool(JsonOut *j, const char *k, int v);
// Finishes the line ("\n"); returns the length, or 0 if it overflowed.
size_t jo_finish(JsonOut *j);

#ifdef __cplusplus
}
#endif
#endif
