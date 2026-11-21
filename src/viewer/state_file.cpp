#include "state_file.hpp"

#include <stdexcept>

namespace cfv {

bool isCloudFile(const std::filesystem::path& file) { return file.extension() == ".cfs"; }

Cloud readCloudFile(const std::filesystem::path& file) {
    // The state file format is defined in src/core; this is filled in when it lands.
    throw std::runtime_error("reading " + file.string() + ": state files are not supported by this build yet");
}

}  // namespace cfv
