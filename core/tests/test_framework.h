// Minimal self-registering test framework (no third-party dependency, so the
// suite builds anywhere the core builds).
#pragma once

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace sctest {

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct AbortTest {};

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline void fail(const char* file, int line, const std::string& message) {
    ++failureCount();
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, message.c_str());
}

template <class A, class B>
std::string describe(const char* expr, const A& a, const B& b) {
    std::ostringstream os;
    os << expr << "  [" << a << " vs " << b << "]";
    return os.str();
}

}  // namespace sctest

#define SC_CONCAT_INNER(a, b) a##b
#define SC_CONCAT(a, b) SC_CONCAT_INNER(a, b)

#define TEST_CASE(name)                                                              \
    static void name();                                                              \
    static const ::sctest::Registrar SC_CONCAT(name, _registrar)(#name, &name);      \
    static void name()

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) ::sctest::fail(__FILE__, __LINE__, #cond);                      \
    } while (0)

#define REQUIRE(cond)                                                                \
    do {                                                                             \
        if (!(cond)) {                                                               \
            ::sctest::fail(__FILE__, __LINE__, #cond);                               \
            throw ::sctest::AbortTest{};                                             \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto& sc_a = (a);                                                      \
        const auto& sc_b = (b);                                                      \
        if (!(sc_a == sc_b)) ::sctest::fail(__FILE__, __LINE__, ::sctest::describe(#a " == " #b, sc_a, sc_b)); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                        \
    do {                                                                             \
        const double sc_a = static_cast<double>(a);                                  \
        const double sc_b = static_cast<double>(b);                                  \
        if (!(std::fabs(sc_a - sc_b) <= static_cast<double>(eps)))                   \
            ::sctest::fail(__FILE__, __LINE__, ::sctest::describe(#a " ~= " #b, sc_a, sc_b)); \
    } while (0)
