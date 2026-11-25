#include "start.hpp"

#include <climits>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "sim_cpu.hpp"
#include "topology.hpp"

namespace cf {

const char* const kStartShapeHelp =
    "icosphere:<level>, ellipsoid:<level>:<ax>:<ay>:<az> or noisy:<level>:<amplitude>:<frequency>";

bool parse_start_food(std::string_view text, StartFood& food) {
    if (text == "random") food = StartFood::Random;
    else if (text == "zero") food = StartFood::Zero;
    else return false;
    return true;
}

const char* start_food_name(StartFood food) { return food == StartFood::Zero ? "zero" : "random"; }

namespace {

[[noreturn]] void bad(std::string_view shape, const std::string& why) {
    throw std::invalid_argument("start shape '" + std::string(shape) + "': " + why + " (expected " + kStartShapeHelp + ")");
}

int level_of(std::string_view shape, const std::string& v) {
    char* end = nullptr;
    const unsigned long long x = std::strtoull(v.c_str(), &end, 10);
    if (v.empty() || *end != '\0' || v[0] == '-' || x > static_cast<unsigned long long>(INT_MAX)) bad(shape, "bad level '" + v + "'");
    return static_cast<int>(x);
}

float number_of(std::string_view shape, const std::string& v) {
    char* end = nullptr;
    const double x = std::strtod(v.c_str(), &end);
    if (v.empty() || *end != '\0' || !std::isfinite(x)) bad(shape, "bad number '" + v + "'");
    return static_cast<float>(x);
}

}  // namespace

State make_start_state(std::string_view shape, std::uint64_t seed, const Params& params, StartFood food) {
    std::vector<std::string> spec;
    std::size_t at = 0;
    for (;;) {
        const std::size_t colon = shape.find(':', at);
        spec.emplace_back(shape.substr(at, colon == std::string_view::npos ? std::string_view::npos : colon - at));
        if (colon == std::string_view::npos) break;
        at = colon + 1;
    }
    State s;
    if (spec.size() == 2 && spec[0] == "icosphere") {
        s = make_icosphere(level_of(shape, spec[1]), params);
    } else if (spec.size() == 5 && spec[0] == "ellipsoid") {
        const int level = level_of(shape, spec[1]);
        s = make_ellipsoid(level, params, number_of(shape, spec[2]), number_of(shape, spec[3]), number_of(shape, spec[4]));
    } else if (spec.size() == 4 && spec[0] == "noisy") {
        const int level = level_of(shape, spec[1]);
        s = make_noisy_sphere(level, params, number_of(shape, spec[2]), number_of(shape, spec[3]), seed);
    } else {
        bad(shape, "unknown shape");
    }
    s.seed = seed;
    if (food == StartFood::Random) randomise_initial_food(s);
    return s;
}

}  // namespace cf
