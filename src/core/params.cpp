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
// Food sources of M5. Written only when they differ from the default, so states that do not
// use them keep their text form and their checksum.
constexpr FloatField kExtraFields[] = {
    {"fieldAmount", &Params::fieldAmount},     {"fieldScale", &Params::fieldScale},       {"fieldOctaves", &Params::fieldOctaves},
    {"fieldContrast", &Params::fieldContrast}, {"pulsePeriod", &Params::pulsePeriod},     {"pulseDuty", &Params::pulseDuty},
    {"pulseLow", &Params::pulseLow},           {"lightAmount", &Params::lightAmount},     {"lightDirX", &Params::lightDirX},
    {"lightDirY", &Params::lightDirY},         {"lightDirZ", &Params::lightDirZ},         {"lightAmbient", &Params::lightAmbient},
    {"lightBias", &Params::lightBias},         {"lightPixel", &Params::lightPixel},       {"foodDiffusion", &Params::foodDiffusion},
    {"foodDecay", &Params::foodDecay},         {"crowdLow", &Params::crowdLow},           {"crowdHigh", &Params::crowdHigh},
    {"orientAmount", &Params::orientAmount},   {"orientAxisX", &Params::orientAxisX},     {"orientAxisY", &Params::orientAxisY},
    {"orientAxisZ", &Params::orientAxisZ},     {"fieldBulge", &Params::fieldBulge},       {"morphogens", &Params::morphogens},
    {"morphDiffA", &Params::morphDiffA},       {"morphDiffB", &Params::morphDiffB},       {"morphFeed", &Params::morphFeed},
    {"morphKill", &Params::morphKill},         {"morphIterations", &Params::morphIterations}, {"morphSeeds", &Params::morphSeeds},
    {"morphFood", &Params::morphFood},         {"morphBulge", &Params::morphBulge},       {"morphPlanar", &Params::morphPlanar},
    {"morphRest", &Params::morphRest},
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
    auto assign = [&](const FloatField& f) -> std::string {
        char* end = nullptr;
        const float x = std::strtof(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0') return "bad value for " + std::string(key) + ": '" + v + "'";
        p.*(f.member) = x;
        return "";
    };
    for (const FloatField& f : kFloatFields)
        if (key == f.name) return assign(f);
    for (const FloatField& f : kExtraFields)
        if (key == f.name) return assign(f);
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
    for (const FloatField& f : kExtraFields) all = finite(f.name, p.*(f.member)) && all;
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

    // Food sources (M5).
    if (p.fieldAmount < 0.0f || p.fieldAmount > 1.0f) r.errors.push_back("fieldAmount must be in [0, 1]");
    if (!(p.fieldScale > 0.0f)) r.errors.push_back("fieldScale must be > 0");
    if (p.fieldOctaves < 1.0f || p.fieldOctaves > 6.0f) r.errors.push_back("fieldOctaves must be in [1, 6]");
    if (p.fieldContrast < 0.0f) r.errors.push_back("fieldContrast must be >= 0");
    if (p.pulsePeriod < 0.0f || p.pulsePeriod > 1.0e9f) r.errors.push_back("pulsePeriod must be in [0, 1e9] steps (0 = off)");
    if (p.pulseDuty < 0.0f || p.pulseDuty > 1.0f) r.errors.push_back("pulseDuty must be in [0, 1]");
    if (p.pulseLow < 0.0f || p.pulseLow > 1.0f) r.errors.push_back("pulseLow must be in [0, 1]");
    if (p.lightAmount < 0.0f || p.lightAmount > 1.0f) r.errors.push_back("lightAmount must be in [0, 1]");
    if (p.lightAmount > 0.0f && !(p.lightDirX * p.lightDirX + p.lightDirY * p.lightDirY + p.lightDirZ * p.lightDirZ > 1.0e-12f))
        r.errors.push_back("the light direction must not be zero");
    if (p.lightAmbient < 0.0f || p.lightAmbient > 1.0f) r.errors.push_back("lightAmbient must be in [0, 1]");
    if (p.lightBias < 0.0f) r.errors.push_back("lightBias must be >= 0");
    if (!(p.lightPixel > 0.0f)) r.errors.push_back("lightPixel must be > 0");
    if (p.foodDiffusion < 0.0f || p.foodDiffusion >= 1.0f) r.errors.push_back("foodDiffusion must be in [0, 1)");
    if (p.foodDecay < 0.0f || p.foodDecay >= 1.0f) r.errors.push_back("foodDecay must be in [0, 1)");
    if (p.crowdLow < 0.0f || p.crowdHigh < 0.0f) r.errors.push_back("crowdLow and crowdHigh must be >= 0");

    // Model extensions (M8).
    if (p.orientAmount < 0.0f || p.orientAmount > 1.0f) r.errors.push_back("orientAmount must be in [0, 1]");
    if (p.orientAmount > 0.0f && !(p.orientAxisX * p.orientAxisX + p.orientAxisY * p.orientAxisY + p.orientAxisZ * p.orientAxisZ > 1.0e-12f))
        r.errors.push_back("the orientation axis must not be zero");
    if (p.fieldBulge < -1.0f || p.fieldBulge > 1.0f) r.errors.push_back("fieldBulge must be in [-1, 1]");
    if (p.morphDiffA < 0.0f || p.morphDiffA > 1.0f || p.morphDiffB < 0.0f || p.morphDiffB > 1.0f)
        r.errors.push_back("morphDiffA and morphDiffB must be in [0, 1]");
    if (p.morphFeed < 0.0f || p.morphFeed > 1.0f || p.morphKill < 0.0f || p.morphKill > 1.0f)
        r.errors.push_back("morphFeed and morphKill must be in [0, 1]");
    if (p.morphSeeds < 0.0f || p.morphSeeds > 1.0f) r.errors.push_back("morphSeeds must be in [0, 1]");
    if (p.morphFood < -1.0f || p.morphFood > 1.0f || p.morphBulge < -1.0f || p.morphBulge > 1.0f || p.morphPlanar < -1.0f || p.morphPlanar > 1.0f)
        r.errors.push_back("morphFood, morphBulge and morphPlanar must be in [-1, 1]");
    if (p.morphRest < -0.5f || p.morphRest > 0.5f) r.errors.push_back("morphRest must be in [-0.5, 0.5]");
    return r;
}

bool uses_model_extensions(const Params& p) {
    return uses_food_extensions(p) || p.orientAmount > 0.0f || p.fieldBulge != 0.0f || p.morphogens != 0.0f;
}

bool uses_food_extensions(const Params& p) {
    return p.fieldAmount > 0.0f || p.pulsePeriod > 0.0f || p.lightAmount > 0.0f || p.foodDiffusion > 0.0f || p.foodDecay > 0.0f ||
           p.crowdHigh > p.crowdLow;
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
    const Params defaults;
    for (const FloatField& f : kExtraFields) {
        if (p.*(f.member) == defaults.*(f.member)) continue;  // unchanged: keeps old files and checksums valid
        std::snprintf(buf, sizeof(buf), "%s=%.9g\n", f.name, static_cast<double>(p.*(f.member)));
        out += buf;
    }
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
