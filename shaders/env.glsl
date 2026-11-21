// The environment map (needs frame.glsl and screen.glsl): an equirectangular image of the
// surroundings in linear colour, used as the light from all around and, optionally, as the
// background. F.env: use it for light, intensity, rotation about the vertical, show it behind
// the form.
layout(set = 0, binding = 8) uniform sampler2D texEnv;

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

#ifdef ENV_SAMPLING
// Tables for picking directions where the map is bright (the path tracer): a map with a small
// bright lamp lights the form through very few of its texels, and directions picked evenly
// would almost never find them. The tables are made from a small copy of the map, weighted by
// the area a texel covers on the sphere (the sine of the angle from the vertical).
layout(set = 0, binding = 9) uniform sampler2D texEnvTable;  // r: share of the row's light up to and including this column; g: the texel's light
layout(set = 0, binding = 10) uniform sampler2D texEnvRows;  // one column: share of all the light up to and including this row

// A direction (world space) picked in proportion to the light from it, from four random
// numbers. Returns the light from there; `pdf` is the density of the pick per solid angle.
vec3 envSample(vec4 u, out vec3 dir, out float pdf) {
    int w = int(F.envSample.z), h = int(F.envSample.w);
    int lo = 0, hi = h - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (texelFetch(texEnvRows, ivec2(0, mid), 0).r < u.x) lo = mid + 1;
        else hi = mid;
    }
    int row = lo;
    lo = 0;
    hi = w - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (texelFetch(texEnvTable, ivec2(mid, row), 0).r < u.y) lo = mid + 1;
        else hi = mid;
    }
    float weight = texelFetch(texEnvTable, ivec2(lo, row), 0).g;
    vec2 uv = (vec2(lo, row) + u.zw) / vec2(w, h);
    float theta = uv.y * 3.14159265, phi = (uv.x - 0.5) * 6.2831853;
    float sinTheta = sin(theta);
    vec3 d = vec3(sinTheta * sin(phi), cos(theta), sinTheta * cos(phi));
    float c = cos(F.env.z), s = sin(F.env.z);
    dir = vec3(c * d.x - s * d.z, d.y, s * d.x + c * d.z);  // the rotation of envLight, undone
    pdf = weight / (F.envSample2.x * 19.7392088 * max(sinTheta, 1e-4));
    return textureLod(texEnv, uv, F.envSample.y).rgb * F.env.y;
}

// The density with which envSample picks the direction `dir`.
float envPdf(vec3 dir) {
    float c = cos(F.env.z), s = sin(F.env.z);
    vec3 d = vec3(c * dir.x + s * dir.z, dir.y, c * dir.z - s * dir.x);
    vec2 uv = vec2(atan(d.x, d.z) * 0.15915494 + 0.5, acos(clamp(d.y, -1.0, 1.0)) * 0.31830989);
    ivec2 size = ivec2(F.envSample.zw);
    ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
    return texelFetch(texEnvTable, texel, 0).g / (F.envSample2.x * 19.7392088 * max(sin(uv.y * 3.14159265), 1e-4));
}
#endif
