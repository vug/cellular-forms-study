"""The atlas: a database of every form that was tried, and the tools that fill and read it.

    python scripts/atlas survey --runs 3000        fill the parameter space evenly
    python scripts/atlas status                    what is in the database
    python scripts/atlas features                  measure the spread between seeds of every feature
    python scripts/atlas stop-fit                  fit and test the early-stop rule on the database
    python scripts/atlas kinds                     sort the forms into kinds, lay out the map
    python scripts/atlas search --driver novelty   look for forms unlike those already there
    python scripts/atlas export                    write the data and sheets the page shows
    python scripts/atlas serve                     show the page (and keep ratings)

Every command takes --root (default runs/atlas). docs/design.md, "Finding interesting
parameters", says what each part is for. Only the Python standard library is used.
"""
import argparse
import os
import random
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
__package__ = "atlas"  # so that the modules can import each other when the folder is run as a script

from . import space  # noqa: E402
from .runner import Job, Runner  # noqa: E402
from .store import Store  # noqa: E402

REPO = os.path.dirname(os.path.dirname(HERE))


def progress(label, t0):
    counts = {}

    def on_done(run, record, stages):
        counts[record["status"]] = counts.get(record["status"], 0) + 1
        total = sum(counts.values())
        if total % 96 == 0:
            print(f"{label}: {total} runs, {time.time() - t0:.0f} s; " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), flush=True)

    return on_done, counts


def cmd_survey(args):
    store = Store(args.root)
    rng = random.Random(args.seed)
    genomes = [space.default_genome()] + space.even_genomes(args.runs, rng, args.sources)
    jobs = []
    for i, genome in enumerate(genomes):
        if i > 0 and rng.random() < args.other_starts:
            genome["init"] = dict(rng.choice(space.OTHER_INITS))
        jobs.append(Job(genome, seed=1, origin="survey", target=args.target))
        if i % 20 == 0:  # every twentieth also with a second seed: the spread between seeds, kept in view
            jobs.append(Job(genome, seed=2, origin="survey", target=args.target))
    runner = Runner(store, max_steps=args.max_steps, photos=not args.no_photos)
    t0 = time.time()
    on_done, counts = progress("survey", t0)
    runner.run_many(jobs, on_done)
    print(f"survey: {sum(counts.values())} new runs in {time.time() - t0:.0f} s; " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
    cmd_status(args)


def cmd_photos(args):
    """Takes the screenshots that are missing (or, with --all, every one again)."""
    store = Store(args.root)
    have = store.images()
    runs = [r for r in store.runs("ORDER BY id") if args.all or not all(v in have.get(r["id"], {}) for v in ("a", "b", "tile", "thumb"))]
    print(f"{len(runs)} runs to photograph")
    t0 = time.time()
    Runner(store).retake(runs)
    print(f"done in {time.time() - t0:.0f} s")


def cmd_status(args):
    store = Store(args.root)
    print(f"{store.count()} runs in {store.root}")
    for row in store.db.execute("SELECT origin, status, COUNT(*) AS n, SUM(wall_s) AS wall FROM runs GROUP BY origin, status ORDER BY origin, status"):
        print(f"  {row['origin'] or '-':18s} {row['status']:14s} {row['n']:6d} runs  {row['wall'] or 0:8.0f} s of simulation")
    print(f"  {store.db.execute('SELECT COUNT(*) FROM images').fetchone()[0]} screenshots, {store.db.execute('SELECT COUNT(*) FROM labels').fetchone()[0]} rated or named")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.join(REPO, "runs", "atlas"), help="the folder of the database and its screenshots")
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("survey", help="grow forms that fill the parameter space evenly")
    p.add_argument("--runs", type=int, default=1000)
    p.add_argument("--seed", type=int, default=1, help="seed of the draw (a different seed adds different forms to the same database)")
    p.add_argument("--target", type=int, default=31623, help="cells every form grows to")
    p.add_argument("--sources", type=float, default=0.25, help="chance of each optional food source or extension being on")
    p.add_argument("--other-starts", type=float, default=0.1, help="share of the forms that start from another shape than the icosphere")
    p.add_argument("--max-steps", type=int, default=60000)
    p.add_argument("--no-photos", action="store_true")
    p.set_defaults(run=cmd_survey)

    p = sub.add_parser("status", help="what is in the database")
    p.set_defaults(run=cmd_status)

    p = sub.add_parser("photos", help="take the screenshots that are missing (each run is grown again for it)")
    p.add_argument("--all", action="store_true", help="every screenshot again")
    p.set_defaults(run=cmd_photos)

    from . import commands  # the later parts: features, early stop, kinds, search, export, serve
    commands.register(sub)

    args = ap.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
