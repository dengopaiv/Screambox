# -*- coding: utf-8 -*-
"""The sample-exact tester: proves the sound did not change.

Screambox's standard is that the sound is not "close", it is identical. This
script makes that a one-command check. It runs the render driver
(tests/render.c, built as the `screambox_render` target) and compares what it
prints with tests/golden/sound.manifest, the record of a build whose sound was
accepted.

    python tools/check_sound.py               # compare with the manifest
    python tools/check_sound.py --update      # accept the current sound
    python tools/check_sound.py --against X   # compare with another driver
                                              # build X, case by case
    python tools/check_sound.py --exe X       # test driver X instead of the
                                              # default Release build

What it checks, for every case:

  * The render at 512 frames per block hashes to the manifest's value.
  * Hard requirements, which --update refuses to record a failure of: the
    same render at 32, 64 and 1024 frames is identical; the case's .roq read
    back renders the same; .roq and .hape read back and saved again are the
    same bytes; and process() makes no heap allocation (invariant 3).
  * The .roq and .hape files the case saves are the manifest's bytes, so the
    writer - and the version it stamps - cannot change unnoticed either.
  * Relations that must hold whatever the manifest says (see RELATIONS).

The songs in tests/songs/ are rendered from their files, and so is the local
modules/ folder (not in git) when it is present. The modules' lines live in
tests/golden/modules.manifest, which is git-ignored like the folder - their
names are not to be published; a missing modules/ folder is reported as
skipped rather than failed.

When a hash differs and a reference driver exists (--update copies the
driver to build/sound/reference/), both are re-run on the failing cases and
the report names the first differing sample as a time, a pattern and a step.

Building the driver:

    cmake --build build --config Release --target screambox_render

When the C port starts, the same render.c is linked against the C engine, and
the manifest stays exactly as it is: that is the point of it.
"""
import argparse
import datetime
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MANIFEST = os.path.join(ROOT, "tests", "golden", "sound.manifest")
# The lines for modules/ - the local presets and testers' songs, which are not
# in git and must not be published (Dengo, 2026-11-09). Their lines name the
# files, so they live in a git-ignored manifest of their own beside the
# tracked one; read together, written apart.
MODULE_MANIFEST = os.path.join(ROOT, "tests", "golden", "modules.manifest")
MODULE_PREFIX = "file modules_"
# tests/songs/ is local too: song files are never committed (Dengo,
# 2026-11-09), so its lines go to the local manifest with the modules'.
SONGS_PREFIX = "file tests_songs_"
LOCAL_PREFIXES = (MODULE_PREFIX, SONGS_PREFIX)
DEFAULT_EXE = os.path.join(ROOT, "build", "tests", "Release", "screambox_render.exe")
WORK = os.path.join(ROOT, "build", "sound")
REFERENCE_EXE = os.path.join(WORK, "reference", "screambox_render.exe")
MODULES = os.path.join(ROOT, "modules")
# Songs written by older builds, so that what a newer build makes of them on
# load (the v15 PWM LFO fold, say) is tested on the bytes those builds really
# wrote. Local, not in git: song files are never committed.
SONGS = os.path.join(ROOT, "tests", "songs")

# Fields that must match the manifest. sr, tempo, loop, pats and frames are
# compared too: a case whose setup changed is a different case, not a
# different sound, and should say so.
COMPARED = ["sr", "tempo", "loop", "pats", "frames", "hash", "roq", "hape", "resave"]

# Pairs of cases that must render identically whatever the manifest holds.
RELATIONS = [
    # Classic is the stock 2004 clipper: gain, tone and level are inert while
    # it is selected, so the shipped sound is unchanged by their settings.
    ("stock", "dist_classic", "Classic distortion ignores gain, tone and level"),
    # An oscillating sweep adds depth x wave to the target; at depth 0 it must
    # add nothing - on a per-step target and on the per-sample PWM path alike.
    ("stock", "osc_zero_cutoff", "an oscillating sweep of depth 0 changes nothing"),
    ("stock", "osc_zero_pwm", "an oscillating sweep of depth 0 changes nothing, per sample"),
    # A straight sweep glides within the step; one whose value never changes
    # must still be exactly the slider at that value.
    ("cutoff_high", "glide_const_cutoff", "a constant sweep glides to the same value"),
    ("ring_high", "glide_const_ring", "a constant sweep on a geometric target glides to the same value"),
    ("cutoff_high", "osc_between_const", "a between-LFO with equal ends holds that value"),
    ("ring_high", "osc_between_ring_const", "a between-LFO with equal ends holds that value, on a frequency"),
]


def run_driver(exe, args):
    proc = subprocess.run([exe] + args, capture_output=True, text=True)
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)
    return proc.returncode, proc.stdout


def parse(stdout):
    """{key: fields} from the driver's lines; key is 'case NAME' or 'file NAME'."""
    out = {}
    for line in stdout.splitlines():
        parts = line.split()
        if len(parts) < 3 or parts[0] not in ("case", "file"):
            continue
        fields = dict(kv.split("=", 1) for kv in parts[2:])
        out[parts[0] + " " + parts[1]] = fields
    return out


def module_paths():
    found = []
    for top in (SONGS, MODULES):
        if not os.path.isdir(top):
            continue
        for dirpath, _, files in os.walk(top):
            for f in files:
                if f.lower().endswith((".roq", ".cb")):
                    found.append(os.path.relpath(os.path.join(dirpath, f), ROOT).replace("\\", "/"))
    return sorted(found)


def run_all(exe, outdir, keep=False, only=None):
    """Run every case (and module) and return (results, exit_ok)."""
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    args = ["cases", outdir] + (["--keep"] if keep else [])
    if only is not None:
        names = [k.split(" ", 1)[1] for k in only if k.startswith("case ")]
        args += ["--only", ",".join(names)] if names else ["--only", "-none-"]
    code, stdout = run_driver(exe, args)
    results = parse(stdout)
    ok = code == 0
    cwd = os.getcwd()
    os.chdir(ROOT)   # module paths are given relative to the repository
    try:
        for rel in module_paths():
            if only is not None and not any(k.startswith("file ") for k in only):
                break
            fcode, fout = run_driver(exe, ["file", outdir, rel] + (["--keep"] if keep else []))
            got = parse(fout)
            if only is not None:
                got = {k: v for k, v in got.items() if k in only}
            results.update(got)
            ok = ok and fcode in (0, 1)
    finally:
        os.chdir(cwd)
    return results, ok


def hard_failures(results):
    bad = []
    for key, f in sorted(results.items()):
        why = []
        if f.get("blocks") != "ok":
            why.append("differs with the block size (" + f.get("blocks", "?") + ")")
        if f.get("allocs") != "0":
            why.append(f.get("allocs", "?") + " allocations inside process()")
        if key.startswith("case "):
            if f.get("reload") != "ok":
                why.append("its .roq renders differently when read back")
            if f.get("resave") != "ok":
                why.append("its .roq changes when read back and saved")
        if f.get("haperesave") not in ("ok", "-"):
            why.append("its .hape changes when read back and saved")
        if why:
            bad.append(key + ": " + "; ".join(why))
    return bad


def relation_failures(results):
    bad = []
    for a, b, why in RELATIONS:
        fa, fb = results.get("case " + a), results.get("case " + b)
        if fa and fb and fa["hash"] != fb["hash"]:
            bad.append(f"{a} and {b} must render identically ({why})")
    return bad


def _read_one(path, entries, header):
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                header.append(line)
                continue
            parts = line.split()
            entries[parts[0] + " " + parts[1]] = dict(kv.split("=", 1) for kv in parts[2:])


def read_manifest():
    """The tracked manifest and, when this machine has one, the local module
    manifest, as one dict."""
    entries, header = {}, []
    if not os.path.exists(MANIFEST):
        return None, []
    _read_one(MANIFEST, entries, header)
    if os.path.exists(MODULE_MANIFEST):
        _read_one(MODULE_MANIFEST, entries, [])
    return entries, header


def git_describe():
    try:
        head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                              capture_output=True, text=True).stdout.strip()
        # src/ is what makes the sound; an uncommitted doc or test edit does
        # not make the accepted sound any less that commit's.
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no", "--", "src"],
                               cwd=ROOT, capture_output=True, text=True).stdout.strip()
        return head + (" plus uncommitted changes to src/" if dirty else "")
    except OSError:
        return "unknown"


def write_manifest(results):
    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    keep = ["sr", "tempo", "loop", "pats", "frames", "hash", "roq", "hape", "resave"]
    tracked = {k: v for k, v in results.items() if not k.startswith(LOCAL_PREFIXES)}
    modules = {k: v for k, v in results.items() if k.startswith(LOCAL_PREFIXES)}
    # Accepted local lines whose folder is not on the machine this run - the
    # modules moved away, say - are kept, not wiped: this run knows nothing
    # about them, which is not the same as their sound having gone.
    if os.path.exists(MODULE_MANIFEST):
        old = {}
        _read_one(MODULE_MANIFEST, old, [])
        for key, f in old.items():
            absent = (key.startswith(MODULE_PREFIX) and not os.path.isdir(MODULES)) or \
                     (key.startswith(SONGS_PREFIX) and not os.path.isdir(SONGS))
            if absent and key not in modules:
                modules[key] = f

    def lines(fh, entries):
        for key in sorted(entries, key=lambda k: (k.startswith("file "), k)):
            f = entries[key]
            fh.write(key + " " + " ".join(f"{k}={f[k]}" for k in keep if k in f) + "\n")

    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("# Screambox sample-exact manifest - written by tools/check_sound.py --update.\n")
        fh.write("# Every line is a sound that was accepted. Change this file only on purpose,\n")
        fh.write("# in the same commit as the change that legitimately moved the sound, and say\n")
        fh.write("# in that commit which lines moved and why.\n")
        fh.write(f"# Accepted {datetime.date.today().isoformat()} from {git_describe()}.\n")
        fh.write("# The modules/ and tests/songs/ lines are in modules.manifest beside this file:\n")
        fh.write("# local, not in git.\n")
        fh.write("#\n")
        lines(fh, tracked)
    # Only rewritten when this run had the modules: a machine without modules/
    # must not wipe the accepted module lines another run wrote.
    if modules:
        with open(MODULE_MANIFEST, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# Screambox module and test-song renders - LOCAL ONLY (git-ignored): modules/\n")
            fh.write("# and song files are not published, and these lines name them. Written by\n")
            fh.write("# check_sound.py --update.\n")
            fh.write(f"# Accepted {datetime.date.today().isoformat()} from {git_describe()}.\n")
            fh.write("#\n")
            lines(fh, modules)


# ------------------------------------------------------------- diagnosis

def step_starts(fields):
    """Sample offsets at which each (pattern, step) starts, reproducing
    stepLenFrom / stepCarry in CBEngine.cpp in the same double arithmetic."""
    sr = float(fields["sr"])
    tempo = float(fields["tempo"])
    pats = []
    for p in fields["pats"].split(","):
        trip = p.endswith("t")
        pats.append((int(p.rstrip("t")), trip))
    order = [0] * 3 if fields["loop"] == "1" else list(range(len(pats))) + [0]
    frames = int(fields["frames"])
    starts, at, owed = [], 0, 0.0
    for p in order:
        n, trip = pats[p]
        exact = (sr * 60.0) / (tempo * 3.0) if trip else (sr * 60.0) / (tempo * 4.0)
        for s in range(n):
            length = max(1, int(exact + owed))
            c = (exact + owed) - length
            if c < 0.0:
                c = 0.0
            if c >= 1.0:
                c -= int(c)
            owed = c
            starts.append((at, p, s))
            at += length
            if at >= frames:
                return starts
    return starts


def where(fields, sample):
    sr = float(fields["sr"])
    pos = "?"
    for at, p, s in step_starts(fields):
        if at <= sample:
            pos = f"pattern {p + 1}, step {s + 1}, {sample - at} samples in"
    return f"sample {sample} ({sample / sr:.4f} s), {pos}"


def explain(key, fields, dir_a, dir_b):
    name = key.split(" ", 1)[1]
    pa, pb = os.path.join(dir_a, name + ".f32"), os.path.join(dir_b, name + ".f32")
    if not (os.path.exists(pa) and os.path.exists(pb)):
        return "      (no renders to compare)"
    a, b = open(pa, "rb").read(), open(pb, "rb").read()
    if len(a) != len(b):
        return f"      lengths differ: {len(a) // 8} vs {len(b) // 8} frames"
    fa = struct.unpack(f"<{len(a) // 4}f", a)
    fb = struct.unpack(f"<{len(b) // 4}f", b)
    diffs = [i for i in range(len(fa)) if fa[i] != fb[i]]
    if not diffs:
        return "      renders are identical (only a file hash differs?)"
    first = diffs[0]
    worst = max(abs(fa[i] - fb[i]) for i in diffs)
    return (f"      first difference at {where(fields, first // 2)}, "
            f"{'left' if first % 2 == 0 else 'right'} channel; "
            f"{len(diffs)} of {len(fa)} values differ, largest by {worst:.3g}")


def diagnose(failing, results, exe_a, exe_b, label_a, label_b):
    if not failing:
        return
    if not (exe_a and os.path.exists(exe_a)):
        print("   (no reference driver at build/sound/reference/ to locate the differences;"
              " run --update on a good build, or use --against)")
        return
    dir_a, dir_b = os.path.join(WORK, "diag_" + label_a), os.path.join(WORK, "diag_" + label_b)
    run_all(exe_a, dir_a, keep=True, only=set(failing))
    run_all(exe_b, dir_b, keep=True, only=set(failing))
    for key in failing:
        print(f"   {key}:")
        print(explain(key, results[key], dir_a, dir_b))


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--exe", default=DEFAULT_EXE, help="render driver to test")
    ap.add_argument("--update", action="store_true", help="accept the current sound")
    ap.add_argument("--against", help="compare with another render driver instead of the manifest")
    args = ap.parse_args()

    if not os.path.exists(args.exe):
        print(f"No render driver at {args.exe}.\n"
              "Build it: cmake --build build --config Release --target screambox_render")
        return 2

    results, exit_ok = run_all(args.exe, os.path.join(WORK, "run"))
    if not results:
        print("The driver produced no results.")
        return 2
    fails = 0

    def check(ok, msg):
        nonlocal fails
        print(("PASS  " if ok else "FAIL  ") + msg)
        if not ok:
            fails += 1

    n_cases = sum(1 for k in results if k.startswith("case "))
    n_files = len(results) - n_cases
    hard = hard_failures(results)
    check(not hard, f"hard requirements on {len(results)} renders "
                    f"({n_cases} cases, {n_files} module files): block size, read-back, "
                    f"re-save, zero allocations")
    for h in hard:
        print("      " + h)
    rel = relation_failures(results)
    check(not rel, f"{len(RELATIONS)} relation(s) between cases")
    for r in rel:
        print("      " + r)
    check(exit_ok, "the driver ran every case to completion")

    if args.update:
        if fails:
            print("\nNot updating: a sound that fails a hard requirement is not one to accept.")
            return 1
        write_manifest(results)
        os.makedirs(os.path.dirname(REFERENCE_EXE), exist_ok=True)
        shutil.copy2(args.exe, REFERENCE_EXE)
        print(f"\nAccepted: {len(results)} renders written to {os.path.relpath(MANIFEST, ROOT)}; "
              f"this driver kept as the reference for locating future differences.")
        return 0

    if args.against:
        other, _ = run_all(args.against, os.path.join(WORK, "against"))
        want, label = other, "the other build"
        ref_exe = args.against
    else:
        want, header = read_manifest()
        if want is None:
            print(f"\nNo manifest yet at {os.path.relpath(MANIFEST, ROOT)}; run with --update "
                  "on a build whose sound is accepted.")
            return 1
        label = "the manifest"
        ref_exe = REFERENCE_EXE

    have_modules = os.path.isdir(MODULES)
    have_songs = os.path.isdir(SONGS)
    differing, missing, extra, skipped = [], [], [], 0
    for key, expected in sorted(want.items()):
        if key not in results:
            if (key.startswith(MODULE_PREFIX) and not have_modules) or \
               (key.startswith(SONGS_PREFIX) and not have_songs):
                skipped += 1
            else:
                missing.append(key)
            continue
        moved = [k for k in COMPARED if expected.get(k) != results[key].get(k)]
        if moved:
            differing.append((key, moved))
    extra = [k for k in results if k not in want]

    check(not differing, f"{len(want) - len(missing) - skipped - len(differing)} of "
                         f"{len(want) - skipped} renders identical to {label}")
    for key, moved in differing:
        print(f"      {key}: " + ", ".join(
            f"{k} {want[key].get(k, '-')} -> {results[key].get(k, '-')}" for k in moved))
    check(not missing, f"every render in {label} was produced")
    for key in missing:
        print(f"      missing: {key}")
    check(not extra, f"no render missing from {label}")
    for key in extra:
        print(f"      new, not in {label}: {key} (run --update once it is accepted)")
    if skipped:
        print(f"SKIP  {skipped} local renders: modules/ or tests/songs/ is not on this machine")

    sound_moved = [k for k, moved in differing if "hash" in moved]
    if sound_moved:
        print("\nWhere the sound first differs:")
        diagnose(sound_moved, results, ref_exe, args.exe, "reference", "current")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
