// Adapter between the simulation's file formats (src/core) and the viewer's Cloud.
#pragma once
#include <filesystem>

#include "cloud.hpp"

namespace cfv {

// True for files the viewer can load (by extension).
bool isCloudFile(const std::filesystem::path& file);

// Reads positions, birth steps and food from a state or snapshot file.
// Throws std::runtime_error with the reason when the file cannot be read.
Cloud readCloudFile(const std::filesystem::path& file);

}  // namespace cfv
