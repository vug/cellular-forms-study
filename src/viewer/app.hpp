// The viewer application: options, the headless capture path and the interactive window.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "core/params.hpp"
#include "math.hpp"
#include "renderer.hpp"

namespace cfv {

struct ViewOptions {
    // What to show.
    std::vector<std::filesystem::path> files;  // state/snapshot files, in run order
    int demoLevel = -1;                        // >= 0: icosphere of this subdivision level
    int demoSteps = 0;                         // CPU simulation steps to run on the demo

    // Headless capture: when `screenshot` is set, render and exit without opening a window.
    std::filesystem::path screenshot;
    int frame = -1;         // index into `files`; -1 = the last one
    bool allFrames = false; // capture every file as <stem>_<index>.png
    std::vector<int> frames; // several indices (negative counts from the end); with `sheet`, one tiled image
    bool sheet = false;      // tile the captured frames into a single PNG, row by row
    int sheetColumns = 0;    // 0 = as square as possible
    int width = 1600;
    int height = 1000;
    int ssaa = 2;
    // A check of the blending of frames on a form that moves (see runHeadless): this many frames.
    int motionTest = 0;

    // Camera (degrees; zoom < 1 moves closer). Unset values keep the automatic framing.
    float yawDeg = 35.0f;
    float pitchDeg = 20.0f;
    float zoom = 1.0f;

    RenderSettings settings;
    bool validate = false;
    bool noRayTracing = false;   // do not use ray tracing even when the GPU has it (to try the fallbacks)
    bool noBarycentric = false;  // nor fragment-shader barycentrics (cell walls on the membrane)
    bool vsync = true;

    // Automated runs of the window (tests and measurements).
    bool play = false;        // start with playback on
    float playFps = 8.0f;     // snapshots per second during playback
    float spin = 0.0f;        // turn the camera around the form, degrees per second (0: off)
    int exitAfterFrames = 0;  // > 0: close after this many frames and print frame-time statistics
    std::filesystem::path windowCapture;  // write the window contents (with the UI) to this PNG ...
    int windowCaptureFrame = 30;          // ... at this frame

    // Live mode: the viewer drives a simulation instead of showing files.
    bool live = false;
    bool liveCuda = true;         // CUDA backend (falls back to the CPU reference when it is not available)
    int liveLevel = 2;            // icosphere level of the start state
    std::uint64_t liveSeed = 1;
    cf::Params liveParams;        // maxCells is the capacity of the live buffers
    bool livePaused = false;      // start paused
    double liveStepLimit = 0.0;   // steps per second, 0 = unlimited
    // Scripted run of the live controls (parameter changes, pause, single steps, restart) for
    // unattended testing; needs exitAfterFrames. The exit code is non-zero if anything broke.
    bool liveSelfTest = false;
};

// Renders the requested frame(s) offscreen and writes PNG files. Returns a process exit code.
int runHeadless(const ViewOptions& options);

// Opens the window and runs until it is closed. Returns a process exit code.
int runWindowed(const ViewOptions& options);

}  // namespace cfv
