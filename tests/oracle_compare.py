"""Cross-check the C++ vertex split against docs/reference/split_check.py, ring for ring.

Usage: python3 tests/oracle_compare.py <path to cf_oracle> [work dir]

For icosphere levels 0 and 1: take the C++ start rings, apply random splits with the Python
reference (respecting the 12-slot cap), replay the same (v, i, j) sequence in C++, and compare
every ring exactly. Both sides also run their own invariant checker.
"""
import os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "docs", "reference"))
from split_check import split, check  # noqa: E402

MAX_DEG = 12


def read_rings(path):
    with open(path) as f:
        n = int(f.readline())
        rings = []
        for _ in range(n):
            parts = [int(x) for x in f.readline().split()]
            assert parts[0] == len(parts) - 1
            rings.append(parts[1:])
    return rings


def allowed(R, v, i, j):
    d = len(R[v])
    jr = (j - i) % d
    return (jr + 2 <= MAX_DEG and d - jr + 2 <= MAX_DEG
            and len(R[R[v][i]]) + 1 <= MAX_DEG and len(R[R[v][j]]) + 1 <= MAX_DEG)


def run(tool, work, level, splits, seed, balanced):
    start = os.path.join(work, f"start_{level}.txt")
    subprocess.run([tool, "dump-rings", str(level), start], check=True)
    R = read_rings(start)
    check(R)
    random.seed(seed)
    seq = []
    while len(seq) < splits:
        v = random.randrange(len(R))
        d = len(R[v])
        i = random.randrange(d)
        if balanced:
            j = (i + random.choice([d // 2, (d + 1) // 2])) % d
        else:
            j = random.choice([x for x in range(d) if x != i])
        if j == i or not allowed(R, v, i, j):
            continue
        split(R, v, i, j)
        seq.append((v, i, j))
    check(R)
    seq_path = os.path.join(work, f"seq_{level}_{seed}.txt")
    with open(seq_path, "w") as f:
        for v, i, j in seq:
            f.write(f"{v} {i} {j}\n")
    out = os.path.join(work, f"final_{level}_{seed}.txt")
    subprocess.run([tool, "apply-splits", str(level), seq_path, out], check=True)
    C = read_rings(out)
    assert len(C) == len(R), (len(C), len(R))
    for v, (rc, rp) in enumerate(zip(C, R)):
        assert rc == rp, f"ring {v} differs: C++ {rc} vs Python {rp}"
    kind = "near-opposite" if balanced else "arbitrary"
    print(f"level {level}: {splits} {kind} splits, {len(R)} cells, all rings identical, max degree {max(map(len, R))}")


def main():
    tool = sys.argv[1]
    work = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="cf_oracle_")
    os.makedirs(work, exist_ok=True)
    run(tool, work, 0, 20000, 1, True)
    run(tool, work, 1, 20000, 2, True)
    run(tool, work, 0, 8000, 3, False)
    print("oracle comparison passed")


if __name__ == "__main__":
    main()
