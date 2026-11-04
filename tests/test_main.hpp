#pragma once
// A tiny test framework so the project has no dependencies.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tt {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();
int& failures();
void report_failure(const char* file, int line, const std::string& what);

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace tt

#define TEST(name)                                         \
    static void test_##name();                             \
    static tt::Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) tt::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        const auto va_ = (a);                                                                 \
        const auto vb_ = (b);                                                                 \
        if (!(va_ == vb_)) {                                                                  \
            tt::report_failure(__FILE__, __LINE__,                                            \
                               std::string("CHECK_EQ(" #a ", " #b ") got ") + std::to_string(va_) + " vs " + \
                                   std::to_string(vb_));                                      \
        }                                                                                     \
    } while (0)
