// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal zero-dependency test harness for the SqlCore golden suite.
//
// Deliberately tiny (no GoogleTest/Catch2 / vcpkg dependency) so the P0 gate
// builds with nothing but MSVC. Tests self-register via the TEST() macro; main()
// (in test_harness.cpp) runs them and returns non-zero on any failure.
#pragma once

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace th {

inline int g_checks = 0;
inline int g_failures = 0;
inline std::string g_currentTest;

// Render a wide string as ASCII, escaping non-ASCII / control characters so test
// output is readable in a console.
inline std::string narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) {
        if (c == L'\n') {
            s += "\\n";
        } else if (c == L'\t') {
            s += "\\t";
        } else if (c == L'\r') {
            s += "\\r";
        } else if (c >= 32 && c < 127) {
            s.push_back(static_cast<char>(c));
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
            s += buf;
        }
    }
    return s;
}

using TestFn = void (*)();
struct TestCase {
    const char* name;
    TestFn fn;
};
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

}  // namespace th

#define TEST(name)                                            \
    static void name();                                       \
    static th::Registrar th_reg_##name(#name, name);          \
    static void name()

#define CHECK(cond)                                                            \
    do {                                                                       \
        th::g_checks++;                                                        \
        if (!(cond)) {                                                         \
            th::g_failures++;                                                  \
            std::cout << "  FAIL [" << th::g_currentTest << "] " << #cond      \
                      << "  (line " << __LINE__ << ")\n";                      \
        }                                                                      \
    } while (0)

// Requires a `dbg(...)` overload to be in scope for the operand types.
#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        th::g_checks++;                                                        \
        auto&& _chk_a = (a);                                                   \
        auto&& _chk_b = (b);                                                   \
        if (!(_chk_a == _chk_b)) {                                             \
            th::g_failures++;                                                  \
            std::cout << "  FAIL [" << th::g_currentTest << "] " << #a         \
                      << " == " << #b << "  (line " << __LINE__ << ")\n";      \
            std::cout << "         actual:   " << dbg(_chk_a) << "\n";         \
            std::cout << "         expected: " << dbg(_chk_b) << "\n";         \
        }                                                                      \
    } while (0)
