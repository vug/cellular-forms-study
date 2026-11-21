// Adapter between the simulation's file formats (src/core) and the viewer's Cloud.
#pragma once
#include <filesystem>

#include "cloud.hpp"

namespace cfv {

// True for files the viewer can load (by extension).
bool isCloudFile(const std::filesystem::path& file);

// Reads positions, birth steps, food and generations from a state or snapshot file; from a
// checkpoint also the normals, the triangles of the membrane, the curvature and (when the state
// has morphogens) the activator. Throws std::runtime_error with the reason when the file cannot
// be read.
Cloud readCloudFile(const std::filesystem::path& file);

}  // namespace cfv
