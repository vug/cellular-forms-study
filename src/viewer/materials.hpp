// A small library of materials for the cells: each sets the material part of RenderSettings
// (roughness, metal, highlight, sheen, subsurface scattering, translucency, glow, and which cell
// property drives what), and optionally the colour the look depends on.
#pragma once
#include <string>
#include <vector>

#include "renderer.hpp"

namespace cfv {

struct Material {
    const char* name;
    const char* note;  // one line for the UI
    float color[3];    // the colour that goes with it
    float roughness, metalness, specular, sheen;
    float subsurface, sssRadius;
    float sssColor[3];
    float translucency, transDepth;
    float emission;
    float emissionColor[3];
    float emissionTint;
    int modSource[kMaterialChannels];  // cell property that drives emission, roughness, subsurface, translucency (0: none)
    float modLo[kMaterialChannels], modHi[kMaterialChannels];
    bool environment;  // looks like little without surroundings to mirror: turns the environment light on
    // The part of the property's range over which the driven parts change (see RenderSettings).
    float modFrom[kMaterialChannels] = {0.0f, 0.0f, 0.0f, 0.0f};
    float modTo[kMaterialChannels] = {1.0f, 1.0f, 1.0f, 1.0f};
};

const std::vector<Material>& materials();
// Index of the material with this name, or -1.
int findMaterial(const std::string& name);
// Index of the material whose surface, scattering and glow the settings have, or -1.
int matchMaterial(const RenderSettings& settings);
// Sets the material part of the settings. With `withColor`, also colours every cell in the
// material's own colour (the "uniform" colour mode).
void applyMaterial(RenderSettings& settings, const Material& material, bool withColor);

}  // namespace cfv
