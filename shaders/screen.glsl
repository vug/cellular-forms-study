// Helpers for the full-screen passes (needs frame.glsl).

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// A random number for this pixel. While the image is being accumulated over frames every frame
// gets its own (F.aa.x), so the noise of the sampled effects averages out; otherwise it stays
// the same, so a single image does not flicker.
float pixelNoise(float salt) { return hash12(gl_FragCoord.xy + vec2(salt * 17.13, salt * 3.71) + F.aa.x * vec2(5.3, 9.7)); }

// Direction of the view ray through `uv`, scaled so that z is -1 (view space). The third column
// of the projection is not zero for a stereo eye (a skewed frustum) or a jittered frame.
vec3 viewRay(vec2 uv) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3((ndc.x + F.proj[2][0]) / F.proj[0][0], (ndc.y + F.proj[2][1]) / F.proj[1][1], -1.0);
}

// View-space position of the surface at `uv` with linear depth `depth`.
vec3 viewPos(vec2 uv, float depth) { return viewRay(uv) * depth; }

// Pixels per world unit at a depth.
float pixelsPerUnit(float depth) { return 0.5 * F.viewport.y * abs(F.proj[1][1]) / depth; }

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
    if (op == 3) {
        // Soft: unchanged up to 0.8, then eased towards 1 by the brightest channel, which keeps
        // the hue of a glow; what is many times too bright turns pale, as a hot light does.
        float m = max(c.r, max(c.g, c.b));
        if (m <= 0.8) return c;
        float eased = 0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2));
        return mix(c * (eased / m), vec3(eased), 0.7 * smoothstep(1.5, 8.0, m));
    }
    return c;
}

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
