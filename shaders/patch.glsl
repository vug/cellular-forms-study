// A triangle of the membrane refined into n * n smaller ones on a triangular lattice. A lattice
// point (r, c) is r rows from the first corner towards the third and c columns towards the
// second, 0 <= c <= n - r. The three corners weigh (n - r - c, c, r) / n at it.

// Number of a lattice point within its patch.
int patchVertex(int r, int c, int n) { return r * (n + 1) - (r * (r - 1)) / 2 + c; }

// The three lattice points of small triangle q (0 .. n * n - 1), wound like the large one. Row r
// has n - r upright and n - r - 1 inverted triangles, listed alternately.
void patchTriangle(int q, int n, out ivec2 a, out ivec2 b, out ivec2 c) {
    int r = 0;
    int count = 2 * n - 1;
    while (q >= count) {
        q -= count;
        count -= 2;
        r += 1;
    }
    int col = q / 2;
    if ((q & 1) == 0) {
        a = ivec2(r, col);
        b = ivec2(r, col + 1);
        c = ivec2(r + 1, col);
    } else {
        a = ivec2(r, col + 1);
        b = ivec2(r + 1, col + 1);
        c = ivec2(r + 1, col);
    }
}

vec3 patchWeights(ivec2 rc, int n) { return vec3(float(n - rc.x - rc.y), float(rc.y), float(rc.x)) / float(n); }
