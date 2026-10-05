#include <cstring>
#include <exception>
#include <string>

#include "test_framework.h"

// Usage: sculpt_tests [substring]  — runs the tests whose name contains it.
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failedTests = 0;
    for (const sctest::TestCase& test : sctest::registry()) {
        if (filter && !std::strstr(test.name, filter)) continue;
        ++run;
        const int before = sctest::failureCount();
        std::printf("[ RUN  ] %s\n", test.name);
        try {
            test.fn();
        } catch (const sctest::AbortTest&) {
            // Already reported by REQUIRE.
        } catch (const std::exception& e) {
            sctest::fail(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            sctest::fail(__FILE__, __LINE__, "unexpected non-standard exception");
        }
        const bool ok = sctest::failureCount() == before;
        if (!ok) ++failedTests;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", test.name);
    }
    std::printf("\n%d test(s) run, %d failed, %d failed check(s).\n", run, failedTests, sctest::failureCount());
    if (run == 0) {
        std::printf("No tests matched the filter.\n");
        return 2;
    }
    return failedTests == 0 ? 0 : 1;
}
