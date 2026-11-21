#include "topology.hpp"

#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

#include "hash.hpp"
#include "kernels.hpp"

namespace cf {

// ============================================================================ construction

State state_from_faces(const std::vector<Vec3>& positions,
                       const std::vector<std::array<std::uint32_t, 3>>& faces) {
    const std::uint32_t n = static_cast<std::uint32_t>(positions.size());
    if (n < 4) throw std::runtime_error("state_from_faces: need at least 4 cells");

    // For every cell v, each incident face (v, a, b) says: in ring(v), a is followed by b.
    std::vector<std::vector<std::pair<std::uint32_t, std::uint32_t>>> next(n);
    for (const auto& f : faces) {
        for (std::size_t c = 0; c < 3; ++c) {
            const std::uint32_t v = f[c], a = f[(c + 1) % 3], b = f[(c + 2) % 3];
            if (v >= n || a >= n || b >= n) throw std::runtime_error("state_from_faces: face index out of range");
            if (v == a || a == b || v == b) throw std::runtime_error("state_from_faces: degenerate face");
            next[v].emplace_back(a, b);
        }
    }

    State s;
    s.resize(n);
    s.pos = positions;
    for (std::uint32_t v = 0; v < n; ++v) {
        const auto& nx = next[v];
        const std::size_t d = nx.size();
        if (d < 3) throw std::runtime_error("state_from_faces: cell " + std::to_string(v) + " has fewer than 3 faces");
        if (d > kMaxDeg) throw std::runtime_error("state_from_faces: cell " + std::to_string(v) + " needs more than kMaxDeg slots");
        // Start at the smallest neighbour id so the ring has one canonical rotation.
        std::uint32_t cur = nx[0].first;
        for (const auto& e : nx) cur = e.first < cur ? e.first : cur;
        std::uint32_t* r = s.ring(v);
        for (std::size_t k = 0; k < d; ++k) {
            r[k] = cur;
            std::uint32_t following = kInvalid;
            int matches = 0;
            for (const auto& e : nx) {
                if (e.first == cur) {
                    following = e.second;
                    ++matches;
                }
            }
            if (matches == 0) throw std::runtime_error("state_from_faces: open fan at cell " + std::to_string(v) + " (mesh is not closed)");
            if (matches > 1) throw std::runtime_error("state_from_faces: edge shared by more than two faces at cell " + std::to_string(v));
            cur = following;
        }
        if (cur != r[0]) throw std::runtime_error("state_from_faces: faces around cell " + std::to_string(v) + " do not form one cycle");
        s.deg[v] = static_cast<std::uint8_t>(d);
    }

    for (std::uint32_t v = 0; v < n; ++v) {
        Vec3 nrm;
        // eps2 = 0: only an exactly zero (or NaN) sum counts as degenerate for a freshly built mesh.
        if (!ring_normal(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], Vec3{0.0f, 0.0f, 0.0f}, 0.0f, nrm))
            throw std::runtime_error("state_from_faces: degenerate ring at cell " + std::to_string(v));
        s.normal[v] = nrm;
    }
    const CheckResult c = check_invariants(s);
    if (!c.ok()) throw std::runtime_error("state_from_faces: " + c.message);
    return s;
}

State make_tetrahedron() {
    const std::vector<Vec3> p = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
    // Each face counter-clockwise seen from outside.
    const std::vector<std::array<std::uint32_t, 3>> f = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
    return state_from_faces(p, f);
}

State make_octahedron() {
    const std::vector<Vec3> p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const std::vector<std::array<std::uint32_t, 3>> f = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                                          {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    return state_from_faces(p, f);
}

State make_icosphere(int level, const Params& params) {
    if (level < 0 || level > 9) throw std::runtime_error("make_icosphere: level must be 0..9");
    struct D3 {
        double x, y, z;
    };
    auto normalised = [](D3 a) {
        const double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        return D3{a.x / l, a.y / l, a.z / l};
    };
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    // Same vertex and face tables as docs/reference/split_check.py.
    std::vector<D3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                         {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (D3& a : v) a = normalised(a);
    std::vector<std::array<std::uint32_t, 3>> f = {
        {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
        {3, 9, 4},  {3, 4, 2}, {3, 2, 6}, {3, 6, 8},  {3, 8, 9},   {4, 9, 5}, {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};

    for (int l = 0; l < level; ++l) {
        std::map<std::uint64_t, std::uint32_t> mid;
        auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
            const std::uint64_t key = a < b ? (static_cast<std::uint64_t>(a) << 32) | b : (static_cast<std::uint64_t>(b) << 32) | a;
            const auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            const std::uint32_t id = static_cast<std::uint32_t>(v.size());
            v.push_back(normalised(D3{v[a].x + v[b].x, v[a].y + v[b].y, v[a].z + v[b].z}));
            mid.emplace(key, id);
            return id;
        };
        std::vector<std::array<std::uint32_t, 3>> g;
        g.reserve(f.size() * 4);
        for (const auto& tri : f) {
            const std::uint32_t a = tri[0], b = tri[1], c = tri[2];
            const std::uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            g.push_back({a, ab, ca});
            g.push_back({b, bc, ab});
            g.push_back({c, ca, bc});
            g.push_back({ab, bc, ca});
        }
        f.swap(g);
    }

    // Make sure every face is counter-clockwise seen from outside (it is for the table above;
    // this guards against edits).
    for (auto& tri : f) {
        const D3 a = v[tri[0]], b = v[tri[1]], c = v[tri[2]];
        const D3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
        const D3 nrm{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        if (nrm.x * (a.x + b.x + c.x) + nrm.y * (a.y + b.y + c.y) + nrm.z * (a.z + b.z + c.z) < 0.0) std::swap(tri[1], tri[2]);
    }

    // Scale so that the mean link length is linkRestLength.
    double sum = 0.0;
    std::size_t count = 0;
    for (const auto& tri : f) {
        for (std::size_t c = 0; c < 3; ++c) {
            const D3 a = v[tri[c]], b = v[tri[(c + 1) % 3]];
            sum += std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            ++count;
        }
    }
    const double scale = static_cast<double>(params.linkRestLength) / (sum / static_cast<double>(count));
    std::vector<Vec3> pos(v.size());
    for (std::size_t k = 0; k < v.size(); ++k)
        pos[k] = Vec3{static_cast<float>(v[k].x * scale), static_cast<float>(v[k].y * scale), static_cast<float>(v[k].z * scale)};

    State s = state_from_faces(pos, f);
    s.params = params;
    return s;
}

namespace {

// Rescales positions so the mean link length is the rest length, then recomputes the normals.
void finish_shape(State& s, const char* what) {
    double sum = 0.0;
    std::uint64_t links = 0;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        for (std::uint32_t k = 0; k < s.deg[v]; ++k) {
            sum += length(s.pos[v] - s.pos[s.ring(v)[k]]);
            ++links;
        }
    }
    const float scale = static_cast<float>(static_cast<double>(s.params.linkRestLength) / (sum / static_cast<double>(links)));
    for (Vec3& p : s.pos) p = p * scale;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        Vec3 nrm;
        if (!ring_normal(s.pos.data(), s.ring(v), s.deg[v], s.pos[v], Vec3{0.0f, 0.0f, 0.0f}, 0.0f, nrm))
            throw std::runtime_error(std::string(what) + ": degenerate ring at cell " + std::to_string(v));
        s.normal[v] = nrm;
    }
    const CheckResult c = check_invariants(s);
    if (!c.ok()) throw std::runtime_error(std::string(what) + ": " + c.message);
}

// Smooth value noise in [-1, 1] from hashed lattice values (trilinear, smoothstep weights).
float value_noise(Vec3 p, std::uint64_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const float tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    const float ux = tx * tx * (3.0f - 2.0f * tx), uy = ty * ty * (3.0f - 2.0f * ty), uz = tz * tz * (3.0f - 2.0f * tz);
    const std::int64_t ix = static_cast<std::int64_t>(fx), iy = static_cast<std::int64_t>(fy), iz = static_cast<std::int64_t>(fz);
    auto corner = [&](std::int64_t dx, std::int64_t dy, std::int64_t dz) {
        std::uint64_t h = mix64(seed ^ 0xA24BAED4963EE407ull);
        h = mix64(h ^ static_cast<std::uint64_t>(ix + dx));
        h = mix64(h ^ static_cast<std::uint64_t>(iy + dy));
        h = mix64(h ^ static_cast<std::uint64_t>(iz + dz));
        return static_cast<float>(h >> 40) * (2.0f / 16777216.0f) - 1.0f;
    };
    auto mixf = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = mixf(corner(0, 0, 0), corner(1, 0, 0), ux), x10 = mixf(corner(0, 1, 0), corner(1, 1, 0), ux);
    const float x01 = mixf(corner(0, 0, 1), corner(1, 0, 1), ux), x11 = mixf(corner(0, 1, 1), corner(1, 1, 1), ux);
    return mixf(mixf(x00, x10, uy), mixf(x01, x11, uy), uz);
}

}  // namespace

State make_ellipsoid(int level, const Params& params, float ax, float ay, float az) {
    if (!(ax > 0.0f && ay > 0.0f && az > 0.0f) || !is_finite(Vec3{ax, ay, az})) throw std::runtime_error("make_ellipsoid: axes must be finite and > 0");
    State s = make_icosphere(level, params);
    for (Vec3& p : s.pos) p = Vec3{p.x * ax, p.y * ay, p.z * az};
    finish_shape(s, "make_ellipsoid");
    return s;
}

State make_noisy_sphere(int level, const Params& params, float amplitude, float frequency, std::uint64_t seed) {
    if (!(amplitude >= 0.0f && amplitude < 1.0f)) throw std::runtime_error("make_noisy_sphere: amplitude must be in [0, 1)");
    if (!(frequency > 0.0f) || !is_finite(frequency)) throw std::runtime_error("make_noisy_sphere: frequency must be finite and > 0");
    State s = make_icosphere(level, params);
    for (Vec3& p : s.pos) {
        const Vec3 dir = unit_or_zero(p);
        p = p * (1.0f + amplitude * value_noise(dir * frequency, seed));
    }
    finish_shape(s, "make_noisy_sphere");
    return s;
}

// ============================================================================ checker

const char* invariant_name(Invariant i) {
    switch (i) {
        case Invariant::Ok: return "ok";
        case Invariant::ArraySizes: return "array-sizes";
        case Invariant::IndexRange: return "index-range";
        case Invariant::Degree: return "degree";
        case Invariant::SelfLink: return "self-link";
        case Invariant::Duplicate: return "duplicate";
        case Invariant::Symmetry: return "symmetry";
        case Invariant::RingNotLinked: return "ring-not-linked";
        case Invariant::Orientation: return "orientation";
        case Invariant::Euler: return "euler";
        case Invariant::Connected: return "connected";
        case Invariant::NonFinite: return "non-finite";
    }
    return "?";
}

namespace {

CheckResult fail(Invariant what, std::uint32_t cell, std::uint32_t other, std::string msg) {
    CheckResult r;
    r.failed = what;
    r.cell = cell;
    r.other = other;
    r.message = std::string("invariant '") + invariant_name(what) + "' broken: " + std::move(msg);
    return r;
}

}  // namespace

CheckResult check_invariants(const State& s) {
    const std::uint32_t n = s.n;
    const auto str = [](std::uint64_t x) { return std::to_string(x); };

    if (n < 4 || s.pos.size() != n || s.normal.size() != n || s.food.size() != n || s.birth.size() != n ||
        s.gen.size() != n || s.deg.size() != n || s.nbr.size() != static_cast<std::size_t>(n) * kMaxDeg)
        return fail(Invariant::ArraySizes, kInvalid, kInvalid, "array lengths do not match cell count " + str(n));

    // Check 2 (and index range, so the later lookups are safe).
    for (std::uint32_t v = 0; v < n; ++v) {
        const std::uint32_t d = s.deg[v];
        if (d < 3 || d > kMaxDeg) return fail(Invariant::Degree, v, kInvalid, "cell " + str(v) + " has degree " + str(d));
        const std::uint32_t* r = s.ring(v);
        for (std::uint32_t k = 0; k < d; ++k) {
            if (r[k] >= n) return fail(Invariant::IndexRange, v, r[k], "cell " + str(v) + " slot " + str(k) + " holds id " + str(r[k]));
            if (r[k] == v) return fail(Invariant::SelfLink, v, v, "cell " + str(v) + " lists itself");
            for (std::uint32_t l = k + 1; l < d; ++l)
                if (r[k] == r[l]) return fail(Invariant::Duplicate, v, r[k], "cell " + str(v) + " lists " + str(r[k]) + " twice");
        }
    }

    // Checks 1 and 3.
    for (std::uint32_t v = 0; v < n; ++v) {
        const std::uint32_t d = s.deg[v];
        const std::uint32_t* r = s.ring(v);
        for (std::uint32_t k = 0; k < d; ++k) {
            const std::uint32_t a = r[k], b = r[(k + 1) % d];
            if (ring_index(s, a, v) < 0) return fail(Invariant::Symmetry, v, a, "cell " + str(v) + " lists " + str(a) + " but not the reverse");
            const int ib = ring_index(s, a, b);
            if (ib < 0)
                return fail(Invariant::RingNotLinked, v, a,
                            "consecutive neighbours " + str(a) + " and " + str(b) + " of cell " + str(v) + " are not linked");
            const std::uint32_t after = s.ring(a)[(static_cast<std::uint32_t>(ib) + 1) % s.deg[a]];
            if (after != v)
                return fail(Invariant::Orientation, v, a,
                            "in ring(" + str(a) + ") the entry after " + str(b) + " is " + str(after) + ", expected " + str(v));
        }
    }

    // Check 4.
    std::uint64_t degSum = 0;
    for (std::uint32_t v = 0; v < n; ++v) degSum += s.deg[v];
    if (degSum != 6ull * n - 12ull)
        return fail(Invariant::Euler, kInvalid, kInvalid, "sum of degrees is " + str(degSum) + ", expected 6V-12 = " + str(6ull * n - 12ull));

    // Check 5.
    {
        std::vector<std::uint8_t> seen(n, 0);
        std::vector<std::uint32_t> queue;
        queue.reserve(n);
        queue.push_back(0);
        seen[0] = 1;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const std::uint32_t x = queue[head];
            const std::uint32_t* r = s.ring(x);
            for (std::uint32_t k = 0; k < s.deg[x]; ++k) {
                if (!seen[r[k]]) {
                    seen[r[k]] = 1;
                    queue.push_back(r[k]);
                }
            }
        }
        if (queue.size() != n) {
            std::uint32_t firstMissing = 0;
            while (firstMissing < n && seen[firstMissing]) ++firstMissing;
            return fail(Invariant::Connected, firstMissing, kInvalid,
                        "only " + str(queue.size()) + " of " + str(n) + " cells reachable from cell 0; cell " + str(firstMissing) + " is not");
        }
    }

    // Check 6.
    for (std::uint32_t v = 0; v < n; ++v) {
        if (!is_finite(s.pos[v])) return fail(Invariant::NonFinite, v, kInvalid, "position of cell " + str(v) + " is not finite");
        if (!is_finite(s.normal[v])) return fail(Invariant::NonFinite, v, kInvalid, "normal of cell " + str(v) + " is not finite");
        if (!std::isfinite(s.food[v])) return fail(Invariant::NonFinite, v, kInvalid, "food of cell " + str(v) + " is not finite");
    }
    return {};
}

// ============================================================================ split

bool can_split(const State& s, std::uint32_t v, std::uint32_t i, std::uint32_t j) {
    const std::uint32_t d = s.deg[v];
    if (i == j || i >= d || j >= d) return false;
    const std::uint32_t jr = (j + d - i) % d;  // position of j after rotating i to the front
    if (jr + 2 > kMaxDeg) return false;        // new ring(v)  = K + v'
    if (d - jr + 2 > kMaxDeg) return false;    // new ring(v') = M + v
    const std::uint32_t* r = s.ring(v);
    if (s.deg[r[i]] + 1u > kMaxDeg) return false;
    if (s.deg[r[j]] + 1u > kMaxDeg) return false;
    return true;
}

void split_rings(State& s, std::uint32_t v, std::uint32_t i, std::uint32_t j, std::uint32_t vp) {
    const std::uint32_t d = s.deg[v];
    std::uint32_t rot[kMaxDeg];
    {
        const std::uint32_t* r = s.ring(v);
        for (std::uint32_t k = 0; k < d; ++k) rot[k] = r[(i + k) % d];
    }
    const std::uint32_t jr = (j + d - i) % d;
    const std::uint32_t a = rot[0], b = rot[jr];

    // new ring(v) = K followed by v' = [a, ..., b, v']
    {
        std::uint32_t* r = s.ring(v);
        for (std::uint32_t k = 0; k <= jr; ++k) r[k] = rot[k];
        r[jr + 1] = vp;
        for (std::uint32_t k = jr + 2; k < kMaxDeg; ++k) r[k] = kInvalid;
        s.deg[v] = static_cast<std::uint8_t>(jr + 2);
    }
    // new ring(v') = M followed by v = [b, ..., a, v]
    {
        std::uint32_t* r = s.ring(vp);
        std::uint32_t m = 0;
        for (std::uint32_t k = jr; k < d; ++k) r[m++] = rot[k];
        r[m++] = a;
        r[m++] = v;
        for (std::uint32_t k = m; k < kMaxDeg; ++k) r[k] = kInvalid;
        s.deg[vp] = static_cast<std::uint8_t>(m);
    }
    // every w strictly inside M: replace v by v' in the same slot
    for (std::uint32_t k = jr + 1; k < d; ++k) {
        const std::uint32_t w = rot[k];
        std::uint32_t* r = s.ring(w);
        for (std::uint32_t q = 0; q < s.deg[w]; ++q) {
            if (r[q] == v) {
                r[q] = vp;
                break;
            }
        }
    }
    // ring(a): insert v' immediately AFTER v.  ring(b): insert v' immediately BEFORE v.
    auto insert_at = [&](std::uint32_t cell, std::uint32_t slot) {
        std::uint32_t* r = s.ring(cell);
        const std::uint32_t dc = s.deg[cell];
        for (std::uint32_t q = dc; q > slot; --q) r[q] = r[q - 1];
        r[slot] = vp;
        s.deg[cell] = static_cast<std::uint8_t>(dc + 1);
    };
    insert_at(a, static_cast<std::uint32_t>(ring_index(s, a, v)) + 1u);
    insert_at(b, static_cast<std::uint32_t>(ring_index(s, b, v)));
}

CleavagePair choose_cleavage_pair(const State& s, std::uint32_t v, std::uint64_t tieHash, const Vec3* pos, const Vec3* normal,
                                  const Orientation* orient) {
    const std::uint32_t d = s.deg[v];
    const std::uint32_t* r = s.ring(v);
    const std::uint32_t offs[2] = {d / 2, (d + 1) / 2};
    const std::uint32_t nOffs = offs[0] == offs[1] ? 1u : 2u;

    // Every near-opposite pair inside the degree cap, with its degree cost.
    std::uint8_t allI[2 * kMaxDeg], allJ[2 * kMaxDeg];
    std::uint32_t allCost[2 * kMaxDeg];
    std::uint32_t nAll = 0;
    std::uint32_t best = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < d; ++i) {
        for (std::uint32_t o = 0; o < nOffs; ++o) {
            const std::uint32_t j = (i + offs[o]) % d;
            if (!can_split(s, v, i, j)) continue;
            const std::uint32_t da = s.deg[r[i]], db = s.deg[r[j]];
            const std::uint32_t cost = (da > db ? da : db) * 100u + da + db;
            allI[nAll] = static_cast<std::uint8_t>(i);
            allJ[nAll] = static_cast<std::uint8_t>(j);
            allCost[nAll] = cost;
            ++nAll;
            if (cost < best) best = cost;
        }
    }
    CleavagePair p;
    if (nAll == 0) return p;

    // The pairs to choose among: the best degree cost, or (oriented division) close to it.
    const bool oriented = orient != nullptr && orient->amount > 0.0f && pos != nullptr && normal != nullptr;
    const std::uint32_t margin = oriented ? static_cast<std::uint32_t>(orient->amount * 200.0f + 0.5f) : 0u;
    std::uint8_t tiesI[2 * kMaxDeg], tiesJ[2 * kMaxDeg];
    std::uint32_t nTies = 0;
    for (std::uint32_t k = 0; k < nAll; ++k) {
        if (allCost[k] <= best + margin) {
            tiesI[nTies] = allI[k];
            tiesJ[nTies] = allJ[k];
            ++nTies;
        }
    }

    // Start from the hashed choice and walk the candidates from there, keeping a strictly better
    // one only (so exact ties stay hashed).
    std::uint32_t pick = static_cast<std::uint32_t>(tieHash % nTies);
    if (oriented && nTies > 1) {
        // The daughter is placed across the line a-b, in the surface: steer that along the axis.
        const Vec3 N = normal[v];
        const std::uint32_t start = pick;
        float bestAlign = std::fabs(dot(unit_or_zero(cross(N, pos[r[tiesJ[pick]]] - pos[r[tiesI[pick]]])), orient->axis));
        for (std::uint32_t k = 1; k < nTies; ++k) {
            const std::uint32_t q = (start + k) % nTies;
            const float align = std::fabs(dot(unit_or_zero(cross(N, pos[r[tiesJ[q]]] - pos[r[tiesI[q]]])), orient->axis));
            if (align > bestAlign) {
                bestAlign = align;
                pick = q;
            }
        }
    } else if (pos != nullptr && nTies > 1) {
        // The pair with the shortest cleavage links.
        const Vec3 P = pos[v];
        float bestLen = length(pos[r[tiesI[pick]]] - P) + length(pos[r[tiesJ[pick]]] - P);
        const std::uint32_t start = pick;
        for (std::uint32_t k = 1; k < nTies; ++k) {
            const std::uint32_t q = (start + k) % nTies;
            const float len = length(pos[r[tiesI[q]]] - P) + length(pos[r[tiesJ[q]]] - P);
            if (len < bestLen) {
                bestLen = len;
                pick = q;
            }
        }
    }
    p.valid = true;
    p.i = tiesI[pick];
    p.j = tiesJ[pick];
    return p;
}

}  // namespace cf
