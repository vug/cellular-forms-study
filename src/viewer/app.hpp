// The viewer application: options, the headless capture path and the interactive window.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

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

    // Camera (degrees; zoom < 1 moves closer). Unset values keep the automatic framing.
    float yawDeg = 35.0f;
    float pitchDeg = 20.0f;
    float zoom = 1.0f;

    RenderSettings settings;
    bool validate = false;
    bool vsync = true;

    // Automated runs of the window (tests and measurements).
    bool play = false;        // start with playback on
    float playFps = 8.0f;     // snapshots per second during playback
    int exitAfterFrames = 0;  // > 0: close after this many frames and print frame-time statistics
    std::filesystem::path windowCapture;  // write the window contents (with the UI) to this PNG ...
    int windowCaptureFrame = 30;          // ... at this frame
};

// Renders the requested frame(s) offscreen and writes PNG files. Returns a process exit code.
int runHeadless(const ViewOptions& options);

// Opens the window and runs until it is closed. Returns a process exit code.
int runWindowed(const ViewOptions& options);

}  // namespace cfv
