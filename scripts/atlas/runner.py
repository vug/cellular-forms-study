"""Grows forms for the atlas: one simulation after the other on the GPU (cf_gpu), described at
every stage of growth (cf_describe, on other cores while the next form grows), photographed in
batches (cf_view) and entered in the store.

A stage is a quarter of a decade of cells: stage = round(4 * log10(cells)), so 12 is 1,000
cells, 14 is 3,162, 16 is 10,000 and 18 is 31,623. With a stop rule (stop.py) a run is grown
stage by stage and may be ended at a decision stage when nothing is expected of it.

Only the Python standard library is used.
"""
import concurrent.futures
import glob
import json
import math
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import explore  # noqa: E402

from . import space
from .store import run_key

STAGE_FACTOR = 10.0 ** 0.25
VIEWS = {"a": [], "b": ["--yaw", "215", "--pitch", "-20"]}
THUMB = "320x240"
# Besides its two screenshots every run has a tile in a contact sheet (view a), eight across:
# the page that is published shows those, since it cannot carry a file per run.
TILE = "240x180"
SHEET_COLUMNS = 8
# ... and a smaller tile in a second set of sheets, for long lists on a phone.
SHEETS = {"tile": TILE, "thumb": "120x90"}
LOOK = ["--mode", "mesh", "--color", "uniform", "--set", "shadows=on"]


def stage_of(cells):
    return int(round(4.0 * math.log10(max(cells, 1))))


def cells_of(stage):
    return int(round(10.0 ** (stage / 4.0)))


class Job:
    """One run to make: a genome (or bare parameters), a seed, and where it came from."""

    def __init__(self, genome, seed=1, origin="", parent=None, parent2=None, target=31623):
        self.genome = genome
        self.init = dict(genome.get("init") or space.DEFAULT_INIT)
        self.seed = int(seed)
        self.params = space.to_params(genome)
        self.origin = origin
        self.parent = parent
        self.parent2 = parent2
        self.target = int(target)
        self.key = run_key(self.init, self.seed, self.params, self.target)


class Runner:
    def __init__(self, store, max_steps=60000, max_seconds=180.0, workers=6, keep_states=False, stop_rule=None, photos=True):
        self.store = store
        # Every process works in a folder of its own; what a process that died left behind is
        # cleared away once it is six hours old.
        base = os.path.join(store.root, "work")
        os.makedirs(base, exist_ok=True)
        for name in os.listdir(base):
            path = os.path.join(base, name)
            try:
                if time.time() - os.path.getmtime(path) > 6 * 3600:
                    shutil.rmtree(path, ignore_errors=True)
            except OSError:
                pass
        self.work = os.path.join(base, f"p{os.getpid()}")
        os.makedirs(self.work, exist_ok=True)
        self.max_steps = max_steps
        self.max_seconds = max_seconds
        self.keep_states = keep_states
        self.stop_rule = stop_rule
        self.photos = photos
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=workers)
        self.code = explore.git_version()
        self.sequence = 0

    # ---- one simulation
    def _param_args(self, params):
        out = []
        for k, v in params.items():
            out += ["--param", f"{k}={v:.9g}"]
        return out

    def _start_args(self, job, run_dir):
        """How cf_gpu gets its start state: an icosphere directly; any other shape through a
        state file that cf_run makes (the same state on either tool)."""
        shape, food = job.init["shape"], job.init.get("food", "random")
        if shape.startswith("icosphere:"):
            return ["--new", shape, "--seed", str(job.seed), "--init-food", food]
        start = os.path.join(run_dir, "start")
        os.makedirs(start, exist_ok=True)
        made = subprocess.run([explore.tool("cf_run"), "--new", shape, "--seed", str(job.seed), "--init-food", food, "--steps", "0", "--out", start]
                              + self._param_args(job.params), capture_output=True, text=True)
        if made.returncode != 0:
            raise RuntimeError("cf_run could not make the start shape " + shape + ": " + (made.stderr or made.stdout)[-300:])
        return ["--load", os.path.join(start, "latest.cfs")]

    def _simulate(self, job, run_dir, until_cells, resume):
        """One cf_gpu run. Exit code 1 is bad usage, a file or the GPU, not the simulation (a
        file that Windows would not let go of, once in thousands of runs): tried once more, and
        if it fails again the run is not entered as anything."""
        summary = self._simulate_once(job, run_dir, until_cells, resume)
        if summary["exit_code"] == 1:
            time.sleep(0.5)
            summary = self._simulate_once(job, run_dir, until_cells, resume)
        if summary["exit_code"] == 1:
            raise RuntimeError("cf_gpu could not run: " + summary["tail"].strip()[-240:])
        return summary

    def _simulate_once(self, job, run_dir, until_cells, resume):
        start = ["--load", os.path.join(run_dir, "latest.cfs")] if resume else self._start_args(job, run_dir)
        cmd = [explore.tool("cf_gpu")] + start + ["--capacity", str(int(job.target * 1.5) + 4096), "--until-cells", str(until_cells), "--until-step", str(self.max_steps),
                                                  "--max-seconds", str(self.max_seconds), "--out", run_dir, "--snapshot-factor", f"{STAGE_FACTOR:.9g}",
                                                  "--checkpoint-factor", f"{STAGE_FACTOR:.9g}"] + self._param_args(job.params)
        t0 = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True)
        summary = explore.parse_summary(proc.stdout)
        summary["wall"] = time.time() - t0
        summary["exit_code"] = proc.returncode
        summary["tail"] = (proc.stderr or proc.stdout)[-400:]
        return summary

    def _describe(self, run_dir, with_latest):
        """Descriptors of every checkpoint of the run so far, by stage (the last file of a stage wins)."""
        files = sorted(glob.glob(os.path.join(run_dir, "ckpt_c*.cfs")))
        if with_latest and os.path.exists(os.path.join(run_dir, "latest.cfs")):
            files.append(os.path.join(run_dir, "latest.cfs"))
        if not files:
            return [], ""
        proc = subprocess.run([explore.tool("cf_describe")] + files, capture_output=True, text=True)
        by_stage = {}
        problem = ""
        for line in proc.stdout.splitlines():
            if not line.startswith("{"):
                continue
            d = json.loads(line)
            if "error" in d:
                problem = d["error"]
                continue
            d["stage"] = stage_of(d["cells"])
            by_stage[d["stage"]] = d
        return [by_stage[k] for k in sorted(by_stage)], problem

    def _status(self, summary, until_cells):
        if summary["exit_code"] != 0 or summary.get("invariants") != "ok":
            return "broken"
        if summary.get("stop_reason") != "until-cells":
            return "stalled" if summary.get("stop_reason") in ("time-budget", "until-step", "steps") else str(summary.get("stop_reason"))
        return "ok"

    def _grow(self, job, run_dir):
        """Grows one form. Returns (record, pending): `pending` finishes the describing, on
        another core, when the run did not need its descriptors to decide anything."""
        record = {"key": job.key, "created": time.time(), "origin": job.origin, "parent": job.parent, "parent2": job.parent2, "init": job.init, "seed": job.seed,
                  "params": job.params, "genome": job.genome, "code": self.code, "target_cells": job.target, "stopped_at": None, "predicted": None}
        wall = 0.0
        if self.stop_rule is None:
            summary = self._simulate(job, run_dir, job.target, False)
            wall = summary["wall"]
            record["status"] = self._status(summary, job.target)
            stages = None
        else:
            # Stage by stage, up to each decision; the rule sees the descriptors so far.
            stages, resume = [], False
            decisions = [s for s in self.stop_rule.decision_stages() if cells_of(s) < job.target] + [None]
            for decision in decisions:
                until = job.target if decision is None else cells_of(decision)
                summary = self._simulate(job, run_dir, until, resume)
                wall += summary["wall"]
                resume = True
                record["status"] = self._status(summary, until)
                if record["status"] != "ok" or decision is None:
                    break
                stages, problem = self._describe(run_dir, True)
                if problem:
                    record["status"], summary["tail"] = "broken", problem
                    break
                verdict = self.stop_rule.decide(stages, decision)
                record["predicted"] = verdict["expected"]
                if verdict["stop"]:
                    record["status"], record["stopped_at"] = "stopped-early", summary.get("cells")
                    break
        record.update({"stop_reason": str(summary.get("stop_reason")), "cells": summary.get("cells"), "step": summary.get("step"), "wall_s": round(wall, 3),
                       "detail": summary["tail"] if record["status"] == "broken" else None})
        return record

    # ---- many runs
    def run_many(self, jobs, on_done=None, batch=48):
        """Runs the jobs that are not in the store yet. `on_done(run id, record, stages)` is
        called for each as it is entered. Returns the ids, in the order of the jobs (None for a
        job that could not be started)."""
        ids = [None] * len(jobs)
        waiting = []  # (index, job, run_dir, record, future of its descriptors)

        def flush(everything):
            nonlocal waiting
            if not waiting or (not everything and len(waiting) < batch):
                return
            entered = []
            for index, job, run_dir, record, future in waiting:
                stages, problem = future.result()
                if problem and record["status"] != "broken":
                    record["status"], record["detail"] = "broken", problem
                run = self.store.add_run(record, stages)
                ids[index] = run
                entered.append((run, run_dir, record, stages))
            if self.photos:
                self._photograph([(run, run_dir) for run, run_dir, record, stages in entered if os.path.exists(os.path.join(run_dir, "latest.cfs"))])
            for run, run_dir, record, stages in entered:
                if not self.keep_states:
                    shutil.rmtree(run_dir, ignore_errors=True)
                if on_done:
                    on_done(run, record, stages)
            self.store.commit()
            waiting = []

        for index, job in enumerate(jobs):
            known = self.store.find(job.key)
            if known is not None:
                ids[index] = known
                continue
            self.sequence += 1
            run_dir = os.path.join(self.work, f"r{self.sequence:06d}")
            shutil.rmtree(run_dir, ignore_errors=True)
            os.makedirs(run_dir, exist_ok=True)
            try:
                record = self._grow(job, run_dir)
            except RuntimeError as e:
                print("atlas:", e, flush=True)
                shutil.rmtree(run_dir, ignore_errors=True)
                continue
            waiting.append((index, job, run_dir, record, self.pool.submit(self._describe, run_dir, True)))
            flush(False)
        flush(True)
        return ids

    def _photograph(self, states):
        """Two screenshots of every state, one cf_view process per view for the whole batch."""
        if not states:
            return
        out_dir = os.path.join(self.work, "photos")
        for view, extra in VIEWS.items():
            shutil.rmtree(out_dir, ignore_errors=True)
            os.makedirs(out_dir, exist_ok=True)
            cmd = [explore.tool("cf_view")]
            for run, run_dir in states:
                cmd += ["--snapshot", os.path.join(run_dir, "latest.cfs")]
            cmd += ["--frame", "all", "--size", THUMB, "--screenshot", os.path.join(out_dir, "t.png")] + LOOK + extra
            proc = subprocess.run(cmd, capture_output=True, text=True)
            if proc.returncode != 0:
                print("atlas: cf_view failed:", (proc.stderr or proc.stdout)[-300:], flush=True)
                continue
            for k, (run, run_dir) in enumerate(states):
                made = os.path.join(out_dir, "t.png" if len(states) == 1 else f"t_{k:04d}.png")
                if os.path.exists(made):
                    name = f"{run:06d}_{view}.png"
                    shutil.move(made, os.path.join(self.store.root, "img", name))
                    self.store.set_image(run, view, "img/" + name)
        # The contact sheets, 48 tiles each, in two sizes.
        for first in range(0, len(states), 6 * SHEET_COLUMNS):
            part = states[first:first + 6 * SHEET_COLUMNS]
            for kind, size in SHEETS.items():
                cmd = [explore.tool("cf_view")]
                for run, run_dir in part:
                    cmd += ["--snapshot", os.path.join(run_dir, "latest.cfs")]
                name = f"{'sheet' if kind == 'tile' else 'thumbs'}_{part[0][0]:06d}.jpg"  # JPEG: a tenth of the size of a PNG
                cmd += ["--frame", "all", "--sheet", "--sheet-columns", str(SHEET_COLUMNS), "--size", size, "--screenshot", os.path.join(self.store.root, "img", name)] + LOOK
                proc = subprocess.run(cmd, capture_output=True, text=True)
                if proc.returncode != 0:
                    print("atlas: cf_view failed:", (proc.stderr or proc.stdout)[-300:], flush=True)
                    continue
                for k, (run, run_dir) in enumerate(part):
                    self.store.set_image(run, kind, f"img/{name}#{k % SHEET_COLUMNS},{k // SHEET_COLUMNS}")
        shutil.rmtree(out_dir, ignore_errors=True)

    def contact_sheet(self, runs, out_png, columns=4, size=THUMB):
        """One picture of the given runs (run dicts), row by row: each is grown again and drawn."""
        states = []
        for r in runs:
            job = Job.__new__(Job)
            job.init, job.seed, job.params, job.target = r["init"], r["seed"], r["params"], r["target_cells"]
            self.sequence += 1
            run_dir = os.path.join(self.work, f"c{self.sequence:06d}")
            os.makedirs(run_dir, exist_ok=True)
            self._simulate(job, run_dir, r["stopped_at"] or r["target_cells"], False)
            if os.path.exists(os.path.join(run_dir, "latest.cfs")):
                states.append(run_dir)
        cmd = [explore.tool("cf_view")]
        for run_dir in states:
            cmd += ["--snapshot", os.path.join(run_dir, "latest.cfs")]
        cmd += ["--frame", "all", "--sheet", "--sheet-columns", str(columns), "--size", size, "--screenshot", out_png] + LOOK
        proc = subprocess.run(cmd, capture_output=True, text=True)
        for run_dir in states:
            shutil.rmtree(run_dir, ignore_errors=True)
        if proc.returncode != 0:
            raise RuntimeError("cf_view failed: " + (proc.stderr or proc.stdout)[-300:])
        return len(states)

    def retake(self, runs, batch=48):
        """Screenshots of runs that are in the store already: each is grown again (the same
        parameters and seed give the same form) and photographed. `runs`: run dicts."""
        pending = []

        def flush():
            self._photograph(pending)
            for run, run_dir in pending:
                shutil.rmtree(run_dir, ignore_errors=True)
            self.store.commit()
            pending.clear()

        for r in runs:
            job = Job.__new__(Job)  # (only what growing needs: the start, the seed, the parameters as they were passed)
            job.init, job.seed, job.params, job.target = r["init"], r["seed"], r["params"], r["target_cells"]
            self.sequence += 1
            run_dir = os.path.join(self.work, f"s{self.sequence:06d}")
            os.makedirs(run_dir, exist_ok=True)
            until = r["stopped_at"] or r["target_cells"]
            try:
                summary = self._simulate(job, run_dir, until, False)
            except RuntimeError as e:
                print("atlas:", e, flush=True)
                shutil.rmtree(run_dir, ignore_errors=True)
                continue
            if os.path.exists(os.path.join(run_dir, "latest.cfs")) and abs((summary.get("cells") or 0) - (r["cells"] or 0)) <= 0.02 * max(r["cells"] or 1, 1):
                pending.append((r["id"], run_dir))
            else:
                print(f"atlas: run {r['id']} did not come out as before ({summary.get('cells')} cells, {r['cells']} then); no screenshot", flush=True)
                shutil.rmtree(run_dir, ignore_errors=True)
            if len(pending) >= batch:
                flush()
        flush()
