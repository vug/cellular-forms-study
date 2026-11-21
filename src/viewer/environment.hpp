// Environment maps for the renderer: equirectangular images of the surroundings in linear
// colour, three floats per pixel, top row first (straight up), the centre column looking
// along +Z.
#pragma once
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

}  // namespace cfv
