// Per-frame uniforms shared by every pass. Keep in sync with FrameUniforms in
// src/viewer/renderer.hpp (std140, all vec4/mat4 so there is no padding to get wrong).
layout(set = 0, binding = 0, std140) uniform Frame {
    mat4 view;           // world -> view (right-handed, camera looks down -Z, +Y up)
    mat4 proj;           // view -> clip (Vulkan: y down, depth 0..1)
    mat4 invView;        // view -> world
    mat4 lightViewProj;  // world -> clip space of the shadow map
    vec4 viewport;       // width, height, 1/width, 1/height (pixels)
    vec4 sphere;         // radius (world), minimum radius (pixels), near, far
    vec4 colorParams;    // colour mode, scalar min, scalar max, current step
    vec4 light;          // view-space direction towards the key light (xyz), ambient share (w)
    vec4 lightColor;     // key light colour times intensity (rgb), shadow strength (a; 0 = no shadows)
    vec4 sunWorld;       // world-space direction towards the key light (xyz), its angular radius in radians (w; path tracer)
    vec4 sky;            // colour of the light from above (rgb), sky intensity in the path tracer (a)
    vec4 ground;         // colour of the light from below (rgb), sun intensity in the path tracer (a)
    vec4 shadow;         // normal offset (world units), softness (texels), 1 / map size, depth bias (clip units)
    vec4 ao;             // small radius (world), large radius (world), intensity, enabled
    vec4 rt;             // ray-traced occlusion: rays per pixel (0 = screen space), radius (world); path tracer: samples so far, samples this frame
    vec4 pt;             // path tracer: bounces, lens radius (world units), focus distance, unused
    vec4 baseColor;      // rgb for the "uniform" colour mode, a = exposure
    vec4 background;     // rgb top colour, a = vignette strength
    vec4 material;       // specular strength, shininess, palette, palette reversed
    vec4 wall;           // cell wall colour (rgb), strength (a; 0 = off)
    vec4 outline;        // outline colour (rgb), strength (a; 0 = off)
    vec4 style;          // wall width (pixels), outline width (pixels), outline threshold (relative depth step), toon bands (0 = off)
    vec4 env;            // environment map: light the form with it (0 or 1), intensity, rotation (radians), show it as the background (0 or 1)
    vec4 simLightDir;    // the simulation's food light: direction towards it (world, xyz), in use (w)
    vec4 simLightU;      // first axis of its shadow map (xyz), lower corner along it (w)
    vec4 simLightV;      // second axis (xyz), lower corner along it (w)
    vec4 simLightMap;    // texels per world unit, texels along U, texels along V, depth tolerance (world units)
    vec4 misc;           // frame index, debug view, draw mode (0 cells, 1 x-ray, 2 membrane, 3 membrane with scattering, 4 path traced), x-ray density scale or scatter radius
    vec4 misc2;          // tone map (0 none, 1 Reinhard, 2 filmic), background gradient, unused, blurriest level of the environment map
} F;
