// cf_view: Vulkan 1.4 viewer for Cellular Forms states and snapshots.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "app.hpp"
#include "core/params.hpp"
#include "cloud.hpp"
#include "state_file.hpp"

namespace {

void usage() {
    std::puts(
        "cf_view: Vulkan viewer for Cellular Forms\n"
        "\n"
        "What to show (one of):\n"
        "  --run <dir>            every snapshot file in a run directory (scrub with the slider)\n"
        "  --snapshot <file>      one state or snapshot file (repeatable)\n"
        "  --demo <level>         an icosphere of the given subdivision level\n"
        "  --demo-steps <n>       run n CPU simulation steps on the demo first\n"
        "\n"
        "Live simulation (the viewer drives the simulation; parameters can be changed while it runs):\n"
        "  --live                 start a live simulation instead of showing files\n"
        "  --backend <cuda|cpu>   default cuda (shares Vulkan buffers with CUDA); cpu = the reference\n"
        "  --level <n> --seed <n> start state (icosphere level, random seed)\n"
        "  --max-cells <n>        capacity of the live buffers (default 2000000)\n"
        "  --param key=value      any simulation parameter, repeatable\n"
        "  --paused               start paused;  --step-limit <steps/s>\n"
        "  --live-selftest        scripted parameter changes, pause, single steps and reset (with --exit-after)\n"
        "\n"
        "Headless capture (no window; works with the screen locked):\n"
        "  --screenshot <out.png> render one frame offscreen and exit\n"
        "  --frame <index|all>    which file of the run (default: the last; 'all' writes out_0000.png ...)\n"
        "                         or a list such as 3,10,-1 (negative counts from the end)\n"
        "  --sheet [--sheet-columns <n>]  tile the captured frames into one PNG\n"
        "  --size <W>x<H>         image or window size (default 1600x1000)\n"
        "  --ssaa <1..4>          supersampling for captures (default 2)\n"
        "\n"
        "Look:\n"
        "  --mode <cells|xray|mesh|sss|path>  spheres (default), additive x-ray, the membrane mesh, the membrane\n"
        "                         with subsurface scattering (these two need a state with its rings: a\n"
        "                         checkpoint file or a live run), or path traced spheres (needs ray tracing)\n"
        "  --xray-density <x>     brightness of the x-ray mode (default 0.06)\n"
        "  --color <age|food|birth|uniform|generation|curvature|activator>\n"
        "                         curvature needs a state with its rings, activator one with morphogens\n"
        "  --set name=value       any render setting (repeatable): light, shadows, occlusion, cell walls,\n"
        "                         outlines, path tracer, tone mapping, ...;  --list-settings prints the names\n"
        "  --yaw <deg> --pitch <deg> --zoom <factor>\n"
        "  --radius <r>           cell radius in world units (default 0.55)\n"
        "  --no-ao                turn ambient occlusion off\n"
        "  --ao-radius <small> <large>\n"
        "\n"
        "Other:\n"
        "  --validate             enable the Vulkan validation layer; exit code 3 on any error\n"
        "  --no-vsync\n"
        "  --play [--play-fps <n>]  start with snapshot playback running\n"
        "  --exit-after <frames>  close the window after this many frames and print frame times\n"
        "  --window-capture <png> [--window-capture-frame <n>]  save the window, UI included, at frame n\n");
}

}  // namespace

int main(int argc, char** argv) {
    cfv::ViewOptions o;
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
            } else if (a == "--run") {
                const auto files = cfv::listRun(next());
                o.files.insert(o.files.end(), files.begin(), files.end());
            } else if (a == "--snapshot") {
                o.files.emplace_back(next());
            } else if (a == "--demo") {
                o.demoLevel = std::atoi(next());
            } else if (a == "--demo-steps") {
                o.demoSteps = std::atoi(next());
            } else if (a == "--screenshot") {
                o.screenshot = next();
            } else if (a == "--frame") {
                const std::string v = next();
                if (v == "all") {
                    o.allFrames = true;
                } else if (v.find(',') != std::string::npos) {
                    std::size_t at = 0;
                    while (at <= v.size()) {
                        const std::size_t comma = v.find(',', at);
                        const std::string item = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                        if (!item.empty()) o.frames.push_back(std::atoi(item.c_str()));
                        if (comma == std::string::npos) break;
                        at = comma + 1;
                    }
                } else {
                    o.frame = std::atoi(v.c_str());
                }
            } else if (a == "--sheet") {
                o.sheet = true;
            } else if (a == "--sheet-columns") {
                o.sheetColumns = std::atoi(next());
            } else if (a == "--size") {
                if (std::sscanf(next(), "%dx%d", &o.width, &o.height) != 2 || o.width < 16 || o.height < 16)
                    throw std::runtime_error("--size expects <W>x<H>");
            } else if (a == "--ssaa") {
                o.ssaa = std::atoi(next());
            } else if (a == "--color") {
                std::string v = next();
                if (v == "morph") v = "activator";
                const std::string err = cfv::setRenderSetting(o.settings, "color", v);
                if (!err.empty()) throw std::runtime_error("unknown colour mode " + v);
            } else if (a == "--set") {
                // Any render setting by name: --set shadows=on --set sunElevation=35 ...
                const std::string kv = next();
                const std::size_t eq = kv.find('=');
                if (eq == std::string::npos) throw std::runtime_error("--set expects name=value");
                const std::string err = cfv::setRenderSetting(o.settings, kv.substr(0, eq), kv.substr(eq + 1));
                if (!err.empty()) throw std::runtime_error("--set: " + err);
            } else if (a == "--list-settings") {
                for (const std::string& n : cfv::renderSettingNames()) std::printf("%s%s", n.c_str(), "\n");
                return 0;
            } else if (a == "--yaw") {
                o.yawDeg = static_cast<float>(std::atof(next()));
            } else if (a == "--pitch") {
                o.pitchDeg = static_cast<float>(std::atof(next()));
            } else if (a == "--zoom") {
                o.zoom = static_cast<float>(std::atof(next()));
            } else if (a == "--radius") {
                o.settings.sphereRadius = static_cast<float>(std::atof(next()));
            } else if (a == "--mode") {
                const std::string v = next();
                if (!cfv::setRenderSetting(o.settings, "drawMode", v).empty()) throw std::runtime_error("--mode expects cells, xray, mesh, sss or path");
            } else if (a == "--xray-density") {
                o.settings.xrayDensity = static_cast<float>(std::atof(next()));
            } else if (a == "--no-ao") {
                o.settings.aoEnabled = false;
            } else if (a == "--ao-radius") {
                o.settings.aoRadiusSmall = static_cast<float>(std::atof(next()));
                o.settings.aoRadiusLarge = static_cast<float>(std::atof(next()));
            } else if (a == "--validate") {
                o.validate = true;
            } else if (a == "--no-vsync") {
                o.vsync = false;
            } else if (a == "--play") {
                o.play = true;
            } else if (a == "--play-fps") {
                o.playFps = static_cast<float>(std::atof(next()));
            } else if (a == "--exit-after") {
                o.exitAfterFrames = std::atoi(next());
            } else if (a == "--live") {
                o.live = true;
            } else if (a == "--backend") {
                const std::string v = next();
                if (v == "cuda") o.liveCuda = true;
                else if (v == "cpu") o.liveCuda = false;
                else throw std::runtime_error("--backend expects cuda or cpu");
            } else if (a == "--level") {
                o.liveLevel = std::atoi(next());
            } else if (a == "--seed") {
                o.liveSeed = std::strtoull(next(), nullptr, 10);
            } else if (a == "--max-cells") {
                o.liveParams.maxCells = static_cast<std::uint32_t>(std::strtoull(next(), nullptr, 10));
            } else if (a == "--param") {
                const std::string err = cf::apply_text(o.liveParams, next());
                if (!err.empty()) throw std::runtime_error("--param: " + err);
            } else if (a == "--paused") {
                o.livePaused = true;
            } else if (a == "--step-limit") {
                o.liveStepLimit = std::atof(next());
            } else if (a == "--live-selftest") {
                o.live = true;
                o.liveSelfTest = true;
            } else if (a == "--window-capture") {
                o.windowCapture = next();
            } else if (a == "--window-capture-frame") {
                o.windowCaptureFrame = std::atoi(next());
            } else {
                throw std::runtime_error("unknown option " + a + " (try --help)");
            }
        }
        if (o.files.empty() && o.demoLevel < 0 && !o.live) o.demoLevel = 4;
        if (o.liveSelfTest && o.exitAfterFrames <= 0) o.exitAfterFrames = 2400;

        if (!o.screenshot.empty()) return cfv::runHeadless(o);
        return cfv::runWindowed(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cf_view: %s\n", e.what());
        return 1;
    }
}
