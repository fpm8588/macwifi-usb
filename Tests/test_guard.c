// test_guard.c — the crash guard's levels. Original code (MIT).
#include <stdio.h>
#include <string.h>

#include "guard.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void) {
    CHECK(guard_parse(NULL, 0) == 0 && guard_parse("", 0) == 0);
    CHECK(guard_parse("0\n", 2) == 0 && guard_parse("1\n", 2) == 1 && guard_parse("2\n", 2) == 2 && guard_parse("2", 1) == 2);
    CHECK(guard_parse("3\n", 2) == 0 && guard_parse("x\n", 2) == 0 && guard_parse("12\n", 3) == 0 && guard_parse("1x", 2) == 0);

    // a clean start keeps the stored level; each unclean start goes one lower, to the floor
    CHECK(guard_after_start(0, 0) == 0 && guard_after_start(1, 0) == 1 && guard_after_start(2, 0) == 2);
    CHECK(guard_after_start(0, 1) == 1 && guard_after_start(1, 1) == 2 && guard_after_start(2, 1) == 2);
    CHECK(guard_after_start(-4, 0) == 0 && guard_after_start(9, 0) == 2);

    // the person's own actions lift it
    CHECK(guard_after_power_on(2) == 1 && guard_after_power_on(1) == 1 && guard_after_power_on(0) == 0);
    CHECK(guard_after_join(1) == 0 && guard_after_join(2) == 0);

    printf("guard: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
