// Helpers for the full-screen passes (needs frame.glsl).

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Direction of the view ray through `uv`, scaled so that z is -1 (view space). The third column
// of the projection is not zero for a stereo eye, whose frustum is skewed.
vec3 viewRay(vec2 uv) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + F.proj[2][0]) / F.proj[0][0], (ndc.y + F.proj[2][1]) / F.proj[1][1], -1.0);
}

// View-space position of the surface at `uv` with linear depth `depth`.
vec3 viewPos(vec2 uv, float depth) { return viewRay(uv) * depth; }

// Behind the form: a vertical gradient with a soft vignette.
vec3 backgroundColor(vec2 uv) {
    vec3 top = F.background.rgb;
    vec3 bottom = top * (1.0 - F.misc2.y);
    vec3 c = mix(top, bottom, uv.y);
    vec2 q = uv - 0.5;
    return c * (1.0 - F.background.a * dot(q, q));
}

vec3 toneMap(vec3 c) {
    int op = int(F.misc2.x + 0.5);
    if (op == 1) return c / (1.0 + c);
    if (op == 2) return clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    return c;
}
