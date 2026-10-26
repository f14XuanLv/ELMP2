#pragma once

// Tiny assertion helpers for the few pure-logic CTests (§13). Each test executable returns the
// failure count.

#include <cstdio>

namespace elmp::test {
inline int g_failures = 0;
}

#define CHECK(cond)                                                                                     \
    do {                                                                                                \
        if (!(cond)) {                                                                                  \
            ++::elmp::test::g_failures;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                                               \
    } while (0)

#define TEST_MAIN(...)                                                                                  \
    int main() {                                                                                        \
        __VA_ARGS__;                                                                                    \
        if (::elmp::test::g_failures) std::fprintf(stderr, "%d failure(s)\n", ::elmp::test::g_failures); \
        return ::elmp::test::g_failures;                                                                \
    }
