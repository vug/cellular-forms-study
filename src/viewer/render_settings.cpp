// Render settings by name (for the command line and scripts), and what the settings imply.
#include <algorithm>
#include <cstdlib>
#include <functional>

#include "materials.hpp"
#include "renderer.hpp"

namespace cfv {

const char* cellPropertyName(int property) {
    static const char* names[] = {"none", "age", "food", "birth", "generation", "curvature", "activator"};
    return property >= 0 && property <= kCellProperties ? names[property] : "none";
}

bool materialUses(const RenderSettings& s, int channel) {
    if (channel < 0 || channel >= kMaterialChannels) return false;
    const bool driven = s.modSource[channel] != 0;
    const float most = driven ? std::max(s.modLo[channel], s.modHi[channel]) : 1.0f;
    switch (channel) {
        case kMatEmission: return s.emission > 0.0f && most > 0.0f;
        case kMatRoughness: return true;
        case kMatSubsurface: return s.sssRadius > 0.0f && (driven ? most > 0.0f : s.subsurface > 0.0f);
        case kMatTranslucency: return driven ? most > 0.0f : s.translucency > 0.0f;
    }
    return false;
}

void setPropertyRanges(RenderSettings& s, const float* lo, const float* hi) {
    for (int p = 0; p < kCellProperties; ++p) {
        s.propMin[p] = lo[p];
        s.propMax[p] = hi[p];
    }
    const int colour = static_cast<int>(s.colorMode);
    if (colour >= 1 && colour <= kCellProperties) {
        s.scalarMin = lo[colour - 1];
        s.scalarMax = hi[colour - 1];
    }
}

int extraNeeds(const RenderSettings& s) {
    int mask = extraBitFor(static_cast<int>(s.colorMode));
    for (int c = 0; c < kMaterialChannels; ++c)
        if (materialUses(s, c)) mask |= extraBitFor(s.modSource[c]);
    return mask;
}

namespace {

bool parseFloats(const std::string& text, float* out, int count) {
    const char* p = text.c_str();
    for (int i = 0; i < count; ++i) {
        char* end = nullptr;
        out[i] = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    return *p == '\0';
}

bool parseBool(const std::string& v, bool& out) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") out = true;
    else if (v == "0" || v == "false" || v == "off" || v == "no") out = false;
    else return false;
    return true;
}

bool parseProperty(const std::string& v, int& out) {
    for (int i = 0; i <= kCellProperties; ++i)
        if (v == cellPropertyName(i)) {
            out = i;
            return true;
        }
    return false;
}

struct Entry {
    std::string name;
    std::function<bool(RenderSettings&, const std::string&)> set;
};

Entry number(const char* name, float RenderSettings::* field) {
    return {name, [field](RenderSettings& s, const std::string& v) { return parseFloats(v, &(s.*field), 1); }};
}
Entry whole(const char* name, int RenderSettings::* field) {
    return {name, [field](RenderSettings& s, const std::string& v) {
                float x = 0.0f;
                if (!parseFloats(v, &x, 1)) return false;
                s.*field = static_cast<int>(x);
                return true;
            }};
}
Entry flag(const char* name, bool RenderSettings::* field) {
    return {name, [field](RenderSettings& s, const std::string& v) { return parseBool(v, s.*field); }};
}
Entry triple(const char* name, float (RenderSettings::* field)[3]) {
    return {name, [field](RenderSettings& s, const std::string& v) { return parseFloats(v, s.*field, 3); }};
}

std::vector<Entry> makeEntries() {
    std::vector<Entry> table = {
        {"drawMode",
         [](RenderSettings& s, const std::string& v) {
             const std::pair<const char*, int> names[] = {{"cells", kDrawCells},       {"xray", kDrawXray},
                                                          {"membrane", kDrawMembrane}, {"mesh", kDrawMembrane},
                                                          {"path", kDrawPathTraced},   {"pathmesh", kDrawMembranePathTraced}};
             for (const auto& [name, mode] : names)
                 if (v == name) {
                     s.drawMode = mode;
                     return true;
                 }
             if (v == "sss") {  // the membrane as skin
                 s.drawMode = kDrawMembrane;
                 applyMaterial(s, materials()[static_cast<std::size_t>(findMaterial("skin"))], true);
                 return true;
             }
             return false;
         }},
        number("radius", &RenderSettings::sphereRadius),
        number("minPixelRadius", &RenderSettings::minPixelRadius),
        whole("meshSmooth", &RenderSettings::meshSmooth),
        flag("meshRelax", &RenderSettings::meshRelax),
        {"color",
         [](RenderSettings& s, const std::string& v) {
             if (v == "uniform") {
                 s.colorMode = ColorMode::Uniform;
                 return true;
             }
             int property = 0;
             if (!parseProperty(v, property) || property == 0) return false;
             s.colorMode = static_cast<ColorMode>(property);
             return true;
         }},
        flag("autoRange", &RenderSettings::autoRange),
        number("min", &RenderSettings::scalarMin),
        number("max", &RenderSettings::scalarMax),
        whole("palette", &RenderSettings::palette),
        flag("paletteReverse", &RenderSettings::paletteReverse),
        triple("baseColor", &RenderSettings::baseColor),
        // The material: a whole one by name (with its colour), or part by part.
        {"material",
         [](RenderSettings& s, const std::string& v) {
             const int i = findMaterial(v);
             if (i < 0) return false;
             applyMaterial(s, materials()[static_cast<std::size_t>(i)], true);
             return true;
         }},
        {"materialOnly",  // the same without its colour: the cells keep their colour mode
         [](RenderSettings& s, const std::string& v) {
             const int i = findMaterial(v);
             if (i < 0) return false;
             applyMaterial(s, materials()[static_cast<std::size_t>(i)], false);
             return true;
         }},
        number("roughness", &RenderSettings::roughness),
        number("metalness", &RenderSettings::metalness),
        number("specular", &RenderSettings::specular),
        number("sheen", &RenderSettings::sheen),
        number("subsurface", &RenderSettings::subsurface),
        number("sssRadius", &RenderSettings::sssRadius),
        triple("sssColor", &RenderSettings::sssColor),
        number("translucency", &RenderSettings::translucency),
        number("transDepth", &RenderSettings::transDepth),
        number("emission", &RenderSettings::emission),
        triple("emissionColor", &RenderSettings::emissionColor),
        number("emissionTint", &RenderSettings::emissionTint),
        flag("emissionLights", &RenderSettings::emissionLights),
        number("xrayDensity", &RenderSettings::xrayDensity),
        flag("lightFollowsCamera", &RenderSettings::lightFollowsCamera),
        flag("simLight", &RenderSettings::simLight),
        triple("lightDir", &RenderSettings::lightDir),
        number("sunAzimuth", &RenderSettings::sunAzimuth),
        number("sunElevation", &RenderSettings::sunElevation),
        number("lightIntensity", &RenderSettings::lightIntensity),
        triple("lightColor", &RenderSettings::lightColor),
        number("ambient", &RenderSettings::ambient),
        triple("skyColor", &RenderSettings::skyColor),
        triple("groundColor", &RenderSettings::groundColor),
        flag("shadows", &RenderSettings::shadows),
        whole("shadowMapSize", &RenderSettings::shadowMapSize),
        number("shadowStrength", &RenderSettings::shadowStrength),
        number("shadowSoftness", &RenderSettings::shadowSoftness),
        number("shadowBias", &RenderSettings::shadowBias),
        flag("ao", &RenderSettings::aoEnabled),
        number("aoSmall", &RenderSettings::aoRadiusSmall),
        number("aoLarge", &RenderSettings::aoRadiusLarge),
        number("aoIntensity", &RenderSettings::aoIntensity),
        flag("rtAo", &RenderSettings::rtAo),
        whole("rtAoRays", &RenderSettings::rtAoRays),
        number("rtAoRadius", &RenderSettings::rtAoRadius),
        flag("walls", &RenderSettings::walls),
        number("wallWidth", &RenderSettings::wallWidth),
        number("wallStrength", &RenderSettings::wallStrength),
        triple("wallColor", &RenderSettings::wallColor),
        whole("toonBands", &RenderSettings::toonBands),
        flag("outlines", &RenderSettings::outlines),
        number("outlineWidth", &RenderSettings::outlineWidth),
        number("outlineThreshold", &RenderSettings::outlineThreshold),
        number("outlineStrength", &RenderSettings::outlineStrength),
        triple("outlineColor", &RenderSettings::outlineColor),
        number("lensAperture", &RenderSettings::lensAperture),
        number("lensFocus", &RenderSettings::lensFocus),
        number("lensFocusShift", &RenderSettings::lensFocusShift),
        number("dofMaxBlur", &RenderSettings::dofMaxBlur),
        flag("fxaa", &RenderSettings::fxaa),
        flag("taa", &RenderSettings::taa),
        flag("cellMotion", &RenderSettings::cellMotion),
        flag("aaAccumulate", &RenderSettings::aaAccumulate),
        whole("aaFrames", &RenderSettings::aaFrames),
        number("renderScale", &RenderSettings::renderScale),
        number("bloom", &RenderSettings::bloom),
        number("bloomThreshold", &RenderSettings::bloomThreshold),
        whole("ptBounces", &RenderSettings::ptBounces),
        whole("ptSamplesPerFrame", &RenderSettings::ptSamplesPerFrame),
        whole("ptMaxSamples", &RenderSettings::ptMaxSamples),
        number("ptSunSize", &RenderSettings::ptSunSize),
        number("ptSunIntensity", &RenderSettings::ptSunIntensity),
        number("ptSkyIntensity", &RenderSettings::ptSkyIntensity),
        number("ptClamp", &RenderSettings::ptClamp),
        flag("ptWalk", &RenderSettings::ptWalk),
        flag("ptTemporal", &RenderSettings::ptTemporal),
        flag("ptDenoise", &RenderSettings::ptDenoise),
        number("ptDenoiseStrength", &RenderSettings::ptDenoiseStrength),
        flag("envLight", &RenderSettings::envLight),
        number("envIntensity", &RenderSettings::envIntensity),
        number("envRotation", &RenderSettings::envRotation),
        flag("envBackground", &RenderSettings::envBackground),
        {"envFile",
         [](RenderSettings& st, const std::string& v) {
             st.envFile = v;
             return true;
         }},
        {"stereo",
         [](RenderSettings& st, const std::string& v) {
             const char* names[] = {"off", "sidebyside", "crosseyed", "anaglyph"};
             for (int i = 0; i < 4; ++i)
                 if (v == names[i]) {
                     st.stereoMode = i;
                     return true;
                 }
             return false;
         }},
        number("stereoSeparation", &RenderSettings::stereoSeparation),
        number("exposure", &RenderSettings::exposure),
        whole("toneMap", &RenderSettings::toneMap),
        triple("background", &RenderSettings::background),
        number("backgroundGradient", &RenderSettings::backgroundGradient),
        number("vignette", &RenderSettings::vignette),
        whole("debugView", &RenderSettings::debugView),
    };
    // A cell property driving part of the material: emissionBy=food, emissionLo=0, emissionHi=1,
    // emissionFrom=0.6, emissionTo=1 (only the top of the range glows), and the same for
    // roughness, subsurface and translucency.
    const char* channels[kMaterialChannels] = {"emission", "roughness", "subsurface", "translucency"};
    for (int c = 0; c < kMaterialChannels; ++c) {
        table.push_back({std::string(channels[c]) + "By", [c](RenderSettings& s, const std::string& v) { return parseProperty(v, s.modSource[c]); }});
        table.push_back({std::string(channels[c]) + "Lo", [c](RenderSettings& s, const std::string& v) { return parseFloats(v, &s.modLo[c], 1); }});
        table.push_back({std::string(channels[c]) + "Hi", [c](RenderSettings& s, const std::string& v) { return parseFloats(v, &s.modHi[c], 1); }});
        table.push_back({std::string(channels[c]) + "From", [c](RenderSettings& s, const std::string& v) { return parseFloats(v, &s.modFrom[c], 1); }});
        table.push_back({std::string(channels[c]) + "To", [c](RenderSettings& s, const std::string& v) { return parseFloats(v, &s.modTo[c], 1); }});
    }
    return table;
}

const std::vector<Entry>& entries() {
    static const std::vector<Entry> table = makeEntries();
    return table;
}

}  // namespace

std::string setRenderSetting(RenderSettings& settings, const std::string& name, const std::string& value) {
    for (const Entry& e : entries())
        if (name == e.name) return e.set(settings, value) ? std::string() : "bad value '" + value + "' for " + name;
    return "unknown render setting '" + name + "'";
}

std::vector<std::string> renderSettingNames() {
    std::vector<std::string> names;
    for (const Entry& e : entries()) names.push_back(e.name);
    return names;
}

}  // namespace cfv
