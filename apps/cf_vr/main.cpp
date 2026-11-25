// cf_vr: a live growing form in a headset, through OpenXR (Meta Quest with Link, or any other
// PC runtime such as SteamVR).
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

#include "vr_app.hpp"

namespace {

void usage() {
    std::puts(
        "cf_vr: grow a form live in a headset (OpenXR: Meta Quest through Link, SteamVR, ...)\n"
        "\n"
        "With no options it grows form 5031 of the gallery (web/app/gallery/gallery.json).\n"
        "  --gallery <id>         grow another form of the gallery\n"
        "  --gallery-file <path>  another gallery.json\n"
        "  --mirror               also show the left eye in a window on the desktop\n"
        "  --validate             Vulkan validation layer; exit code 3 on any error\n"
        "  --exit-after <frames>  end after this many frames and print frame times (for tests)\n"
        "  --help\n"
        "\n"
        "In the headset: grip holds the form (move and turn it with the hand; both grips also scale\n"
        "it), A runs or pauses, B starts it again, Y or the menu button opens the menu, where the\n"
        "trigger clicks and the thumbstick scrolls.\n"
        "\n"
        "Exit codes: 0 done, 1 error, 2 no headset (or the session did not start), 3 validation errors.");
}

}  // namespace

int main(int argc, char** argv) {
    cfvr::VrOptions o;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto next = [&]() -> const char* {
                if (i + 1 >= argc) throw std::runtime_error("missing value after " + a);
                return argv[++i];
            };
            if (a == "--help" || a == "-h") {
                usage();
                return 0;
            } else if (a == "--gallery") {
                char* end = nullptr;
                const char* v = next();
                o.galleryId = static_cast<int>(std::strtol(v, &end, 10));
                if (end == v || *end != '\0') throw std::runtime_error("--gallery expects the number of a form");
                o.galleryGiven = true;
            } else if (a == "--gallery-file") {
                o.galleryFile = next();
            } else if (a == "--mirror") {
                o.mirror = true;
            } else if (a == "--validate") {
                o.validate = true;
            } else if (a == "--exit-after") {
                o.exitAfter = std::atoi(next());
                if (o.exitAfter <= 0) throw std::runtime_error("--exit-after expects a number of frames");
            } else {
                throw std::runtime_error("unknown option " + a + " (try --help)");
            }
        }
        return cfvr::runVr(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_vr: %s\n", e.what());
        return 1;
    }
}
