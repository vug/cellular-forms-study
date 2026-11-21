#include "materials.hpp"

#include <algorithm>

namespace cfv {

const std::vector<Material>& materials() {
    // Scatter distances are in world units; cells sit about one unit apart. The three numbers
    // after the radius say how far red, green and blue get relative to it.
    static const std::vector<Material> table = {
        // name, note, colour, roughness, metal, specular, sheen, subsurface, radius, per colour, translucency, depth,
        // emission, colour, tint, driven by (emission, roughness, subsurface, translucency), low, high, environment
        {"clay", "matt, nothing scatters: the plain look the viewer starts with", {0.80f, 0.74f, 0.66f}, 0.6f, 0.0f, 0.5f, 0.0f, 0.0f, 1.5f, {1.0f, 0.35f, 0.2f}, 0.0f, 3.0f,
         0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false},
        {"skin", "soft highlights, red light under the surface, thin parts glow against the light", {0.80f, 0.52f, 0.42f}, 0.42f, 0.0f, 0.5f, 0.25f,
         0.9f, 2.6f, {1.0f, 0.36f, 0.18f}, 0.15f, 4.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f},
         {1.0f, 0.9f, 1.0f, 1.0f}, false},
        {"tissue", "a thin wet membrane: much of the light goes through", {0.90f, 0.72f, 0.68f}, 0.3f, 0.0f, 0.6f, 0.3f, 0.6f, 1.6f, {1.0f, 0.5f, 0.4f},
         0.6f, 7.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false},
        {"wax", "light travels far inside and comes out warm", {0.93f, 0.86f, 0.70f}, 0.35f, 0.0f, 0.5f, 0.0f, 1.0f, 3.2f, {1.0f, 0.72f, 0.42f}, 0.4f, 6.0f,
         0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false},
        {"jade", "polished stone that lets green light through", {0.36f, 0.68f, 0.46f}, 0.14f, 0.0f, 0.65f, 0.0f, 0.9f, 2.6f, {0.45f, 1.0f, 0.6f}, 0.35f,
         5.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, true},
        {"porcelain", "glazed and white, a little light under the glaze", {0.95f, 0.94f, 0.91f}, 0.08f, 0.0f, 0.65f, 0.0f, 0.5f, 0.8f, {1.0f, 0.9f, 0.8f},
         0.0f, 3.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, true},
        {"pearl", "smooth with a pale rim at grazing angles", {0.92f, 0.88f, 0.86f}, 0.2f, 0.0f, 0.8f, 0.6f, 0.4f, 1.0f, {1.0f, 0.85f, 0.8f}, 0.0f, 3.0f,
         0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, true},
        {"rubber", "dark and matt with a broad highlight", {0.07f, 0.07f, 0.08f}, 0.65f, 0.0f, 0.5f, 0.0f, 0.0f, 1.5f, {1.0f, 0.35f, 0.2f}, 0.0f, 3.0f,
         0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false},
        {"gold", "a metal: it mirrors its surroundings in its own colour", {1.0f, 0.77f, 0.34f}, 0.25f, 1.0f, 0.5f, 0.0f, 0.0f, 1.5f, {1.0f, 0.35f, 0.2f},
         0.0f, 3.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, true},
        {"chrome", "a mirror", {0.95f, 0.95f, 0.96f}, 0.06f, 1.0f, 0.5f, 0.0f, 0.0f, 1.5f, {1.0f, 0.35f, 0.2f}, 0.0f, 3.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f,
         {0, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, true},
        {"copper patina", "metal that is rougher where the surface curves in", {0.85f, 0.47f, 0.34f}, 0.3f, 1.0f, 0.5f, 0.0f, 0.0f, 1.5f,
         {1.0f, 0.35f, 0.2f}, 0.0f, 3.0f, 0.0f, {1.0f, 0.75f, 0.35f}, 0.0f, {0, 5, 0, 0}, {0.0f, 0.75f, 0.0f, 0.0f}, {1.0f, 0.12f, 1.0f, 1.0f}, true},
        {"bioluminescent", "a dark translucent body; cells glow by how much food they hold", {0.05f, 0.09f, 0.13f}, 0.4f, 0.0f, 0.5f, 0.2f, 0.7f, 2.0f,
         {0.5f, 0.9f, 1.0f}, 0.5f, 6.0f, 3.0f, {0.15f, 0.85f, 1.0f}, 1.0f, {2, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false,
         {0.55f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {"ember", "the youngest cells glow like coals", {0.10f, 0.04f, 0.03f}, 0.7f, 0.0f, 0.3f, 0.0f, 0.5f, 1.5f, {1.0f, 0.4f, 0.2f}, 0.3f, 4.0f, 5.0f,
         {1.0f, 0.38f, 0.08f}, 1.0f, {1, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false,
         {0.7f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        {"growth light", "skin that glows where the morphogen activator is high", {0.80f, 0.60f, 0.55f}, 0.45f, 0.0f, 0.5f, 0.2f, 0.8f, 2.0f,
         {1.0f, 0.4f, 0.25f}, 0.3f, 4.0f, 3.0f, {1.0f, 0.8f, 0.4f}, 1.0f, {6, 0, 0, 0}, {0.0f, 0.2f, 0.0f, 0.0f}, {1.0f, 0.9f, 1.0f, 1.0f}, false,
         {0.3f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
    };
    return table;
}

int findMaterial(const std::string& name) {
    const std::vector<Material>& all = materials();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (name == all[i].name) return static_cast<int>(i);
    // Also without the space of a two-word name ("copperpatina", "growthlight").
    for (std::size_t i = 0; i < all.size(); ++i) {
        std::string compact = all[i].name;
        compact.erase(std::remove(compact.begin(), compact.end(), ' '), compact.end());
        if (name == compact) return static_cast<int>(i);
    }
    return -1;
}

int matchMaterial(const RenderSettings& s) {
    const std::vector<Material>& all = materials();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const Material& m = all[i];
        if (s.roughness == m.roughness && s.metalness == m.metalness && s.sheen == m.sheen && s.subsurface == m.subsurface && s.sssRadius == m.sssRadius &&
            s.translucency == m.translucency && s.emission == m.emission && s.modSource[0] == m.modSource[0] && s.modSource[1] == m.modSource[1])
            return static_cast<int>(i);
    }
    return -1;
}

void applyMaterial(RenderSettings& s, const Material& m, bool withColor) {
    s.roughness = m.roughness;
    s.metalness = m.metalness;
    s.specular = m.specular;
    s.sheen = m.sheen;
    s.subsurface = m.subsurface;
    s.sssRadius = m.sssRadius;
    std::copy(m.sssColor, m.sssColor + 3, s.sssColor);
    s.translucency = m.translucency;
    s.transDepth = m.transDepth;
    s.emission = m.emission;
    std::copy(m.emissionColor, m.emissionColor + 3, s.emissionColor);
    s.emissionTint = m.emissionTint;
    for (int c = 0; c < kMaterialChannels; ++c) {
        s.modSource[c] = m.modSource[c];
        s.modLo[c] = m.modLo[c];
        s.modHi[c] = m.modHi[c];
        s.modFrom[c] = m.modFrom[c];
        s.modTo[c] = m.modTo[c];
    }
    if (m.environment) s.envLight = true;
    if (withColor) {
        s.colorMode = ColorMode::Uniform;
        std::copy(m.color, m.color + 3, s.baseColor);
    }
}

}  // namespace cfv
