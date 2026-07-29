// Minimal dependency-free test harness.
//
// The project ships with no third-party runtime deps and cross-compiles against
// musl / the OpenWrt SDK, so pulling in gtest or Catch2 via a meson subproject
// would add a configure-time network fetch to every build. This header is the
// whole framework instead.
//
// Usage:
//     #include "test_harness.h"
//     int main() {
//         SUITE("url_rewriter");
//         CHECK(cond);
//         CHECK_EQ(actual, expected);
//         XCHECK_EQ(actual, expected, "known bug: ...");  // reported, does not fail
//         return tst::summary();
//     }
#pragma once

#include <cstdio>
#include <sstream>
#include <string>

namespace tst
{

inline int passed = 0;
inline int failed = 0;
inline int xfailed = 0;
inline int xpassed = 0;

inline void suite(const char *name)
{
    std::printf("=== %s ===\n", name);
}

template <typename T>
inline std::string show(const T &v)
{
    std::ostringstream os;
    os << v;
    return os.str();
}

inline std::string show(bool v) { return v ? "true" : "false"; }
inline std::string show(const std::string &v) { return "\"" + v + "\""; }
inline std::string show(const char *v) { return std::string("\"") + v + "\""; }

inline void report_pass(const char *expr)
{
    ++passed;
    std::printf("  [ PASS ] %s\n", expr);
}

inline void report_fail(const char *expr, const char *file, int line,
                        const std::string &detail)
{
    ++failed;
    std::printf("  [ FAIL ] %s\n           at %s:%d\n", expr, file, line);
    if (!detail.empty())
        std::printf("%s\n", detail.c_str());
}

// A check that documents a bug which is known to be present and is not being
// fixed in this change. It never fails the run, but an unexpected pass is
// reported loudly so the marker gets removed once the bug is actually fixed.
inline void report_xfail(const char *expr, const char *reason)
{
    ++xfailed;
    std::printf("  [ XFAIL] %s\n           known bug: %s\n", expr, reason);
}

inline void report_xpass(const char *expr, const char *reason)
{
    ++xpassed;
    std::printf("  [ XPASS] %s\n           expected to fail but passed -- drop the XCHECK: %s\n",
                expr, reason);
}

inline int summary()
{
    std::printf("\n---- %d passed, %d failed, %d known-bug, %d unexpectedly-fixed ----\n",
                passed, failed, xfailed, xpassed);
    if (failed == 0)
        std::printf("RESULT: OK\n");
    else
        std::printf("RESULT: FAILED\n");
    return failed == 0 ? 0 : 1;
}

} // namespace tst

#define SUITE(name) tst::suite(name)

#define CHECK(cond)                                                            \
    do                                                                         \
    {                                                                          \
        if (cond)                                                              \
            tst::report_pass(#cond);                                           \
        else                                                                   \
            tst::report_fail(#cond, __FILE__, __LINE__, "");                   \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do                                                                         \
    {                                                                          \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (_a == _e)                                                          \
            tst::report_pass(#actual " == " #expected);                        \
        else                                                                   \
            tst::report_fail(#actual " == " #expected, __FILE__, __LINE__,     \
                             "           expected: " + tst::show(_e) +         \
                                 "\n           actual:   " + tst::show(_a));   \
    } while (0)

#define XCHECK(cond, reason)                                                   \
    do                                                                         \
    {                                                                          \
        if (cond)                                                              \
            tst::report_xpass(#cond, reason);                                  \
        else                                                                   \
            tst::report_xfail(#cond, reason);                                  \
    } while (0)

#define XCHECK_EQ(actual, expected, reason)                                    \
    do                                                                         \
    {                                                                          \
        auto _a = (actual);                                                    \
        auto _e = (expected);                                                  \
        if (_a == _e)                                                          \
            tst::report_xpass(#actual " == " #expected, reason);               \
        else                                                                   \
            tst::report_xfail(#actual " == " #expected, reason);               \
    } while (0)
