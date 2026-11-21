// Minimal in-repo test harness (no external dependency; see PROGRESS.md decision 2).
#pragma once
#include <sstream>
#include <string>
#include <vector>

namespace cftest {

struct Case {
    const char* name;
    void (*fn)();
};
std::vector<Case>& registry();

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct Failure {
    std::string message;
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

// Free-form line printed with the test's result (measurements worth keeping in the log).
void note(const std::string& message);

}  // namespace cftest

#define CF_TEST(name)                                          \
    static void name();                                        \
    static const cftest::Registrar cf_registrar_##name(#name, &name); \
    static void name()

#define CF_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) cftest::fail(__FILE__, __LINE__, "CHECK failed: " #cond);  \
    } while (0)

#define CF_CHECK_MSG(cond, msg)                                                 \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::ostringstream cf_os;                                           \
            cf_os << "CHECK failed: " #cond << " -- " << msg;                   \
            cftest::fail(__FILE__, __LINE__, cf_os.str());                      \
        }                                                                       \
    } while (0)

#define CF_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto cf_a = (a);                                                  \
        const auto cf_b = (b);                                                  \
        if (!(cf_a == cf_b)) {                                                  \
            std::ostringstream cf_os;                                           \
            cf_os << "CHECK_EQ failed: " #a " == " #b " (" << cf_a << " vs " << cf_b << ")"; \
            cftest::fail(__FILE__, __LINE__, cf_os.str());                      \
        }                                                                       \
    } while (0)
