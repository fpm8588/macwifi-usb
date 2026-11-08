// test_json.c — control-socket JSON and the known-network store. Original code (MIT).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "knownnet.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int parse(const char *s, JsonObj *o) { return json_parse_flat(s, strlen(s), o); }

static void test_parse(void) {
    JsonObj o;
    CHECK(parse("{\"cmd\":\"join\",\"ssid\":\"Caf\\u00e9 \\\"5G\\\"\",\"password\":\"p@ss\\\\w\",\"n\":-42.5,\"on\":true,\"x\":null}", &o) == 0);
    CHECK(o.n == 6);
    size_t l = 0;
    CHECK(!strcmp(json_str(&o, "cmd", NULL), "join"));
    const char *ss = json_str(&o, "ssid", &l);
    CHECK(ss && l == 10 && !memcmp(ss, "Caf\xc3\xa9 \"5G\"", 10));
    CHECK(!strcmp(json_str(&o, "password", NULL), "p@ss\\w"));
    CHECK(json_get(&o, "n")->type == JSON_NUMBER && json_get(&o, "n")->num == -42.5);
    CHECK(json_get(&o, "on")->type == JSON_BOOL && json_get(&o, "on")->num == 1);
    CHECK(json_get(&o, "x")->type == JSON_NULL && json_str(&o, "x", NULL) == NULL && !json_get(&o, "missing"));
    CHECK(parse("  { }  \n", &o) == 0 && o.n == 0);
    CHECK(parse("{\"s\":\"\\ud83d\\ude00\"}", &o) == 0 && !strcmp(json_str(&o, "s", NULL), "\xf0\x9f\x98\x80"));
    CHECK(parse("{\"s\":\"a\\u0000b\"}", &o) == 0 && json_get(&o, "s")->str_len == 3);   // embedded NUL kept
    // rejected
    const char *bad[] = {"", "[]", "{\"a\":{}}", "{\"a\":[1]}", "{\"a\":1,}", "{\"a\" 1}", "{\"a\":\"x}", "{\"a\":tru}",
                         "{\"a\":\"\\x\"}", "{\"a\":\"\\ud800\"}", "{\"a\":\"\\udc00\"}", "{\"a\":1}x", "{a:1}",
                         "{\"a\":\"\x01\"}", "{\"a\":1e}", "{\"\\u0000\":1}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = parse(bad[i], &o);
        if (r != -1) fprintf(stderr, "accepted: %s\n", bad[i]);
        CHECK(r == -1);
    }
    // too many keys / too long
    char big[4096] = "{";
    for (int i = 0; i < 17; i++) { char k[16]; snprintf(k, sizeof k, "%s\"k%d\":1", i ? "," : "", i); strcat(big, k); }
    strcat(big, "}");
    CHECK(parse(big, &o) == -1);
    char longs[3000];
    memset(longs, 'a', sizeof longs);
    memcpy(longs, "{\"a\":\"", 6);
    memcpy(longs + sizeof longs - 2, "\"}", 2);
    CHECK(json_parse_flat(longs, sizeof longs, &o) == -1);
    // wipe
    CHECK(parse("{\"password\":\"secret123\"}", &o) == 0);
    json_wipe(&o);
    CHECK(memmem(o.buf, sizeof o.buf, "secret", 6) == NULL);
}

static void test_out(void) {
    char b[512];
    JsonOut j;
    jo_init(&j, b, sizeof b);
    jo_begin_obj(&j);
    jo_bool(&j, "ok", 1);
    jo_str(&j, "state", "connected");
    jo_strn(&j, "ssid", (const uint8_t *)"a\"b\\c\n\x01\xff\xc3\xa9", 10);
    jo_int(&j, "rssi", -44);
    jo_key_arr(&j, "networks");
    jo_begin_obj(&j); jo_str(&j, "ssid", "x"); jo_end_obj(&j);
    jo_begin_obj(&j); jo_str(&j, "ssid", "y"); jo_end_obj(&j);
    jo_end_arr(&j);
    jo_key_obj(&j, "o"); jo_int(&j, "a", 1); jo_end_obj(&j);
    jo_end_obj(&j);
    size_t n = jo_finish(&j);
    const char *want = "{\"ok\":true,\"state\":\"connected\",\"ssid\":\"a\\\"b\\\\c\\u000a\\u0001\xef\xbf\xbd\xc3\xa9\","
                       "\"rssi\":-44,\"networks\":[{\"ssid\":\"x\"},{\"ssid\":\"y\"}],\"o\":{\"a\":1}}\n";
    CHECK(n == strlen(want) && !strcmp(b, want));
    if (strcmp(b, want)) fprintf(stderr, "got  %s", b);
    // invalid UTF-8 variants become U+FFFD
    jo_init(&j, b, sizeof b);
    jo_begin_obj(&j);
    jo_strn(&j, "s", (const uint8_t *)"\xf5\x80\x80\x80|\xc0\xaf|\xed\xa0\x80|\xe2\x82", 15);
    jo_end_obj(&j);
    CHECK(jo_finish(&j) && strstr(b, "\"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd|") != NULL);
    CHECK(strstr(b, "\xed\xa0\x80") == NULL && strstr(b, "\xe2\x82\"") == NULL);
    // overflow
    char tiny[16];
    jo_init(&j, tiny, sizeof tiny);
    jo_begin_obj(&j);
    jo_str(&j, "state", "connected-and-more");
    jo_end_obj(&j);
    CHECK(jo_finish(&j) == 0);
    // round trip of what we write through our parser
    jo_init(&j, b, sizeof b);
    jo_begin_obj(&j);
    jo_strn(&j, "ssid", (const uint8_t *)"Caf\xc3\xa9\t\"x\"", 10);
    jo_end_obj(&j);
    n = jo_finish(&j);
    JsonObj o;
    size_t l;
    CHECK(json_parse_flat(b, n, &o) == 0 && json_str(&o, "ssid", &l) && l == 10 && !memcmp(json_str(&o, "ssid", NULL), "Caf\xc3\xa9\t\"x\"", 10));
}

static void test_known(void) {
    KnownList k;
    uint8_t pmk[32];
    for (int i = 0; i < 32; i++) pmk[i] = (uint8_t)(i * 7);
    memset(&k, 0, sizeof k);
    CHECK(kn_put(&k, (const uint8_t *)"Home", 4, pmk) == 0);
    CHECK(kn_put(&k, (const uint8_t *)"Guest", 5, NULL) == 0);
    CHECK(kn_put(&k, (const uint8_t *)"", 0, NULL) == -1);
    CHECK(k.count == 2 && !memcmp(k.n[0].ssid, "Guest", 5));     // newest first
    char buf[1024];
    size_t n = kn_format(&k, buf, sizeof buf);
    CHECK(n > 0 && strstr(buf, "4775657374 - 1\n") && strstr(buf, "486f6d65 00070e15"));
    CHECK(kn_format(&k, buf, 20) == 0);
    n = kn_format(&k, buf, sizeof buf);
    KnownList r;
    CHECK(kn_parse(&r, buf, n) == 2);
    KnownNet *h = kn_find(&r, (const uint8_t *)"Home", 4);
    CHECK(h && h->has_pmk && !memcmp(h->pmk, pmk, 32) && h->autojoin);
    CHECK(kn_find(&r, (const uint8_t *)"Guest", 5) && !kn_find(&r, (const uint8_t *)"Guest", 5)->has_pmk);
    // replace keeps one entry, moves it first
    CHECK(kn_put(&r, (const uint8_t *)"Home", 4, NULL) == 0 && r.count == 2 && !r.n[0].has_pmk);
    CHECK(kn_remove(&r, (const uint8_t *)"Home", 4) == 1 && r.count == 1 && kn_remove(&r, (const uint8_t *)"Home", 4) == 0);
    // bad lines are skipped, duplicates ignored
    const char *t = "zz - 1\n486f6d65 1234 1\n486f6d65 - 2\n\n486f6d65 - 0\n486f6d65 - 1\n41 - 1";
    CHECK(kn_parse(&r, t, strlen(t)) == 2 && !r.n[0].autojoin && r.n[1].ssid_len == 1);
    // capacity: the oldest is dropped
    memset(&k, 0, sizeof k);
    for (int i = 0; i < KN_MAX + 3; i++) { uint8_t s[2] = {'n', (uint8_t)i}; kn_put(&k, s, 2, NULL); }
    CHECK(k.count == KN_MAX && k.n[0].ssid[1] == KN_MAX + 2 && !kn_find(&k, (const uint8_t *)"n\0", 2));
    kn_wipe(&k);
    CHECK(k.count == 0);

    // choosing
    memset(&k, 0, sizeof k);
    kn_put(&k, (const uint8_t *)"Home", 4, pmk);
    kn_put(&k, (const uint8_t *)"Cafe", 4, NULL);
    kn_put(&k, (const uint8_t *)"Off", 3, pmk);
    k.n[0].autojoin = 0;                                     // "Off"
    KnSeen seen[] = {
        {(const uint8_t *)"Other", 5, -30, 1, 1},
        {(const uint8_t *)"Home", 4, -70, 1, 1},
        {(const uint8_t *)"Cafe", 4, -50, 1, 0},
        {(const uint8_t *)"Off", 3, -20, 1, 1},
        {(const uint8_t *)"Home", 4, -60, 0, 1},             // a Home AP we can't use (e.g. PMF required)
    };
    CHECK(kn_choose(&k, seen, 5) == 2);                      // Cafe: strongest known+usable
    seen[2].secured = 1;                                     // Cafe now secured but stored as open: skip
    CHECK(kn_choose(&k, seen, 5) == 1);
    seen[1].usable = 0;
    CHECK(kn_choose(&k, seen, 5) == -1);
    CHECK(kn_choose(&k, seen, 0) == -1);

    // fuzz the parser
    srand(9);
    for (int it = 0; it < 20000; it++) {
        char f[300];
        size_t fl = (size_t)(rand() % 300);
        for (size_t i = 0; i < fl; i++) { const char a[] = "0123456789abcdefg -\n1"; f[i] = a[rand() % 21]; }
        kn_parse(&r, f, fl);
        CHECK(r.count <= KN_MAX);
    }
}

static void fuzz_json(void) {
    srand(4);
    JsonObj o;
    const char *seeds[] = {"{\"cmd\":\"join\",\"ssid\":\"x\\u00e9\",\"on\":true,\"n\":1.5}", "{\"a\":\"\\ud83d\\ude00\"}"};
    for (int it = 0; it < 100000; it++) {
        char b[128];
        const char *s = seeds[it & 1];
        size_t n = strlen(s);
        memcpy(b, s, n);
        int k = 1 + rand() % 4;
        for (int i = 0; i < k; i++) b[rand() % n] = (char)(rand() % 128);
        char *h = malloc(n);
        memcpy(h, b, n);
        if (json_parse_flat(h, (size_t)(rand() % (int)(n + 1)), &o) == 0)   // strings are always terminated
            for (int i = 0; i < o.n; i++) if (o.v[i].type == JSON_STRING && o.v[i].str[o.v[i].str_len] != 0) failures++;
        free(h);
    }
}

int main(void) {
    test_parse();
    test_out();
    test_known();
    fuzz_json();
    printf("json/known: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
