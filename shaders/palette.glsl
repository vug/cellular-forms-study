// Colour helpers shared by the cell and membrane shaders (needs frame.glsl).

// The 0..1 value a cell is coloured by, from the colour mode in F.colorParams. `extra` is the
// per-cell value of the modes that have no buffer of their own (generation, curvature, activator).
float cellScalar(uint birth, float food, float extra) {
    int mode = int(F.colorParams.x + 0.5);
    float s = 0.0;
    float span = max(F.colorParams.z - F.colorParams.y, 1e-6);
    if (mode == 1) s = (F.colorParams.w - float(birth) - F.colorParams.y) / span;        // age in steps
    else if (mode == 2) s = (food - F.colorParams.y) / span;                             // food
    else if (mode == 3) s = (float(birth) - F.colorParams.y) / span;                     // birth step
    else if (mode >= 4) s = (extra - F.colorParams.y) / span;                            // generation, curvature, activator
    s = clamp(s, 0.0, 1.0);
    return mode == 1 ? 1.0 - s : s;   // age: the youngest cells are the brightest
}

vec3 ramp5(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 c4, float t) {
    t = clamp(t, 0.0, 1.0) * 4.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    return mix(c3, c4, t - 3.0);
}

vec3 palette(float t) {
    if (F.material.w > 0.5) t = 1.0 - t;
    int p = int(F.material.z + 0.5);
    if (p == 1)  // purple through green to yellow (perceptually even)
        return ramp5(vec3(0.27, 0.00, 0.33), vec3(0.23, 0.32, 0.55), vec3(0.13, 0.57, 0.55), vec3(0.37, 0.79, 0.38), vec3(0.99, 0.91, 0.14), t);
    if (p == 2)  // black through purple to cream
        return ramp5(vec3(0.00, 0.00, 0.02), vec3(0.32, 0.07, 0.48), vec3(0.72, 0.21, 0.47), vec3(0.99, 0.53, 0.38), vec3(0.99, 0.99, 0.75), t);
    if (p == 3)  // diverging: blue, white, red (for signed values such as curvature)
        return ramp5(vec3(0.13, 0.27, 0.62), vec3(0.55, 0.72, 0.88), vec3(0.96, 0.96, 0.94), vec3(0.93, 0.62, 0.50), vec3(0.70, 0.09, 0.17), t);
    if (p == 4)  // grey
        return mix(vec3(0.08), vec3(0.98), clamp(t, 0.0, 1.0));
    // Default: dark blue -> teal -> warm yellow -> near white.
    return ramp5(vec3(0.10, 0.13, 0.32), vec3(0.10, 0.45, 0.55), vec3(0.55, 0.75, 0.40), vec3(0.98, 0.84, 0.35), vec3(1.00, 0.96, 0.86), t);
}

vec3 cellAlbedo(float scalar) {
    return int(F.colorParams.x + 0.5) == 0 ? F.baseColor.rgb : palette(scalar);
}
