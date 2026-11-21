// How a surface reflects light, shared by the light pass and the path tracer (needs frame.glsl):
// a mirror-like highlight from microfacets (GGX) over a matt base.
const float kPi = 3.14159265;

// Share of the light that is mirrored at an angle whose cosine is `cosine` (Schlick).
vec3 fresnel(vec3 f0, float cosine) {
    float m = clamp(1.0 - cosine, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// Density of the microfacets that face h (ndh: cosine between h and the normal).
float ggxD(float ndh, float a) {
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Shadowing among the microfacets, with the 1 / (4 ndl ndv) of the reflection folded in.
float ggxV(float ndl, float ndv, float a) {
    float a2 = a * a;
    float gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-6);
}

// Shadowing for one direction alone.
float ggxG1(float ndx, float a) { return 2.0 * ndx / (ndx + sqrt(a * a + (1.0 - a * a) * ndx * ndx)); }

// What a surface mirrors head-on: a few per cent for a non-metal, its own colour for a metal.
vec3 baseReflectance(vec3 albedo) { return mix(vec3(0.08 * F.matBase.z), albedo, F.matBase.y); }

// The matt colour: none for a metal, plus the sheen, a pale rim at grazing angles.
vec3 mattColor(vec3 albedo, float ndv) { return albedo * (1.0 - F.matBase.y) + F.matBase.w * pow(1.0 - clamp(ndv, 0.0, 1.0), 5.0); }

// The density (per solid angle) with which a mirrored ray picked from the microfacets that v
// sees leaves towards l.
float mirrorPdf(vec3 n, vec3 v, vec3 l, float a) {
    vec3 h = normalize(v + l);
    float ndv = max(dot(n, v), 1e-3);
    return ggxG1(ndv, a) * ggxD(max(dot(n, h), 0.0), a) / (4.0 * ndv);
}

float alphaOf(float roughness) {
    float r = clamp(roughness, 0.03, 1.0);
    return r * r;
}

// The highlight of a light from direction l with n, v (towards the eye) and l unit vectors:
// multiply by the light's colour. It is on the scale of the matt term `colour * ndl`.
vec3 highlight(vec3 n, vec3 v, vec3 l, vec3 f0, float a) {
    float ndl = dot(n, l);
    if (ndl <= 0.0) return vec3(0.0);
    vec3 h = normalize(l + v);
    float ndv = max(dot(n, v), 1e-3);
    return fresnel(f0, max(dot(v, h), 0.0)) * (kPi * ggxD(max(dot(n, h), 0.0), a) * ggxV(ndl, ndv, a) * ndl);
}
