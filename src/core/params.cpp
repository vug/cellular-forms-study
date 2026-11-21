#include "params.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cf {
namespace {

struct FloatField {
    const char* name;
    float Params::* member;
};
constexpr FloatField kFloatFields[] = {
    {"linkRestLength", &Params::linkRestLength},
    {"springFactor", &Params::springFactor},
    {"planarFactor", &Params::planarFactor},
    {"bulgeFactor", &Params::bulgeFactor},
    {"repulsionStrength", &Params::repulsionStrength},
    {"radiusOfInfluence", &Params::radiusOfInfluence},
    {"foodThreshold", &Params::foodThreshold},
    {"foodMin", &Params::foodMin},
    {"foodMax", &Params::foodMax},
    {"maxDisplacement", &Params::maxDisplacement},
    {"daughterOffset", &Params::daughterOffset},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string apply_pair(Params& p, std::string_view key, std::string_view value) {
    const std::string v(value);
    if (key == "maxCells") {
        char* end = nullptr;
        const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0' || n == 0 || n >= 0xFFFFFFFFull) return "bad value for maxCells: '" + v + "'";
        p.maxCells = static_cast<std::uint32_t>(n);
        return "";
    }
    for (const FloatField& f : kFloatFields) {
        if (key == f.name) {
            char* end = nullptr;
            const float x = std::strtof(v.c_str(), &end);
            if (end == v.c_str() || *end != '\0') return "bad value for " + std::string(key) + ": '" + v + "'";
            p.*(f.member) = x;
            return "";
        }
    }
    return "unknown parameter '" + std::string(key) + "'";
}

}  // namespace

ParamIssues validate(const Params& p) {
    ParamIssues r;
    auto finite = [&](const char* name, float x) {
        if (!std::isfinite(x)) {
            r.errors.push_back(std::string(name) + " is not finite");
            return false;
        }
        return true;
    };
    bool all = true;
    for (const FloatField& f : kFloatFields) all = finite(f.name, p.*(f.member)) && all;
    if (!all) return r;

    if (!(p.linkRestLength > 0.0f)) r.errors.push_back("linkRestLength must be > 0");
    if (!(p.radiusOfInfluence > 0.0f)) r.errors.push_back("radiusOfInfluence must be > 0");
    if (p.springFactor < 0.0f) r.errors.push_back("springFactor must be >= 0");
    if (p.planarFactor < 0.0f) r.errors.push_back("planarFactor must be >= 0");
    if (p.bulgeFactor < 0.0f) r.errors.push_back("bulgeFactor must be >= 0");
    if (p.repulsionStrength < 0.0f) r.errors.push_back("repulsionStrength must be >= 0");
    const float sum = p.springFactor + p.planarFactor + p.bulgeFactor;
    // P' = P + k (target - P) diverges for k >= 2 (docs/model.md section 2).
    if (sum >= 2.0f) r.errors.push_back("springFactor + planarFactor + bulgeFactor must be < 2 (the update diverges)");
    else if (sum > 1.0f) r.warnings.push_back("springFactor + planarFactor + bulgeFactor > 1: the update overshoots");
    if (!(p.foodThreshold > 0.0f)) r.errors.push_back("foodThreshold must be > 0");
    if (p.foodMin < 0.0f) r.errors.push_back("foodMin must be >= 0");
    if (p.foodMax < p.foodMin) r.errors.push_back("foodMax must be >= foodMin");
    if (p.maxDisplacement < 0.0f) r.errors.push_back("maxDisplacement must be >= 0 (0 disables the cap)");
    if (p.maxDisplacement == 0.0f) r.warnings.push_back("maxDisplacement = 0: the displacement cap is off");
    if (!(p.daughterOffset > 0.0f)) r.errors.push_back("daughterOffset must be > 0 (a daughter may never sit on its parent)");
    else if (p.daughterOffset > 0.5f) r.warnings.push_back("daughterOffset > 0.5: daughters start further apart than a rest length");
    if (p.maxCells < 4u) r.errors.push_back("maxCells must be >= 4");
    if (p.repulsionStrength > 1.0f) r.warnings.push_back("repulsionStrength > 1 is far outside the tuned range");
    return r;
}

std::string to_text(const Params& p) {
    std::string out;
    char buf[96];
    for (const FloatField& f : kFloatFields) {
        // 9 significant digits round-trip any float32 exactly.
        std::snprintf(buf, sizeof(buf), "%s=%.9g\n", f.name, static_cast<double>(p.*(f.member)));
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "maxCells=%u\n", static_cast<unsigned>(p.maxCells));
    out += buf;
    return out;
}

std::string apply_text(Params& p, std::string_view text) {
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = (nl == std::string_view::npos) ? std::string_view{} : text.substr(nl + 1);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return "expected key=value, got '" + std::string(line) + "'";
        const std::string err = apply_pair(p, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
        if (!err.empty()) return err;
    }
    return "";
}

}  // namespace cf
