// Environment maps for the renderer: equirectangular images of the surroundings in linear
// colour, three floats per pixel, top row first (straight up), the centre column looking
// along +Z.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace cfv {

// The built-in map: a studio with a large soft key light above and to one side, a weaker fill
// light, a rim light from behind, and a floor.
std::vector<float> studioEnvironment(int width, int height);

// Loads an image file as an environment map (.hdr as it is; .png, .jpg and other 8-bit formats
// converted to linear colour). Images wider than 4096 pixels are reduced. Returns false and the
// reason when the file cannot be read.
bool loadEnvironmentImage(const std::string& file, std::vector<float>& rgb, int& width, int& height, std::string& error);

// Tables for picking directions in proportion to the light that comes from them (the path
// tracer; see shaders/env.glsl). They are made from a copy of the map reduced to about 256
// texels across, each texel weighted by the area it covers on the sphere.
struct EnvironmentTable {
    int level = 0;             // the level of the map's chain of smaller copies that has this size
    int width = 1, height = 1;
    std::vector<float> table;  // 2 per texel: share of its row's light up to and including it; its light
    std::vector<float> rows;   // per row: share of all the light up to and including the row
    float mean = 1.0f;         // mean of the texels' light
};
EnvironmentTable environmentTable(const float* rgb, int width, int height);

// The folder of environment maps that comes with the repository (assets/hdri), looked for from
// the working directory and from where the program is; empty when it is not found.
std::filesystem::path environmentFolder();
// The image files in it, by name.
std::vector<std::string> listEnvironments();
// A file as the settings name it: a path that exists, or the name of a file in the folder
// above, with or without its extension ("satara_night" finds satara_night_1k.hdr).
std::string resolveEnvironment(const std::string& name);

}  // namespace cfv
