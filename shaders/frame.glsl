// Per-frame uniforms shared by every pass. Keep in sync with FrameUniforms in
// src/viewer/renderer.hpp (std140, all vec4/mat4 so there is no padding to get wrong).
layout(set = 0, binding = 0, std140) uniform Frame {
    mat4 view;           // world -> view (right-handed, camera looks down -Z, +Y up)
    mat4 proj;           // view -> clip (Vulkan: y down, depth 0..1)
    mat4 invView;        // view -> world
    mat4 lightViewProj;  // world -> clip space of the shadow map
    vec4 viewport;       // width, height, 1/width, 1/height of the images being rendered (pixels)
    vec4 sphere;         // radius (world), minimum radius (pixels), near, far
    vec4 colorParams;    // colour mode (a cell property, see palette.glsl), scalar min, scalar max, current step
    vec4 light;          // view-space direction towards the key light (xyz), ambient share (w)
    vec4 lightColor;     // key light colour times intensity (rgb), shadow strength (a; 0 = no shadows)
    vec4 sunWorld;       // world-space direction towards the key light (xyz), its angular radius in radians (w; path tracer)
    vec4 sky;            // colour of the light from above (rgb), sky intensity in the path tracer (a)
    vec4 ground;         // colour of the light from below (rgb), sun intensity in the path tracer (a)
    vec4 shadow;         // normal offset (world units), softness (texels), 1 / map size, depth bias (clip units)
    vec4 shadow2;        // depth of the box of the light in world units, 1 when the shadow map has been drawn, unused, unused
    vec4 ao;             // small radius (world), large radius (world), intensity, enabled
    vec4 rt;             // traced occlusion: rays per pixel (0 = screen space), ray length (world); path tracer: samples so far, samples this frame
    vec4 pt;             // path tracer: bounces, the most a bounced ray may bring (0 = no cap), unused, unused
    vec4 lens;           // lens radius (world units; 0 = everything sharp), focus distance, largest blur radius (pixels), unused
    vec4 baseColor;      // rgb for the "uniform" colour mode, a = exposure
    vec4 background;     // rgb top colour, a = vignette strength
    vec4 palette;        // palette, reversed, unused, unused
    vec4 matBase;        // roughness, metalness, specular, sheen
    vec4 matSss;         // subsurface amount, scatter radius (world), translucency, depth the light reaches through (world)
    vec4 matSssColor;    // how far red, green and blue travel, relative to the scatter radius; unused
    vec4 matEmission;    // emission tint (rgb), strength (a)
    vec4 matEmission2;   // share of the tint against the cell colour, emissive cells light their neighbours (0 or 1), unused, unused
    vec4 modSource;      // the cell property that drives emission, roughness, subsurface, translucency (0: none)
    vec4 modLo;          // value of each where the property is lowest
    vec4 modHi;          // ... and where it is highest
    vec4 modFrom;        // the part of the property's range (0 .. 1) over which each goes from low to high: its start
    vec4 modTo;          // ... and its end
    vec4 propMin0;       // ranges of the cell properties: age, food, birth step, generation
    vec4 propMin1;       // ... curvature, activator
    vec4 propMax0;
    vec4 propMax1;
    vec4 wall;           // cell wall colour (rgb), strength (a; 0 = off)
    vec4 outline;        // outline colour (rgb), strength (a; 0 = off)
    vec4 style;          // wall width (pixels), outline width (pixels), outline threshold (relative depth step), toon bands (0 = off)
    vec4 env;            // environment map: light the form with it (0 or 1), intensity, rotation (radians), show it as the background (0 or 1)
    vec4 envSample;      // blurriest level, the level sampled light is read from, width and height of its sampling table
    vec4 envSample2;     // mean of luminance times sin(theta) over the sampling table, unused, unused, unused
    vec4 simLightDir;    // the food light of the simulation: direction towards it (world, xyz), in use (w)
    vec4 simLightU;      // first axis of its shadow map (xyz), lower corner along it (w)
    vec4 simLightV;      // second axis (xyz), lower corner along it (w)
    vec4 simLightMap;    // texels per world unit, texels along U, texels along V, depth tolerance (world units)
    vec4 bloom;          // strength, threshold, unused, unused
    vec4 aa;             // seed of the noise of this frame (0 while the image is not being accumulated), fast edge smoothing on, unused, unused
    vec4 mesh;           // membrane patches: segments per edge, vertices per patch, triangles per patch, unused
    vec4 misc;           // frame index, debug view, draw mode (0 cells, 1 x-ray, 2 membrane, 4 cells path traced, 5 membrane path traced), x-ray density scale
    vec4 misc2;          // tone map (0 none, 1 Reinhard, 2 filmic, 3 soft), background gradient, unused, unused
} F;
