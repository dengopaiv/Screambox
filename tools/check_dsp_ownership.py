# -*- coding: utf-8 -*-
"""Finding I: prove by construction that DSP state is written on ONE thread.

MSVC ships AddressSanitizer but not ThreadSanitizer, so the acceptance test that
finding I asked for - "under a sanitiser build, reporting no race on the DSP
fields" - cannot be run as written on this toolchain. ASan does not detect data
races. This is the substitute, and it is arguably the better test anyway: rather
than observing that no race happened during one run, it checks the property that
makes a race impossible.

Run it with:

    python tools/check_dsp_ownership.py

The property: every non-atomic member that the DSP touches is assigned ONLY from
functions the audio thread calls. Anything the UI thread can reach must go
through an atomic.
"""
import io
import re
import sys
import os
_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(os.path.dirname(_HERE), "src")


ENG = os.path.join(_SRC, "CBEngine.cpp")
HDR = os.path.join(_SRC, "CBEngine.h")

# Functions the audio thread owns. Writes to DSP state are legal only here.
AUDIO_THREAD = {
    "CBEngine::process",
    "CBEngine::triggerStep",
    "CBEngine::resetFilters",
    "CBEngine::applyTransportReset",
}
# Constructor and setSampleRate run before/outside streaming; setSampleRate is
# called by the audio thread itself, each time it opens (or after losing one,
# reopens) a device and before it renders a block at the new rate - never from
# the UI.
CONSTRUCTION = {"CBEngine::CBEngine", "CBEngine::setSampleRate"}

src = io.open(ENG, encoding="utf-8").read()
hdr = io.open(HDR, encoding="utf-8").read()

# Every private member declared with a trailing underscore that is NOT atomic,
# NOT a container the writers own, and NOT plainly UI-side.
members = set()
# Arrays count too: `float sweptVal_[N]{};` is DSP state exactly like a scalar,
# and before the sweep overrides arrived nothing non-atomic was an array, so
# the pattern never had to allow the brackets.
for m in re.finditer(r"^\s{4}(?!//)([A-Za-z_][\w:<>, ]*?)\s+([a-zA-Z]\w*_)\s*(?:\[[^\]]*\])?\s*(?:=|\{|;)",
                     hdr, re.M):
    typ, name = m.group(1).strip(), m.group(2)
    if "atomic" in typ:
        continue
    if name in ("song_", "writerMu_", "patternMu_", "liveIdx_"):
        continue          # song data, owned by SongWriter; finding A covers it
    members.add(name)

# Split the .cpp into function bodies by top-level "Type CBEngine::name(" lines.
bounds = []
for m in re.finditer(r"^[A-Za-z_][\w:<>&*\s]*?\b(CBEngine::\w+)\s*\(", src, re.M):
    bounds.append((m.start(), m.group(1)))
bounds.append((len(src), "<eof>"))

fails = []
writes = {}
for i in range(len(bounds) - 1):
    start, fname = bounds[i]
    body = src[start:bounds[i + 1][0]]
    for name in members:
        # Assignment, compound assignment, or ++/-- on the member.
        # An element write (`name[i] = ...`) is a write to the member.
        pat = (r"(?<![\w.>])" + re.escape(name) +
               r"\s*(?:\[[^\]]*\])?\s*(?:=(?!=)|\+=|-=|\*=|/=|\+\+|--)|(?:\+\+|--)\s*" + re.escape(name))
        # Binding a NON-const reference to it - `OscRun& r = osc_[k];`,
        # `for (auto& r : osc_)` - is a write too: everything after goes
        # through `r` and the pattern above never sees the member's name
        # again. The oscillating sweeps' state (osc_) is written only that way.
        ref = (r"(?<!const )\b\w+\s*&\s*\w+\s*(?:=|:)\s*" + re.escape(name) + r"\b")
        if re.search(pat, body) or re.search(ref, body):
            writes.setdefault(name, set()).add(fname)

print("=" * 74)
print("Finding I: DSP state ownership (static check, in place of ThreadSanitizer)")
print("=" * 74)
allowed = AUDIO_THREAD | CONSTRUCTION
offenders = {}
for name, fns in sorted(writes.items()):
    bad = fns - allowed
    if bad:
        offenders[name] = bad

print("  %d non-atomic members checked, %d of them written somewhere"
      % (len(members), len(writes)))
for name, fns in sorted(writes.items()):
    tag = "  <-- UI-REACHABLE" if name in offenders else ""
    print("    %-18s written by: %s%s" % (name, ", ".join(sorted(fns)), tag))

print()
if offenders:
    print("  FAIL  %d member(s) written outside the audio thread:" % len(offenders))
    for name, fns in sorted(offenders.items()):
        print("          %s  <- %s" % (name, ", ".join(sorted(fns))))
    fails.append("ownership")
else:
    print("  PASS  every non-atomic DSP member is written only by %s"
          % ", ".join(sorted(AUDIO_THREAD | CONSTRUCTION)))

# And the transport really does go through an atomic now.
if "transportReset_.store" in src and "transportReset_.exchange" in src:
    print("  PASS  setRunning() publishes through transportReset_ and process() consumes it")
else:
    print("  FAIL  transportReset_ is not being used as expected")
    fails.append("transport")
if re.search(r"void CBEngine::setRunning[\s\S]{0,600}?resetFilters\(\)", src):
    print("  FAIL  setRunning() still calls resetFilters() directly")
    fails.append("setRunning")
else:
    print("  PASS  setRunning() no longer calls resetFilters() itself")

print("=" * 74)
print("DSP OWNERSHIP OK" if not fails else "DSP OWNERSHIP FAILURES: " + ", ".join(fails))
print("=" * 74)
sys.exit(1 if fails else 0)
