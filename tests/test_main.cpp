#include "test_main.hpp"

#include <cstring>

namespace tt {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

int& failures() {
    static int f = 0;
    return f;
}

void report_failure(const char* file, int line, const std::string& what) {
    ++failures();
    std::printf("    FAIL %s:%d %s\n", file, line, what.c_str());
}

}  // namespace tt

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;
    for (const auto& t : tt::registry()) {
        if (filter != nullptr && std::strstr(t.name, filter) == nullptr) continue;
        const int before = tt::failures();
        t.fn();
        ++run;
        if (tt::failures() != before) {
            ++failed;
            std::printf("[FAIL] %s\n", t.name);
        } else {
            std::printf("[ ok ] %s\n", t.name);
        }
    }
    std::printf("\n%d tests, %d failed\n", run, failed);
    return failed == 0 ? 0 : 1;
}
