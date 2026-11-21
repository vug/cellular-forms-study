// Cell properties, colours and materials, shared by the cell and membrane shaders and the path
// tracer (needs frame.glsl). A cell brings its birth step, its food, and three more values:
// generation, curvature and morphogen activator (`extra`).

// The raw value of a property: 1 age, 2 food, 3 birth step, 4 generation, 5 curvature, 6 activator.
float cellRaw(int which, uint birth, float food, vec3 extra) {
    if (which == 1) return F.colorParams.w - float(birth);
    if (which == 2) return food;
    if (which == 3) return float(birth);
    if (which == 4) return extra.x;
    if (which == 5) return extra.y;
    if (which == 6) return extra.z;
    return 0.0;
}

// The 0..1 value a cell is coloured by, from the colour mode and range in F.colorParams.
float cellScalar(uint birth, float food, vec3 extra) {
    int mode = int(F.colorParams.x + 0.5);
    float s = clamp((cellRaw(mode, birth, food, extra) - F.colorParams.y) / max(F.colorParams.z - F.colorParams.y, 1e-6), 0.0, 1.0);
    return mode == 1 ? 1.0 - s : s;  // age: the youngest cells are the brightest
}

// A property as 0..1 over its usual range (for age: 1 is the youngest).
float cellProperty(int which, uint birth, float food, vec3 extra) {
    if (which < 1 || which > 6) return 0.0;
    float lo = which <= 4 ? F.propMin0[which - 1] : F.propMin1[which - 5];
    float hi = which <= 4 ? F.propMax0[which - 1] : F.propMax1[which - 5];
    float s = clamp((cellRaw(which, birth, food, extra) - lo) / max(hi - lo, 1e-6), 0.0, 1.0);
    return which == 1 ? 1.0 - s : s;
}

// What varies from cell to cell in the material, each 0..1: how much of the emission the cell
// gives off, its roughness, how much it scatters light under the surface, how much light it
// lets through. Each is the setting of the material, or follows a cell property.
vec4 cellMaterial(uint birth, float food, vec3 extra) {
    vec4 m = vec4(1.0, F.matBase.x, F.matSss.x, F.matSss.z);
    for (int i = 0; i < 4; ++i) {
        int source = int(F.modSource[i] + 0.5);
        if (source == 0) continue;
        float t = cellProperty(source, birth, food, extra);
        m[i] = mix(F.modLo[i], F.modHi[i], clamp((t - F.modFrom[i]) / max(F.modTo[i] - F.modFrom[i], 1e-4), 0.0, 1.0));
    }
    return clamp(m, 0.0, 1.0);
}

vec3 ramp5(vec3 c0, vec3 c1, vec3 c2, vec3 c3, vec3 c4, float t) {
    t = clamp(t, 0.0, 1.0) * 4.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    return mix(c3, c4, t - 3.0);
}

vec3 palette(float t) {
    if (F.palette.y > 0.5) t = 1.0 - t;
    int p = int(F.palette.x + 0.5);
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

// The light a cell gives off: its own colour or the emission tint, times the strength.
vec3 cellEmission(vec3 albedo, float amount) { return mix(albedo, F.matEmission.rgb, F.matEmission2.x) * (F.matEmission.a * amount); }
