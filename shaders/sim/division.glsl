// Division on the rotation system (docs/model.md sections 6 and 7); GLSL versions of the
// functions in src/cuda/topo_kernels.hpp. Geometry and food are read from and written to the
// "new" buffers, as in the CPU step (the split runs after the buffers are swapped).
// Below, "vp" is the daughter cell (v-prime in the docs).

uint ringIndex(uint v, uint u) {
    uint base = v * kMaxDeg;
    uint d = deg[v];
    for (uint k = 0u; k < d; ++k)
        if (nbr[base + k] == u) return k;
    return kInvalid;
}

uint64_t divisionPriority(uint v) {
    uint64_t h = hashU64(pc.seed, pc.step, uint64_t(v), kStreamPriority) >> 33;  // 31 bits
    return ((h << 32) | uint64_t(v)) + 1ul;
}

bool canSplit(uint v, uint i, uint j) {
    uint d = deg[v];
    if (i == j || i >= d || j >= d) return false;
    uint jr = (j + d - i) % d;
    if (jr + 2u > kMaxDeg) return false;
    if (d - jr + 2u > kMaxDeg) return false;
    uint base = v * kMaxDeg;
    if (deg[nbr[base + i]] + 1u > kMaxDeg) return false;
    if (deg[nbr[base + j]] + 1u > kMaxDeg) return false;
    return true;
}

float cleavageLength(uint base, uint i, uint j, vec3 P) {
    precise float r = length3(LOAD3(posNew, nbr[base + i]) - P) + length3(LOAD3(posNew, nbr[base + j]) - P);
    return r;
}

// Packed (i << 4) | j, or kNoPair when no pair respects the degree cap.
uint chooseCleavagePair(uint v, uint64_t tieHash) {
    uint d = deg[v];
    uint base = v * kMaxDeg;
    uint offs0 = d / 2u, offs1 = (d + 1u) / 2u;
    uint nOffs = offs0 == offs1 ? 1u : 2u;
    uint tiesI[24];
    uint tiesJ[24];
    uint nTies = 0u;
    uint best = 0xFFFFFFFFu;
    for (uint i = 0u; i < d; ++i) {
        for (uint o = 0u; o < nOffs; ++o) {
            uint j = (i + (o == 0u ? offs0 : offs1)) % d;
            if (!canSplit(v, i, j)) continue;
            uint da = deg[nbr[base + i]], db = deg[nbr[base + j]];
            uint cost = max(da, db) * 100u + da + db;
            if (cost < best) {
                best = cost;
                nTies = 0u;
            }
            if (cost == best) {
                tiesI[nTies] = i;
                tiesJ[nTies] = j;
                ++nTies;
            }
        }
    }
    if (nTies == 0u) return kNoPair;
    uint pick = uint(tieHash % uint64_t(nTies));
    if (nTies > 1u) {
        // Ties: the pair with the shortest cleavage links, walking from the hashed choice.
        vec3 P = LOAD3(posNew, v);
        float bestLen = cleavageLength(base, tiesI[pick], tiesJ[pick], P);
        uint start = pick;
        for (uint k = 1u; k < nTies; ++k) {
            uint q = (start + k) % nTies;
            float len = cleavageLength(base, tiesI[q], tiesJ[q], P);
            if (len < bestLen) {
                bestLen = len;
                pick = q;
            }
        }
    }
    return (tiesI[pick] << 4) | tiesJ[pick];
}

void splitRings(uint v, uint i, uint j, uint vp) {
    uint d = deg[v];
    uint bv = v * kMaxDeg;
    uint rot[12];
    for (uint k = 0u; k < d; ++k) rot[k] = nbr[bv + (i + k) % d];
    uint jr = (j + d - i) % d;
    uint a = rot[0], b = rot[jr];

    // new ring(v) = K followed by vp = [a, ..., b, vp]
    for (uint k = 0u; k <= jr; ++k) nbr[bv + k] = rot[k];
    nbr[bv + jr + 1u] = vp;
    for (uint k = jr + 2u; k < kMaxDeg; ++k) nbr[bv + k] = kInvalid;
    deg[v] = jr + 2u;

    // new ring(vp) = M followed by v = [b, ..., a, v]
    uint bp = vp * kMaxDeg;
    uint m = 0u;
    for (uint k = jr; k < d; ++k) nbr[bp + m++] = rot[k];
    nbr[bp + m++] = a;
    nbr[bp + m++] = v;
    for (uint k = m; k < kMaxDeg; ++k) nbr[bp + k] = kInvalid;
    deg[vp] = m;

    // every w strictly inside M: replace v by vp in the same slot
    for (uint k = jr + 1u; k < d; ++k) {
        uint w = rot[k];
        uint bw = w * kMaxDeg;
        uint dw = deg[w];
        for (uint q = 0u; q < dw; ++q) {
            if (nbr[bw + q] == v) {
                nbr[bw + q] = vp;
                break;
            }
        }
    }
    // ring(a): insert vp immediately AFTER v.  ring(b): insert vp immediately BEFORE v.
    {
        uint slot = ringIndex(a, v) + 1u;
        uint ba = a * kMaxDeg;
        uint dc = deg[a];
        for (uint q = dc; q > slot; --q) nbr[ba + q] = nbr[ba + q - 1u];
        nbr[ba + slot] = vp;
        deg[a] = dc + 1u;
    }
    {
        uint slot = ringIndex(b, v);
        uint bb = b * kMaxDeg;
        uint dc = deg[b];
        for (uint q = dc; q > slot; --q) nbr[bb + q] = nbr[bb + q - 1u];
        nbr[bb + slot] = vp;
        deg[b] = dc + 1u;
    }
}

void applyDivision(uint v, uint pair, uint vp) {
    uint d = deg[v];
    uint i = pair >> 4, j = pair & 0xFu;
    uint jr = (j + d - i) % d;
    uint bv = v * kMaxDeg;
    uint a = nbr[bv + i], b = nbr[bv + j];
    vec3 P = LOAD3(posNew, v);
    vec3 N = LOAD3(nrmNew, v);

    // Direction towards the daughter: the centroid of the neighbours that move to it.
    vec3 dir = vec3(0.0);
    uint moved = d - jr - 1u;
    if (moved > 0u) {
        precise vec3 c = vec3(0.0);
        for (uint k = jr + 1u; k < d; ++k) c += LOAD3(posNew, nbr[bv + (i + k) % d]);
        precise float invMoved = 1.0 / float(moved);
        precise vec3 cm = c * invMoved;
        precise vec3 toC = cm - P;
        dir = unitOrZero(toC);
    }
    if (dot3(dir, dir) == 0.0) dir = unitOrZero(cross3(N, LOAD3(posNew, b) - LOAD3(posNew, a)));
    if (dot3(dir, dir) == 0.0) {
        float ax = abs(N.x), ay = abs(N.y), az = abs(N.z);
        vec3 axis = (ax <= ay && ax <= az) ? vec3(1.0, 0.0, 0.0) : (ay <= az ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
        dir = unitOrZero(cross3(N, axis));
    }
    if (dot3(dir, dir) == 0.0) dir = vec3(1.0, 0.0, 0.0);

    splitRings(v, i, j, vp);

    precise float delta = pc.daughterOffset * pc.l0;
    precise vec3 pd = P + delta * dir;
    precise vec3 pp = P - delta * dir;
    STORE3(posNew, vp, pd);
    STORE3(posNew, v, pp);
    STORE3(nrmNew, vp, N);
    precise float halfFood = 0.5 * foodNew[v];
    foodNew[v] = halfFood;
    foodNew[vp] = halfFood;
    birth[vp] = uint(pc.step);
    gen[v] += 1u;
    gen[vp] = gen[v];
}
