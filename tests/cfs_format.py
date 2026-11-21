"""Independent Python reader for Cellular Forms state files (layout: src/core/io.hpp).

    from cfs_format import load
    f = load("runs/x/latest.cfs")     # f.kind, f.n, f.step, f.seed, f.params, f.pos, ...
Checkpoints give pos, normal, food, birth, gen, deg, nbr (n x slots, 0xFFFFFFFF = unused).
Snapshots give pos (dequantised), food (0..1), gen, birth.
Needs numpy. The file checksum (FNV-1a 64) is verified unless verify=False.
"""
import struct
from types import SimpleNamespace

import numpy as np

MAGIC = b"CFSTATE\0"
HEADER = struct.Struct("<8sIIIIQQQ")  # magic, version, endian, kind, slots, n, step, seed
CHUNK = struct.Struct("<4sIQ")


def fnv1a64(data: bytes) -> int:
    # Vectorised FNV-1a is awkward; fold in blocks with Python ints (fine for test-sized files).
    h = 0xCBF29CE484222325
    mask = 0xFFFFFFFFFFFFFFFF
    prime = 0x100000001B3
    for b in data:
        h = ((h ^ b) * prime) & mask
    return h


def load(path, verify=True):
    with open(path, "rb") as fh:
        data = fh.read()
    magic, version, endian, kind, slots, n, step, seed = HEADER.unpack_from(data, 0)
    if magic != MAGIC:
        raise ValueError("bad magic")
    if version != 1 or endian != 0x01020304:
        raise ValueError(f"unsupported version {version} / byte order")
    out = SimpleNamespace(kind={1: "checkpoint", 2: "snapshot"}[kind], slots=slots, n=n, step=step, seed=seed, params={}, chunks=[])
    off = HEADER.size
    ended = False
    while off < len(data):
        tag, elem, size = CHUNK.unpack_from(data, off)
        body = off + CHUNK.size
        if body + size > len(data):
            raise ValueError("truncated chunk")
        payload = data[body:body + size]
        out.chunks.append(tag.decode())
        if tag == b"END ":
            if verify and struct.unpack("<Q", payload)[0] != fnv1a64(data[:off]):
                raise ValueError("checksum mismatch")
            ended = True
            break
        if tag == b"PARM":
            for line in payload.decode().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    out.params[k] = float(v)
        elif tag == b"POS ":
            out.pos = np.frombuffer(payload, "<f4").reshape(n, 3)
        elif tag == b"NRM ":
            out.normal = np.frombuffer(payload, "<f4").reshape(n, 3)
        elif tag == b"FOOD":
            out.food = np.frombuffer(payload, "<f4")
        elif tag == b"BRTH":
            out.birth = np.frombuffer(payload, "<u4")
        elif tag == b"GEN ":
            out.gen = np.frombuffer(payload, "<u4")
        elif tag == b"DEG ":
            out.deg = np.frombuffer(payload, "u1")
        elif tag == b"NBR ":
            out.nbr = np.frombuffer(payload, "<u4").reshape(n, slots)
        elif tag == b"POSQ":
            bounds = np.frombuffer(payload[:24], "<f4")
            q = np.frombuffer(payload[24:], "<u2").reshape(n, 3).astype(np.float64)
            lo, hi = bounds[:3].astype(np.float64), bounds[3:].astype(np.float64)
            out.bounds = (lo, hi)
            out.pos = (lo + q * (hi - lo) / 65535.0).astype(np.float32)
        elif tag == b"FODQ":
            out.food = np.frombuffer(payload, "u1").astype(np.float32) / 255.0
        elif tag == b"GENB":
            out.gen = np.frombuffer(payload, "u1").astype(np.uint32)
        off = body + size
    if not ended:
        raise ValueError("no end chunk")
    return out


def links(f):
    """Undirected links of a checkpoint as an (m, 2) array of cell ids (u < v)."""
    v = np.repeat(np.arange(f.n, dtype=np.int64), f.slots).reshape(f.n, f.slots)
    used = np.arange(f.slots)[None, :] < f.deg[:, None]
    u = f.nbr.astype(np.int64)
    keep = used & (u > v)
    return np.stack([v[keep], u[keep]], axis=1)
