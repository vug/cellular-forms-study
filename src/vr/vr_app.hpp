// cf_vr: a live growing form in an OpenXR headset (Meta Quest through Link, SteamVR, ...).
#pragma once
#include <filesystem>

namespace cfvr {

struct VrOptions {
    int galleryId = 5031;              // the form of the gallery to grow
    bool galleryGiven = false;         // --gallery was on the command line
    std::filesystem::path galleryFile; // empty: web/app/gallery/gallery.json of this repository
    bool mirror = false;               // a desktop window showing the left eye
    bool validate = false;             // the Vulkan validation layer; exit code 3 on any error
    int exitAfter = 0;                 // > 0: end after this many frames and print frame times
};

// Exit codes: 0 done, 1 error, 2 no headset (or the session never started), 3 validation errors.
int runVr(const VrOptions& options);

}  // namespace cfvr
