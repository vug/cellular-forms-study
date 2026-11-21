// Per-frame uniforms shared by every pass. Keep in sync with FrameUniforms in
// src/viewer/renderer.hpp (std140, all vec4/mat4 so there is no padding to get wrong).
layout(set = 0, binding = 0, std140) uniform Frame {
    mat4 view;         // world -> view (right-handed, camera looks down -Z, +Y up)
    mat4 proj;         // view -> clip (Vulkan: y down, depth 0..1)
    vec4 viewport;     // width, height, 1/width, 1/height (pixels)
    vec4 sphere;       // radius (world), minimum radius (pixels), near, far
    vec4 colorParams;  // colour mode, scalar min, scalar max, current step
    vec4 light;        // view-space direction towards the light (xyz), ambient (w)
    vec4 ao;           // small radius (world), large radius (world), intensity, enabled
    vec4 baseColor;    // rgb for the "uniform" colour mode, a = exposure
    vec4 background;   // rgb top colour, a = vignette strength
    vec4 misc;         // frame index, debug view, draw mode (0 cells, 1 x-ray, 2 membrane), x-ray density scale
} F;
