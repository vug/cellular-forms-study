#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>

#include "test.hpp"

namespace cftest {

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void fail(const char* file, int line, const std::string& message) {
    std::ostringstream os;
    os << file << ":" << line << ": " << message;
    throw Failure{os.str()};
}

namespace {
std::vector<std::string>& notes() {
    static std::vector<std::string> n;
    return n;
}
}  // namespace

void note(const std::string& message) { notes().push_back(message); }

}  // namespace cftest

// Usage: cf_tests [substring]   runs the tests whose name contains the substring (all by default).
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : "";
    int run = 0, failed = 0;
    for (const cftest::Case& c : cftest::registry()) {
        if (std::strstr(c.name, filter) == nullptr) continue;
        ++run;
        cftest::notes().clear();
        const auto t0 = std::chrono::steady_clock::now();
        std::string error;
        try {
            c.fn();
        } catch (const cftest::Failure& f) {
            error = f.message;
        } catch (const std::exception& e) {
            error = std::string("unexpected exception: ") + e.what();
        } catch (...) {
            error = "unexpected non-standard exception";
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[%s] %s (%.0f ms)\n", error.empty() ? " ok " : "FAIL", c.name, ms);
        for (const std::string& n : cftest::notes()) std::printf("         %s\n", n.c_str());
        if (!error.empty()) {
            std::printf("         %s\n", error.c_str());
            ++failed;
        }
        std::fflush(stdout);
    }
    std::printf("%d tests run, %d failed\n", run, failed);
    return failed == 0 && run > 0 ? 0 : 1;
}
