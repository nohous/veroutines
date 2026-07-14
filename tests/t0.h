/**
 * @brief Tier-0 test support: check macro, pass banner, println shim.
 */

#pragma once

#include <veroutines/veroutines.h>

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <version>

#if defined(__cpp_lib_print)
#include <print>
using std::println;
#else
#include <format>
template<typename... A>
void println(std::format_string<A...> f, A&&... a) {
    auto s = std::format(f, std::forward<A>(a)...);
    s.push_back('\n');
    std::fputs(s.c_str(), stdout);
}
#endif

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                         \
                         __FILE__, __LINE__, #cond);                         \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

inline int t0_pass(const char* name) {
    std::printf("PASS %s\n", name);
    return 0;
}
