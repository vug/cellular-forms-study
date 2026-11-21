"""The database of every run that was tried: start shape, seed, parameters, outcome, the
descriptors at every stage of its growth, its screenshots, and what Uğur said about it.

One SQLite file, <root>/atlas.db, next to the screenshots in <root>/img. Only the Python
standard library is used.
"""
import hashlib
import json
import os
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS runs (
    id INTEGER PRIMARY KEY,
    key TEXT UNIQUE,          -- hash of what defines the run: start shape, seed, parameters, target
    created REAL,
    origin TEXT,              -- what asked for it: survey, novelty, emitter, bred, ...
    parent INTEGER,           -- the run it was varied from, if any
    parent2 INTEGER,          -- the second parent of a cross
    init TEXT,                -- JSON: the start, for example {"shape": "icosphere:2", "food": "random"}
    seed INTEGER,
    params TEXT,              -- JSON: the simulation's key = value parameters
    genome TEXT,              -- JSON: the same in the search's own terms (base values and sources)
    code TEXT,                -- git commit of the tools that ran it
    status TEXT,              -- ok, stopped-early, stalled, broken
    stop_reason TEXT,
    detail TEXT,
    target_cells INTEGER,
    cells INTEGER,
    step INTEGER,
    wall_s REAL,
    stopped_at INTEGER,       -- the cell count at which the early stop ended it, if it did
    predicted REAL,           -- what the early-stop rule expected of it at its last decision
    cluster INTEGER,          -- the kind of form it was sorted into, if any
    map_x REAL,
    map_y REAL
);
CREATE TABLE IF NOT EXISTS stages (
    run INTEGER,
    stage INTEGER,            -- round(4 * log10(cells)): 12 = 1,000 cells, 16 = 10,000, 18 = 31,623
    cells INTEGER,
    step INTEGER,
    descriptors TEXT,         -- JSON, as cf_describe prints it
    PRIMARY KEY (run, stage)
);
CREATE TABLE IF NOT EXISTS images (run INTEGER, view TEXT, path TEXT, PRIMARY KEY (run, view));
CREATE TABLE IF NOT EXISTS labels (run INTEGER PRIMARY KEY, rating INTEGER, category TEXT, note TEXT, updated REAL);
CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX IF NOT EXISTS runs_origin ON runs (origin);
"""

RUN_JSON = ("init", "params", "genome")


def run_key(init, seed, params, target_cells):
    """What makes two runs the same run. Parameters are rounded as they are passed to the tools."""
    canonical = json.dumps({"init": init, "seed": int(seed), "target": int(target_cells), "params": {k: f"{float(v):.9g}" for k, v in sorted(params.items())}},
                           sort_keys=True)
    return hashlib.sha1(canonical.encode()).hexdigest()[:20]


class Store:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        os.makedirs(os.path.join(self.root, "img"), exist_ok=True)
        self.db = sqlite3.connect(os.path.join(self.root, "atlas.db"), timeout=30.0)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.commit()

    # ---- runs
    def find(self, key):
        row = self.db.execute("SELECT id FROM runs WHERE key = ?", (key,)).fetchone()
        return row["id"] if row else None

    def add_run(self, record, stages):
        """`record`: the columns of `runs` (JSON columns as Python values); `stages`: a list of
        descriptor dicts with 'stage', 'cells', 'step'. Returns the run's id."""
        columns = [c for c in record if c != "id"]
        values = [json.dumps(record[c]) if c in RUN_JSON and record[c] is not None else record[c] for c in columns]
        cursor = self.db.execute(f"INSERT INTO runs ({', '.join(columns)}) VALUES ({', '.join('?' for _ in columns)})", values)
        run = cursor.lastrowid
        for d in stages:
            self.db.execute("INSERT OR REPLACE INTO stages (run, stage, cells, step, descriptors) VALUES (?, ?, ?, ?, ?)",
                            (run, d["stage"], d["cells"], d["step"], json.dumps(d)))
        self.db.commit()
        return run

    def update_run(self, run, **columns):
        names = list(columns)
        self.db.execute(f"UPDATE runs SET {', '.join(n + ' = ?' for n in names)} WHERE id = ?", [columns[n] for n in names] + [run])

    def set_image(self, run, view, path):
        self.db.execute("INSERT OR REPLACE INTO images (run, view, path) VALUES (?, ?, ?)", (run, view, path))

    def commit(self):
        self.db.commit()

    def runs(self, where="", args=()):
        rows = self.db.execute("SELECT * FROM runs " + where, args).fetchall()
        out = []
        for row in rows:
            r = dict(row)
            for c in RUN_JSON:
                r[c] = json.loads(r[c]) if r[c] else None
            out.append(r)
        return out

    def count(self, where="", args=()):
        return self.db.execute("SELECT COUNT(*) FROM runs " + where, args).fetchone()[0]

    def stages(self, run):
        return [json.loads(row["descriptors"]) for row in self.db.execute("SELECT descriptors FROM stages WHERE run = ? ORDER BY stage", (run,))]

    def all_stages(self):
        """{run id: [descriptors by stage]} of every run, in one query."""
        out = {}
        for row in self.db.execute("SELECT run, descriptors FROM stages ORDER BY run, stage"):
            out.setdefault(row["run"], []).append(json.loads(row["descriptors"]))
        return out

    def images(self):
        out = {}
        for row in self.db.execute("SELECT run, view, path FROM images"):
            out.setdefault(row["run"], {})[row["view"]] = row["path"]
        return out

    # ---- what Uğur said
    def set_label(self, run, rating=None, category=None, note=None):
        old = self.db.execute("SELECT rating, category, note FROM labels WHERE run = ?", (run,)).fetchone()
        rating = rating if rating is not None else (old["rating"] if old else None)
        category = category if category is not None else (old["category"] if old else None)
        note = note if note is not None else (old["note"] if old else None)
        self.db.execute("INSERT OR REPLACE INTO labels (run, rating, category, note, updated) VALUES (?, ?, ?, ?, ?)", (run, rating, category, note, time.time()))
        self.db.commit()

    def labels(self):
        return {row["run"]: {"rating": row["rating"], "category": row["category"], "note": row["note"]} for row in self.db.execute("SELECT * FROM labels")}

    # ---- everything else that is kept: the spread between seeds, the early-stop rule, the kinds
    def meta(self, key, default=None):
        row = self.db.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
        return json.loads(row["value"]) if row else default

    def set_meta(self, key, value):
        self.db.execute("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)", (key, json.dumps(value)))
        self.db.commit()
