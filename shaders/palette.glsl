// Colour helpers shared by the cell and membrane shaders (needs frame.glsl).

// The 0..1 value a cell is coloured by, from the colour mode in F.colorParams.
float cellScalar(uint birth, float food) {
    int mode = int(F.colorParams.x + 0.5);
    float s = 0.0;
    float span = max(F.colorParams.z - F.colorParams.y, 1e-6);
    if (mode == 1) s = (F.colorParams.w - float(birth) - F.colorParams.y) / span;        // age in steps
    else if (mode == 2) s = (food - F.colorParams.y) / span;                             // food
    else if (mode == 3) s = (float(birth) - F.colorParams.y) / span;                     // birth step
    s = clamp(s, 0.0, 1.0);
    return mode == 1 ? 1.0 - s : s;   // age: the youngest cells are the brightest
}

vec3 palette(float t) {
    // Dark blue -> teal -> warm yellow -> near white.
    const vec3 c0 = vec3(0.10, 0.13, 0.32);
    const vec3 c1 = vec3(0.10, 0.45, 0.55);
    const vec3 c2 = vec3(0.55, 0.75, 0.40);
    const vec3 c3 = vec3(0.98, 0.84, 0.35);
    const vec3 c4 = vec3(1.00, 0.96, 0.86);
    t = clamp(t, 0.0, 1.0) * 4.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    return mix(c3, c4, t - 3.0);
}

vec3 cellAlbedo(float scalar) {
    return int(F.colorParams.x + 0.5) == 0 ? F.baseColor.rgb : palette(scalar);
}
