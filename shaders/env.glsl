// The environment map (needs frame.glsl and screen.glsl): an equirectangular image of the
// surroundings in linear colour, used as the light from all around and, optionally, as the
// background. F.env: use it for light, intensity, rotation about the vertical, show it behind
// the form.
layout(set = 0, binding = 7) uniform sampler2D texEnv;

// Light arriving from direction `dir` (world space). Higher `lod` levels are blurrier: a
// surface gathers light from a wide cone, a mirror from a narrow one.
vec3 envLight(vec3 dir, float lod) {
    float c = cos(F.env.z), s = sin(F.env.z);
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, c * dir.z - s * dir.x);
    vec2 uv = vec2(atan(d.x, d.z) * 0.15915494 + 0.5, acos(clamp(d.y, -1.0, 1.0)) * 0.31830989);
    return textureLod(texEnv, uv, lod).rgb * F.env.y;
}

// What is behind the form at `uv`: the environment, or the plain background.
vec3 sceneBackground(vec2 uv) {
    if (F.env.w < 0.5) return backgroundColor(uv);
    return envLight(mat3(F.invView) * normalize(viewRay(uv)), 0.0);
}
