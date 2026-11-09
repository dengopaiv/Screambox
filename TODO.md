# Screambox — outstanding work

(Called Creakbox until 2026-11-06. The items below keep the name they were written under.)

G took `.roq` v9, B took v10, H took v11, K took v12 and O took v13. Nothing outstanding
wants a format change. Line references are current as of 2026-09-04 for E–K and
2026-11-06 for L–W (they describe the code *before* the fix, which is what the evidence was
taken against).

Background on any of it is in `README.md` (what the program does) and `CLAUDE.md` (the
invariants and the traps). Rounds 1–3 — the PWM LFO, noise / resonance boost / reverse
step, and the documentation plus keyboard rework — are done and verified.

**Done:** ~~E~~, ~~A~~, ~~J~~, ~~G~~, ~~B~~, ~~H~~, ~~I~~, ~~C~~, ~~F~~, ~~K~~, and from
the 2026-11-06 source study ~~L~~, ~~M~~, ~~N~~, ~~O~~, ~~P~~, ~~Q~~, ~~R~~, ~~S~~, ~~T~~,
~~U~~, ~~V~~, ~~W~~ — every decision in those is Dengo's, taken after the study.

**Nothing is open.** Not in code, and not by ear either: the listening and screen-reader
list at the bottom of this file was closed on 2026-11-07, checked by sheer use rather than
item by item — Dengo and the testers have made enough patterns that every feature on it has
been played and heard. See "Checked by use" below for what that means for the open
questions it held.

**Released as 2.0.0 on 2026-11-07** (1.0.0 was Creakbox, 2026-10-03), after testers
reported the 2026-11-06 test build working.

---

# X. Sweeps everywhere, Enter in the dialogs, shapes — BUILT 2026-11-09, needs ears

Dengo's three points, decided through the question dialog:

1. **Accent squelch had no sweep** -> it, and every slider in the Oscillator and Acid FX
   dialogs, is now a sweep target (7..23). `.roq` v14, stamped only when used.
2. **Enter in those dialogs pressed Close**, while Enter on a main-window slider opens the
   sweep editor -> Enter on a dialog slider now opens its sweep editor; Alt+C / Esc close.
3. **Sweeps vs envelopes, and presets** -> sweeps stay song-position automation; "presets"
   became shapes (ramp / up and down / alternate, N cycles) that the sweep editor draws as
   ordinary sweeps. No engine or format change for that part. A per-note envelope was
   discussed and not chosen.

**Verified** (harness against `git show HEAD:`, one source built twice): 58 of 58 common
renders byte-identical to HEAD at 32/64/512/1024 frames, including the original seven's
sweeps and a patch with every dialog feature on; every case identical across block sizes;
for each of the 17 new targets a constant sweep is byte-identical to the slider at that
value, and a ramp audibly differs; zero allocations in `process()` with all 24 targets
swept; all 34 shipped `.roq` re-save byte-identical (still 9 / 11); a v14 song is refused
by HEAD's reader and round-trips in the new one; the shape generator's output, replayed
step by step, matches the shape within half a slider step with no overlaps or gaps. Both
`tools/` checks pass; the app starts and closes with exit code 0.

**Not verified - needs a person:** Enter on each dialog slider with a screen reader (that
the editor opens over the dialog, focus returns to the slider, and the "(n sweeps)" label
is read); the shapes by ear, especially Up and down on Ring frequency and on PWM LFO rate.

---

# Y. Study 2026-11-09: sweep coverage, accent squelch, LFO as sweep, C rewrite

Dengo's four points. 1 and 2 are answered and measured. 3 and 4 were studied, then decided
through the question dialog:

- **Y3: oscillating sweeps**, as proposed below: evaluated at sample rate for PWM and the
  cheap targets and at step rate for the rest. The PWM LFO converts into them, and its
  sliders leave the Oscillator dialog. **Phase comes from the song position**, so every
  play and every export is identical. **BUILT 2026-11-09 on `rework`, needs ears** - see
  *Y3, as built* below.
- **Y4: the whole program in C**, not only the engine. **C17** (Dengo, 2026-11-09 - it is
  what his other projects use; C23's gains here are cosmetic. The plan below was first
  written for C23 on clang-cl). **Sample-exact is the acceptance criterion.** PLANNED - see
  *Y4 plan* - **SHELVED 2026-11-09** (Dengo): the program stays C++. See *Y4, the
  calculus*; the plan stays on file in case it is ever revived.
- **Order:** the tester first (BUILT, see Y4), then the oscillating sweeps in C++, then the
  C port against frozen reference renders.

## Y1. Which parameters have sweeps: all of the requested ones, as of X

Every parameter on the list is a sweep target in X, which is uncommitted and unreleased:
Volume, Cutoff, Resonance, EnvMod, Decay, Accent and Tuning (0-6); PWM (8); Duplet PWM,
called "pwm2" (11); Sub level (9); Duplet detune, called "osc2 tune" (12); Duplet level (13);
Noise (10); Dist gain, tone and level (17-19); Ring frequency and amount (22, 23). X also
added Accent squelch (7), the three PWM LFO controls (14-16) and the compressor's threshold
and depth (20, 21). Nothing is missing.

## Y2. Accent squelch: unchanged, measured

**The engine is unchanged.** I built one harness source against three engines: the initial
commit `77a6309`, 2.0.0 (`HEAD`) and the working tree. All 30 renders are byte-identical
across all three: squelch 0 / 0.5 / 1 / 1.37 / 2, three accent patterns (runs of three,
alternating, every step) and both a plain patch and one with the oscillator extras and
distortion on. The renders avoid the things that legitimately changed: 44.1 kHz, 16-step
patterns, no slide on a last step and no pitch 52. Squelch is clearly audible in them: the
RMS difference from squelch 1 is 0.05-0.09. The only engine edit in all that history is
the read itself, `params_` changed to `effParam()`, which returns the same value when no
sweep is running.

**What did change is the slider, in W (2.0.0).** In Creakbox 1.0 the slider ran 0-100 % and
meant squelch 1.0-2.0, so stock was "0 %". Since 2.0.0 it runs 0-200 % and the position is
the squelch value times 100, so stock reads "100 %". The range 0-99 % is new territory below
stock, and at 0 an accent gives no filter wah at all. Files store the real value, so no saved
song moved, but a setting remembered as "50 %" is now found at 150 %.

**How a squelch sweep acts:** squelch is read only when an accented step triggers
(`boxAccentFrq_ = (tempAccentFrq_ + 2) * sq`), so a sweep on it changes accent to accent and
never moves within a note.

## Y3. Folding the PWM LFO into sweeps: study

**What exists.**
- **Sweeps** have a start and end at song positions (pattern and step) and draw a straight
  line between them, linear or geometric. They can hold their end value, and they are
  evaluated **once per step** (invariant 6). X's shapes generate ordinary sweeps that
  follow the shape step by step.
- **The PWM LFO** is a triangle evaluated **every sample**. It has one rate in Hz and two
  depths (main and duplet PWM), its result is clamped at the rails, and its phase is shared
  and free-running.

**Three facts that shape the design.**
1. **The step-rate ceiling.** At 140 BPM there are 9.3 steps a second, and a cycle needs at
   least 2 steps, so a step-evaluated sweep tops out near **4.7 Hz**, in stairs. The PWM LFO
   goes to 100 Hz. Folding the LFO into sweeps therefore requires a sweep that can be
   evaluated faster than once per step. That means amending invariant 6, and it is the
   real decision here; everything else follows from it.
2. **The LFO's phase is never reset, not even when you press Play.** `pwmLfoPhase_` has no
   reset anywhere. Each play of a song therefore catches it at a different point, and a WAV
   export, which uses a fresh engine, starts it at 0, which is not what you heard. If an
   LFO sweep takes its phase from the song position, every play is identical and the export
   matches the playback. Invariant 1 forbids a reset *per pattern*, and that stays
   forbidden: the phase would run continuously from the start of the sweep.
3. **Targets 14-16 (the LFO's rate and depths) are unreleased.** If the LFO goes, they can be
   dropped before X ships at no compatibility cost. Once a release has shipped, those
   numbers are permanent.

**Proposed model: an oscillating sweep.** A sweep gains optional fields:
- shape: none, triangle, sine, square, saw up or saw down;
- rate: either *steps per cycle* (locked to tempo) or *Hz* (independent of tempo);
- depth: in the target's real units, plus or minus;
- start phase.

The sweep's existing start-to-end line stays as the **centre**, so a sweep can ramp and
wobble at the same time, and Hold applies to the centre. Shape *none* is exactly today's
sweep. An LFO is simply an oscillating sweep that covers the whole song. It needs a
"to the end of the song" endpoint so that adding patterns does not leave it short.

**Evaluation rate, by target class.**
- **a. Step rate, every target.** The oscillation is sampled once per step, as X's shapes are
  now, but it is stored as one editable record instead of many generated pieces, so its rate
  and depth can be changed afterwards. This changes only `evaluateSweeps`, not the sample
  loop.
- **b. Sample rate, for targets that are cheap per sample.** PWM and Duplet PWM already do this
  work, so they cost nothing new. Volume, sub, noise, duplet level and ring amount are nearly
  free. Tuning costs one `pow` per sample, which gives vibrato. The phase comes from the
  absolute sample position since the sweep started, so the result does not depend on the
  block size, and the 32/64/512/1024 test applies unchanged.
- **c. Control rate (every 32 samples) for the expensive targets.** These are cutoff,
  resonance and the dist/comp/ring coefficients, all of which are re-derived through `pow`
  and `prepare`. The 32-sample grid must be counted from the step start, not from the
  block. This class could be deferred.

Off-path rule (invariants 4 and 5): with no oscillating sweep, the sample loop is
byte-identical and pays only one branch.

**Moving the PWM LFO.** The three LFO sliders leave the Oscillator dialog. The `.roq` header
keeps their three fields, because the header is positional, and they are written as 0. On
load, a non-zero LFO converts to oscillating sweeps on targets 8 and 11: triangle, Hz, the
same depth, covering the whole song. That conversion is **provably bit-identical in a fresh
engine** (the export case) if the sample-rate path keeps today's expressions: the
increment `rate / sr` is accumulated, then the triangle formula, then the clamp. Playback
in the app changes on purpose, as described in fact 2. Two consequences:
- A song that used the LFO re-saves as v15 and no longer opens in 2.0.0.
- Darkside's feature moves out of the Oscillator dialog, so tell him.

**Format.** The oscillation goes on the sweep line as appended tokens, using the step-line
trick. But an older build would ignore those tokens and play a plain ramp without saying
so. So `neededVersion` must force **v15** whenever any sweep oscillates, for the same
reason as v14. `.hape` follows the same content rule.

**The sub-editor Dengo described.** A dialog next to Oscillator and Acid FX, called
"Sweeps" or "Modulation":
- A listbox of all the targets, each read with its count, for example
  `Cutoff (2 sweeps, 1 LFO)`.
- Enter opens that target's sweep editor, as Enter on its slider does.
- "Add LFO..." creates a whole-song oscillating sweep, with a choice of shape, rate and
  depth.
- "Apply preset..." applies a saved shape.
- The sweep editor itself gains the shape, rate and depth fields, so an LFO appears in the
  parameter's own sweep list and is edited there.

All of these are standard controls with labels, so the accessibility rule holds. The
dialog's mnemonics are its own, not the main window's.

**Open questions:**
- Which targets get sample rate (b), and whether c is wanted at all.
- What a preset stores: a shape, rate and depth relative to the range (proposed), or
  absolute sweeps like a `.hape`.
- Whether the "Hz" rate is needed at all. Steps per cycle is the musical unit, but the
  converted PWM LFO needs Hz.

## Y3, as built (2026-11-09, branch `rework`)

**The three open questions, decided as defaults (say if any should go the other way):**
- **Sample rate (b) for Main PWM and Duplet PWM only**, which is what the PWM LFO needs.
  Everything else is step rate (a). Volume (tremolo) and Tuning (vibrato) are the obvious
  next per-sample targets; Cutoff wants the control rate (c). Not done: each needs its own
  per-sample path with the hoisted value as its off path, and a reason to pay for it.
- **Presets are relative** - shape, rate, and depth as a fraction of the target's range - so
  one list serves every target. They live in the Modulation dialog and are not files.
- **Both rate units**: steps per cycle (the musical one, follows the tempo) and Hz (what the
  converted LFO needs, and a vibrato's natural unit).

**One design change from the study:** the study had the sweep's own start-to-end line as
the centre. As built, **an LFO moves whatever the target would otherwise be** - the slider,
or a straight sweep on the same step - and its start and end values are its *depth*. That
is what the PWM LFO did (it wobbled around the PWM slider, swept or not), so a song with
both a PWM sweep and the LFO converts exactly, which the study's model could not do; a ramp
under a wobble is still one straight sweep plus one LFO; and the depth can fade in. Straight
sweeps and LFOs are chosen separately per step and never overlap each other. **Confirmed by
Dengo 2026-11-09: keep the riding model.** Next: Dengo tests by ear before `rework` merges.

**Apply** (Dengo, 2026-11-09): the sweep editor's Apply (`Alt+Y`) updates the sweep shown
from the fields and sends the slider's list to the engine without closing, to hear a change
while the song plays; Cancel still undoes it. With no sweep in the list it only sends the
list, so Remove-then-Apply goes quiet instead of re-adding the fields. Cycles moved from
Alt+Y to Alt+X for it. Driven in the app: Apply reaches the engine (the main window's label
changes with the editor open), Add alone does not, Cancel takes it back out.
F5-F8 inside the dialogs (Dengo, same day): a modal dialog runs its own loop, so the main
window's accelerators never saw them. A WH_MSGFILTER hook (`dialogKeyHook`) passes them to
the main window from our own four dialogs only - recognised by dialog procedure, so the file
dialogs keep F5 as Refresh - skips auto-repeat, and copies the main window's status text
into the dialog's status line so a screen reader there hears it.

**A second sweep was impossible to add** (Dengo, same day). The Sweep number list offered
"(new)" only while it was empty; after one sweep the fields always showed an existing sweep,
so Add could only collide with it - "Overlap with sweep 1. Adjust ranges." - and with a
first sweep filling the pattern there was nowhere obvious to adjust to. Now "(new sweep)"
is always the last entry and starts the fields in the first range no sweep of that kind
covers (after patterns 1-4, pattern 5); an overlap names the sweep and range in the way and
the way out. The default range of a first sweep, the whole pattern, stays. Straight sweeps
on one slider still may not overlap - only a straight sweep and an LFO may.
**LFOs explained:** Help (F1) in the sweep editor and the Modulation dialog, a status-line
sentence whenever the LFO wave changes, and an opening line in the Modulation dialog.
Driven in the app, including the patterns 1-4 then 5 case.

**Sweeps glide** (Dengo, same day: "recalculate", and ramps too). Every sweep is still chosen
at the step boundary and has its old value at each step's first sample; within the step
`glideSweeps()` moves it on per sample and the knobs are derived again per sample, while
anything moves. Main PWM and Duplet PWM keep their own per-sample LFO path (bit-exact old
LFO). **Manifest lines moved, on purpose:** every case with a straight sweep that moves
(`sweep_t00`-`t06`, `t08`-`t13`, `t17`-`t23`, `sweep_hold`, `sweep_mixed`,
`sweep_all_48k`), every LFO on a per-step slider (`osc_cutoff_steps`, `osc_on_ramp`,
`osc_tuning_hz`, `osc_starts_late`, `osc_loop_hz`), the travelling LFO depth in
`osc_pwm_saws_48k`, and `tests/songs/lfo_pwm_sweep_v14` (a PWM ramp under the LFO). Each
first differs one sample into the sweep's first step (a few samples where the parameter
acts through a smoother or only at an edge); before that, every render is unchanged.
**Did not move:** the converted LFO (`lfo_*`, `rich*`, the other `tests/songs/`), every
module, every song without sweeps - and `sweep_t07`, Accent squelch, which is read only as
an accented note starts, so gliding it cannot be heard. New RELATIONS: a constant gliding
sweep on Cutoff and on Ring frequency equals the slider at that value.
**LFO "Between" and Ring depth in cents** (Dengo, same day: Ring frequency 95 and 100, then
an LFO, was expected to swing 95 to 100 Hz; it swung about +-95 Hz around the slider). Each
LFO now has a Swing choice: *around the slider* (the two values are a depth, as before) or
*between the start and end values* (it swings between them and sets the value itself).
Stored as an optional fifth oscillation token, written only on a between-LFO, so every other
saved byte is unchanged; v15 had never been released, so no version of its own. And an
around-LFO's depth on Ring frequency is now in CENTS (equal swing in pitch, fine steps) -
before, it reused the frequency slider's taper, whose smallest step was 5 Hz. **Manifest:**
only `osc_starts_late` moved (its Ring LFO, now 300 cents); new cases `osc_between_ring`
(Dengo's example), `_cutoff` (over a straight sweep it ignores), `_pwm` (the per-sample
path), and two RELATIONS - equal ends hold the value, on Cutoff and on Ring frequency.
Driven in the app. **The Ring frequency slider is finer** (Dengo, same day): 1000 positions
instead of 100, about 0.6 % apart (under a tenth of a semitone), in the Acid FX dialog and
the sweep editor alike - 95 Hz is position 491 (94.75 Hz, shown "95 Hz"). Songs store Hz,
so nothing moved; every old position is exactly every tenth new one. The sweep clipboard
now holds a share of the slider's travel instead of a raw position, since sliders no longer
all have about 100 positions: a Cutoff sweep 10-80 pasted onto Ring frequency still covers
positions 100-800 of 1000.
**Not verified - needs ears:** that gliding Decay, Resonance and the effect sliders sound
right within a note, and the CPU cost on a slow machine (every moving sweep re-derives
the knobs and the effect coefficients each sample).

**What changed where:**
- Engine: `SweepOsc` on `ParamSweep`; `evaluateSweeps` chooses the straight and the
  oscillating sweep separately; `osc_` per target, Hz phases accumulated per sample and
  re-derived from the samples since the sweep started after a discontinuity; the PWM LFO's
  per-sample block is now the per-sample LFO path, with its expressions kept verbatim.
  `pwmLfoPhase_` is gone; the three LFO parameters are read by nothing.
- Format: `.roq` / `.hape` v15, four tokens after the hold token, written only on an
  oscillating sweep. `loadRoq()` folds a running PWM LFO into whole-song held triangle LFOs
  in Hz on targets 8 and 11, and drops sweeps on 14-16. A rate-0 song is left untouched.
- UI: the Oscillator dialog's LFO group is gone (the dialog is 84 units shorter). The sweep
  editor has **LFO wave** (Alt+W), **Starting phase** (Alt+I), **Frequency** (Alt+Q) and
  **Measured in** (Alt+M); with a wave, the value captions say *depth* and Hold says what
  it does for an LFO. The sweep list marks LFOs, and every swept label counts them apart:
  `Cutoff (2 sweeps, 1 LFO)`. New **Modulation** dialog, `Ctrl+M` or its button: every live
  target with its counts, Enter edits, eight presets including *The old PWM LFO*.
- Tools: `check_dsp_ownership.py` now counts binding a non-const reference to a member as a
  write (`osc_` is only ever written that way, and was invisible to it).
  `check_keyboard.py` read the main window's mnemonics up to the next `void MainWindow::`,
  which swept in the sweep editor's helper labels; it now stops at `buildLayout`'s brace.

**Verified:**
- The tester: the old driver's renders kept from before the change, compared with the new
  driver's over the first pass of every case. All 44 cases without the LFO are
  byte-identical over the whole render. All 30 that had it - `lfo_*`, `rich*`, every
  `sweep_t*` (they run on the rich patch), `sweep_hold`, `sweep_mixed` - are byte-identical
  over the first pass and differ only after the wrap back to pattern 1, where the phase is
  now taken from the position. `sweep_all_48k` differs from the start because it swept the
  retired targets.
- `tests/songs/`: five songs the OLD build saved with the LFO on (v9, v11, v14 with a PWM
  sweep under it) load through the fold and render byte-identical over the first pass to
  the old build's renders, hash for hash the same as the new in-memory cases, and re-save
  byte-identical to the v15 files those cases write.
- New cases, each block-size invariant, reload / re-save / `.hape` round trip clean, zero
  allocations: steps-mode triangle on Cutoff, a sine riding a ramp with a fading-in depth,
  Hz square on Tuning with a start phase, saws on both PWMs across mixed and triplet
  patterns at 48 kHz, LFOs that start mid-song and end without hold on a dialog target and
  on the ring, and Hz LFOs in loop mode (all three loops byte-identical). Two RELATIONS in
  `check_sound.py`: a depth-0 LFO renders identical to `stock`, per step and per sample.
- Every module renders and re-saves exactly as before - including a tester's song with LFO
  rate 0 and both depths at 1, which the first version of the fold rewrote.
- `check_dsp_ownership.py`, `check_keyboard.py` pass; the app starts and closes with 0.
- Driven through Win32 messages in the running app: the Modulation dialog lists the 21 live
  targets; Add LFO places a bar wobble on Cutoff and refuses a second; *The old PWM LFO* on
  Main PWM opens in the editor as Triangle, 2, Hz, phase 0, captions *Start depth* and the
  LFO's Hold text; wave Off restores the captions; Cancel keeps the LFO; the main window's
  Cutoff reads `Cutoff (1 LFO)`.

**Not verified - needs a person:** everything by ear, above all the converted PWM LFO in
playback (it now restarts with the song instead of running free) and step-rate LFOs on the
filter; the sweep editor's new row and the Modulation dialog with a screen reader.

**Behaviour changes to tell people about:**
- **Darkside:** the PWM LFO left the Oscillator dialog. Enter on Main PWM (or the duplet's
  PWM), then LFO wave - or Ctrl+M, Main PWM, preset *The old PWM LFO*. His songs convert.
- "Everybody needs a 303" used to switch the PWM LFO off. The LFO is a sweep now, and that
  button has never touched sweeps, so it no longer does.
- A song with the LFO re-saves as v15 and no longer opens in 2.0.0.

## Y4. Rewriting in C17 or C23: study

**Size.** About 7000 compiled lines. The 2004 reference sources are not built. The
original DSP (`IIR`, `dist`, `freq`) is already C-style. The C++ features in use: `std::vector`
55 uses, `std::string`/`wstring` 72, `std::atomic` 14, mutex/locks 11, `std::thread` 2,
lambdas 14, classes 25, `try`/`catch`/`throw` 14, streams 14 (in `RoqIO`), `std::optional` 6,
and one template.

**Toolchain, measured on this machine** with VS 2026 (MSVC 19.51) and clang-cl 22:
- **C17 on MSVC works.** `<stdatomic.h>` works, and lock-free `_Atomic float` is fine, but it
  still needs `/experimental:c11atomics`. `<threads.h>` works.
- **C23 on MSVC is partial.** With `/std:clatest`, `typeof`, `[[nodiscard]]`, `static_assert`
  and binary literals compile. **`constexpr`, `bool`/`true` as keywords, `nullptr` and
  `<stdbit.h>` do not.**
- **clang-cl 22**, already installed as a VS component, compiles full C23 (`-T ClangCL`).
  "C23" therefore means changing the compiler, and that change needs its own byte-exact proof
  of the floating-point output.

**The sample-exact tester: BUILT 2026-11-09.** Sample-exact is the acceptance criterion
(Dengo), so the tester came first. Run it with `python tools/check_sound.py`. It is made of
three pieces:
- `tests/sb_api.h`: a C17 interface to the engine and the file code.
- `tests/sb_api_cpp.cpp`: implements that interface over the current C++ sources.
- `tests/render.c`: the render driver, in C and talking only to that header, so the C port
  links the same driver unchanged.

It renders 78 built-in cases:
- every main slider at its extremes;
- squelch 0 and 2;
- reverse steps, including one across a tie;
- slides on the last step, both in sequence and looping;
- triplet and mixed lengths (4 / 12t / 16 / 23), and a 64-step pattern;
- every pitch;
- resonance boost, PWM, sub, duplet, noise, and the PWM LFO at three rates;
- all five distortion types, the compressor and the ring modulator;
- everything on at once at 22.05, 44.1, 48 and 96 kHz;
- tempos 60, 300 and 133.7;
- a ramp on each of the 24 sweep targets, plus hold, missing-pattern and mixed-length
  sweeps.

It also renders every module under `modules/` when that folder is present: 111 files on
this machine.

For each render it checks:
- the 512-frame render's hash against `tests/golden/sound.manifest`;
- that renders at 32, 64 and 1024 frames are identical to the 512-frame one;
- that the case's `.roq` read back renders the same, and that `.roq` and `.hape` re-save
  to the same bytes;
- that the `.roq` / `.hape` bytes themselves match the manifest, which pins the version
  stamping;
- zero allocations in `process()`;
- that `dist_classic` renders the same as `stock`, because Classic ignores gain, tone and
  level.

On a mismatch it re-runs the reference driver that `--update` kept in `build/sound/` and
names the first differing sample as a time, a pattern and a step.

**Proven to catch a change:** changing one engine constant, `2.0 / 4500.0` to `2.0 / 4501.0`
in the accent decay (largest effect 0.0007), fails 141 of 189 renders. Each is located
12-27 samples into pattern 1, step 1, the first accented note. The renders it leaves alone
are the songs without accents. The first version of the script compared the manifest with
itself and passed this mutant; that is how the bug was found.

**Old module files re-save differently, and that is correct:** all 29 modules at format v1-v6
come out different, because the writer never writes below v9. Every v9 and v11 module
re-saves byte-identically. The manifest records the outcome per file, so only a *change* in
it fails.

**The compiler question is answered: clang-cl 22.1.3 passes 189/189**, every render and
every saved file byte-identical to the MSVC build. **C23 under clang-cl does not cost
sample-exactness.** C17 on MSVC remains possible too, with its experimental atomics switch.

The manifest was accepted from `d8882eb` plus the uncommitted round X. Commit X, then the
tester, so that the manifest names a real commit.

**What translates easily.** The Win32 UI is a C API already, and the accessibility strategy
is unaffected because the controls are the same. WASAPI and `IMMNotificationClient` work
from C through `COBJMACROS` and about 100 lines of hand-written vtable. The bit-exact
harness method proves an engine port directly, by rendering the C engine against the C++
engine.

**What is hard or risky.**
- Math overloads: `std::pow(float, float)` is not C's `pow`. Every call has to be checked
  and given the right `powf` or `pow`, or the output bits change.
- `RoqIO`'s stream formatting has to be reproduced byte for byte, or saved files change.
  The 34-module round trip covers that.
- `SongData`'s vectors become fixed-capacity arrays. This is a gain: invariant 3 (no
  allocation on the audio thread) becomes structural.
- `MainWindow` (3000 lines) is the risk. Nothing catches a UI regression except
  `check_keyboard.py` and testing by hand with a screen reader.

**What it buys.** Nothing audible: the sound is identical by construction. What it does buy
is an engine with a plain C ABI. **CLAP is a C plugin API**, so a C engine is the direct
path to a plugin, as well as to other hosts and other languages. (A plugin is not a goal -
Dengo, 2026-11-09.)

**Recommendation at study time** (superseded by Dengo's decision to port the whole
program): port only the engine to C17 on MSVC behind a C header. The engine-first order still
holds inside the whole-program port: `tests/sb_api.h` is that header, and the engine can be
proven byte-exact before the UI moves at all.

**Notes for the port.**
- `dist.cpp` keeps its state in globals, and `CBEngine::resetFilters()` calls
  `dist_initdist()`. Two engines in one process, such as the WAV export running while the
  app plays, share that state today.
- The random-pattern generator is `std::mt19937` with `<random>` distributions and a time
  seed. Its exact sequence is not reproducible today, so a C port needs to keep only its
  probabilities, not its bits.
- `sb_engine_load_song()` repeats what `MainWindow::loadRoqFile()` does to the engine. If
  Open starts doing more, the shim has to follow.

## Y4, the calculus (2026-11-09)

Dengo asked what the port actually buys. Measured, one core, file work included: a plain
song renders about 290 times faster than real time, the everything-on patch about 185
times, and every slider gliding at once (the new per-sample path) about 57 times - under
2 % of a core. **A C port would not be faster**: the per-sample code is already C in all
but name, and the same optimiser produces the same machine code. What it would buy:
explicitness, a C interface (a plugin is not a goal), and one language with Dengo's other
work - which matters little, since Claude writes the code either way. What it costs:
weeks without an audible change, a freeze on features, and the 3,500-line interface - the
accessibility work - rewritten with only checks and a screen reader to catch regressions.
**Recommendation: do not port**; keep the C++, keep the tester (it protects C++ changes
just as well), keep the plan below on file. If the port is ever wanted, the engine and
files alone (phases 0-2) are the low-risk part. **Decided: shelved** (Dengo, 2026-11-09),
and `rework` merged into `main`. The "nothing ships until the port is done" rule and the
`cpp-final` fallback tag went with it.

## Y4 plan (written 2026-11-09 for C23 on clang-cl; the standard is now C17)

**C17 changes phase 0:** no compiler switch is needed - Microsoft's C compiler does C17,
with `/experimental:c11atomics` for `<stdatomic.h>` (clang-cl remains an option, proven
byte-exact). Read "C23" below as C17: nothing in the plan depends on a C23 feature.

**The rule for every phase:** the sound and the files do not change. `check_sound.py` passes
all of its renders (201 when this was written) against a manifest frozen before the port
starts, with the C build as the driver - and with `--against` the C++ driver as well, so
both disagree loudly if either drifts. No phase ends with a failing line, and **no
`--update` happens during the port**: if the sound has to change, that is a separate
commit on the C++ side first, accepted, and only then ported.

**Order, and why.** Engine first, because the tester proves it completely. UI last,
because nothing proves it but `check_keyboard.py`, a scripted smoke test and a person with
a screen reader. In between, a checkpoint that can be released: the C engine inside the
existing C++ program.

### Phase 0 - preconditions (no C written yet)
1. **Freeze the sound.** Dengo ear-tests `rework` (LFOs, Apply, the glide), it merges into
   `main`, and the manifest header names that commit. Anything learned by ear is fixed in
   C++ first.
   **Then keep that C++ program as the fallback** (Dengo, 2026-11-09): tag the merged commit
   (`cpp-final`). It is a good program with the sweeps work in it, publishable as it stands
   if the port proves too costly or does not succeed. (A `sweeps-cpp-backup` branch held
   that work until the port was shelved and it merged into `main`; it was then deleted.)
2. **Switch the whole build to clang-cl** (`cmake -S . -B build -A x64 -T ClangCL`) with no
   code change, and require `check_sound.py` to pass unchanged. It did, 189 of 189, in the
   study; this proves it again on today's 201 and makes it the build. README's build line
   changes with it. One toolset for C and C++ alike: the Visual Studio generator cannot mix
   toolsets inside one build, and mixing compilers would leave two float behaviours to prove.
3. **Set the C standard in CMake** (`C_STANDARD 23`, `C_STANDARD_REQUIRED ON`) and check what
   clang-cl is actually given (`/clang:-std=c23`). Check that the UCRT's `<threads.h>` and
   `<stdatomic.h>` work under clang-cl at that standard; if `<threads.h>` does not, the
   writer lock is a Win32 `SRWLOCK`, which is all it needs anyway.
4. **Make the UI checks repeatable before the UI moves.** Promote this session's ctypes
   drivers (the Modulation dialog, Add LFO, the sweep editor's LFO fields, Apply, "(new
   sweep)", F5-F8 in dialogs, the file dialog keeping F5) into `tools/check_ui.py`, so the C
   UI is held to what the C++ UI does. Teach `check_keyboard.py` and
   `check_dsp_ownership.py` to read a list of source files rather than one hard-coded name.

### Phase 1 - the engine in C (about 2,300 lines: CBEngine, AcidFx, Distortion, IIR, dist, freq)
- New `src/core/` (C23): `engine.c/.h` as a struct and functions, with `sb_api.h`'s shape -
  `tests/sb_api.h` is effectively its interface already. `IIR`, `dist` and `freq` are
  C-style now and move nearly verbatim. **`wwelti_IIR_IIRlp3pole` stays byte for byte**
  (invariant 2); only its file and language change.
- **`SongData` becomes fixed-capacity arrays**, so invariant 3 (no allocation on the audio
  thread) is structural. **Capacities, decided (Dengo, 2026-11-09): exactly what the C++
  reader accepts today - 4096 patterns and 100,000 sweeps per song, 64 steps per pattern** - so no file that opens
  now is refused by the C build (old files load into new builds is the property this project
  protects). The cost is small: a pattern is about 450 bytes, so 4096 are under 2 MB, about
  4 MB for the double buffer; a writer copies only the used part. 4096 bars of 16 steps at
  140 BPM is nearly two hours, and 4096 of 64 steps about 7.8 - "an evening to the
  morning", in Dengo's words, which is why nobody needs more; the largest song in the tests and modules has 32 patterns.
  For comparison, the TB-303 held 64 patterns and 7 tracks of up to 256 bars; its tracks
  chained pattern references, where a Screambox song is the track itself (a repeated bar is
  a copy) and a file is one track. The tester's `SB_MAX_PATTERNS` (256) rises to match.
  The readers must refuse a file beyond the capacity with a reason, never truncate it.
- **Concurrency:** `_Atomic` for what is `std::atomic` now - `params_` (as `_Atomic float`
  or the bits in `_Atomic uint32_t`), `liveIdx_`, `readerIdx_`, `transportReset_`,
  `running_`, `displayStep_`, `playMode_`, `patternIdx_` - with the same memory orders, and
  the `SongWriter` protocol unchanged. `writerMu_` becomes `mtx_t` or an `SRWLOCK`.
- **`dist.cpp` keeps its state in globals** and `resetFilters()` calls `dist_initdist()`;
  the WAV export's engine and the playing engine share it today. In C it becomes per-engine
  state. It does reach the output: `dist_dist()`, the stock clipper every song uses, runs
  through a high-pass whose memory (`d_hibi`) is that global, so a WAV export made while
  the song plays disturbs both. Per-engine state fixes that and changes only that case,
  which the tester (one engine at a time) cannot see - say so in the commit. Worth fixing
  in C++ first, in its own commit, if the port is far off.
- **The maths audit, call by call** - the one place a port silently changes bits. Every
  `std::pow`, `std::sin`, `std::exp`, `std::log`, `std::log2`, `std::floor`, `std::lround`,
  `std::clamp`, `std::max/min` gets its exact C equivalent for the argument types it had:
  `std::pow(float, float)` computes in float and is `powf`, a double pair is `pow`.
  `long double` is 64-bit under both MSVC and clang-cl on Windows, so the `wwelti` filter's
  `long double` stays as written. Implicit float/double promotions in mixed expressions
  are kept exactly - a C rewrite that "tidies" `(double)` casts changes output.
- **The lambdas in `process()`** (`voiceNow`, `knobsNow`, `posOf`, `samplesBetween`,
  `evaluateSweeps`, `oscPhaseNow`, `glideSweeps`, `patAt`) become static functions taking
  a small context struct. Same expressions, same order.
- **Proof:** a C implementation of `sb_api.h`'s engine half (`tests/sb_api_c.c`) and a second
  driver target, `screambox_render_c`, linked from the same `render.c`. `check_sound.py
  --exe <c driver>` passes everything; `--against` the C++ driver agrees on every line.
  The allocation check needs a C counterpart of the `operator new` hook: the C engine
  allocates only through one `sb_alloc` function, which the test build counts.
  `check_dsp_ownership.py` is pointed at the C engine's struct fields.

### Phase 2 - the files in C (about 700 lines: RoqIO, CbPresetIO)
- `.roq`, `.hape` and `.cb` reading and writing in C, with `neededVersion` and every
  `if (version >= N)` block mirrored as they are.
- **The writer's text must be the same bytes.** `ostream << float` is `%g` at precision 6;
  the sweep line's floats already use `floatToken` (shortest round-trip by trying
  precisions with `snprintf`), written that way for this port. The tester pins every saved
  `.roq` and `.hape` byte for byte, and every module's re-save outcome.
- **Paths:** the C++ code narrows wide paths and opens with `ifstream`, so a name outside
  the ANSI code page fails today. C with `_wfopen` fixes that. It is a behaviour change
  (for the better); decide it explicitly, not by accident.
- **Proof:** the rest of `sb_api.h` in C; `check_sound.py` with the C driver, all of it.

### Phase 3 - checkpoint: the C core in the current program
- `MainWindow` and `AudioEngine` (still C++) call the C engine and C file code through
  their headers. Nothing a user sees changes; everything audible and every saved byte is
  already proven. **It is not released** - nothing ships until the port is done (Dengo,
  2026-11-09) - but it is a working point to stop at, or fall back to, before the UI moves.
- Proof: `check_sound.py`, `check_keyboard.py`, `check_ui.py`, and the program used for a
  while.

### Phase 4 - the audio device in C (about 300 lines)
- WASAPI through `COBJMACROS`; `IMMNotificationClient` as a hand-written vtable (about 100
  lines); the device-loss reopen of TODO V kept exactly.
- Proof: by use - play, pull the headphones or switch the default device, the program
  reopens and says so once. Nothing automated covers this today; note what was tried.

### Phase 5 - the UI in C (about 3,500 lines; the risk)
- `MainWindow.cpp` splits by job into C files: window layout, step editor, sweep editor,
  Oscillator / Acid FX / Modulation dialogs, file commands and WAV export, the dialog key
  hook. `std::wstring` and `std::vector` in the UI become fixed buffers and a few small
  helpers; the UI thread may allocate, so this is about clarity, not real time.
- **The accessibility rule carries over unchanged** - the same standard controls, the same
  labels, `announceValue()`, the same mnemonics and accelerators, the same retitled labels.
  `check_keyboard.py` must read the new files before the first of them is written.
- **The random-pattern generator** (`std::mt19937` with `<random>` distributions, time
  seeded) cannot be reproduced bit for bit and does not need to be: a small C generator
  with the same probabilities per flavour. Patterns differ from the C++ build's, as they
  differ between any two runs.
- Proof: `check_keyboard.py`, `check_ui.py`, `check_sound.py` (the WAV export path through
  `songLengthSamples` included), and **Dengo with a screen reader through every dialog** -
  the one check nothing else replaces.

### Phase 6 - C only
- The C++ sources, `tests/sb_api_cpp.cpp` and the C++ driver retire; CMake becomes `C`
  only. The 2004 reference sources are not built and stay as they are.
- The manifest is unchanged by the whole port - that is the point of it - and its header
  says which C commit last passed it.

### Decisions to make on the way (not now)
- ~~The fixed capacities~~ - decided: 4096 patterns, 100,000 sweeps (phase 1, above).
- Wide-character paths (phase 2).
- ~~Whether phase 3 ships~~ - decided: nothing ships until the port is done; the fallback
  is the tagged C++ program (phase 0).
- **Not a goal: a CLAP plugin** (Dengo, 2026-11-09).

---

# 2026-11-06 source study — RESOLVED 2026-11-06

**How these were found.** A read of every compiled source, plus a throwaway headless probe
built against the live `src/` (it lived in a session scratchpad and is gone; each item says
enough to rebuild its test). Items marked *measured* were confirmed by that probe; the rest
come from reading the code.

**Suggested order:** L first (it is the root of M's "swept values" problem and changes
what P builds on), then M, O's mnemonics, W's rounding, S, N, P, Q, T, U, R, V.
Each round gets the standard from `CLAUDE.md`: a reconstructed previous engine, a
bit-exact off-path proof per feature, both `tools/` checks.

**Golden renders that legitimately changed**, so nobody mistakes them for regressions:
any render with a sweep (L), with a sweep spanning patterns of different lengths (P), with a
slide on a pattern's last step (Q), with pitch 52 (T), with Fuzz selected (S), and at any
sample rate / tempo where a step is not a whole number of samples (U).

**How the round was verified.** The baseline was the committed tree (`git show HEAD:`), not
a reverse-applied patch — the same thing, obtained without the reconstruction step that
could go wrong. One harness source built twice, the new APIs behind `#ifdef`:

- **Off-path proofs, byte-identical to HEAD:** plain, saw, reverse step (including a tie
  inside a pattern), triplet (straight 16 → triplet 12), mixed lengths (4 / 23 / 16), Rat,
  compressor, ring, the oscillator extras together (PWM LFO, duplet, sub, noise), resonance
  boost, loop-one-pattern, accent squelch 0. Twelve of twelve.
- **Every decided change differs from HEAD**, each in its own render: Fuzz (S), two sweep
  modes (L), last-step slide (Q), pitch 52 (T), 48 kHz (U), a sweep over mixed lengths (P).
- **Unchanged songs write unchanged bytes:** .roq at v9, v10, v11 and v12 and a .hape are
  byte-identical to what HEAD writes.
- Both `tools/` checks pass; Release builds with no warnings; the app starts and closes
  with exit code 0.

---

## L. Sweeps land late, and leak into `getParam()` — RESOLVED 2026-11-06 (decided: fix)

Two bugs with one root: the audio thread writes sweep values into `params_`, the same
atomics the UI reads and the same ones it reads itself only once per block.

1. **A swept value applies up to one audio buffer late** *(measured)*. `process()` derives
   `knobVolume`, `knobCutoff`, `pre_knobRezo`, `knobEnvMod`, `decayMul`/`decayMulInv`,
   `knobAccent` and `two_power_knobTune` once at the top of the block
   (`CBEngine.cpp:612-639`). The sweep is evaluated at the step boundary inside the sample
   loop and stored to `params_` (`:797`), which nothing reads again until the next
   `process()` call. So each note plays its first 0..`frames` samples with the previous
   step's value. Same rendering at 32 vs 512 frames per block: byte-identical with no
   sweep; with a sweep, peak difference 0.26 (Volume), 0.23 (Cutoff), 0.18 (Tuning)
   against a signal RMS of ~0.05. On Tuning that is a pitch blip at every note start; on
   Cutoff it lands on the attack. Same class as the triplet flam in `CLAUDE.md` invariant 6,
   and the K test cannot see it because it compares onsets only.
2. **Swept values become the "slider" value** *(measured)*. Slider 0.15, sweep holding
   0.90: `getParam(Cutoff)` returns 0.90 while playing **and after Stop**, since nothing
   restores it. `saveRoqFile` (`MainWindow.cpp:2159`) and `saveWavFile` (`:1993`) read
   `getParam`, so a song saved after playing stores the swept value as its slider setting
   and sounds different outside the sweep when reopened.

**Approach.**
- Keep sweep overrides in audio-thread-only fields (e.g. `float sweepVal_[7]`,
  `bool sweepOn_[7]`), written only in `process()` — `check_dsp_ownership.py` accepts that.
  `params_` then always holds the user's setting; `baseParam_` and the per-step restore go
  away, Stop needs no restore, and the setParam-vs-restore race closes.
- At the step boundary, evaluate sweeps **before** `triggerStep()` and the reverse-step
  seed (so a reversed step is sized with the swept Decay), then recompute the derived knob
  values right there. One function computes them, called at the block top and per step.
- The legacy `.cb` path (`sweepsOn`, inert) writes `params_` too; move it onto the same
  override fields so it cannot reintroduce the leak if it is ever switched on.

**Acceptance.** Golden modes without sweeps stay byte-identical (same floats in, same
expressions). New: a sweep render must be byte-identical at 32, 64, 512 and 1024 frames per
block. `getParam()` returns the slider value during and after a sweep. `test_a alloc` still
0.

**Done.** As described; `baseParam_` is gone. **Verified:** a two-sweep render (Cutoff and
Tuning) is byte-identical at all four block sizes. A Cutoff sweep rendered at 512 frames
against HEAD rendered at **1** frame - where the old lag is at most a sample - differs by
0.021, against 0.61 for HEAD at 512: what changed is the lag and nothing else. Slider 0.15
under a sweep holding 0.90 reads 0.15 during playback and after Stop. 0 allocations over
400 blocks with every sweep list, holds, slides and FX populated.

## M. WAV export: no sweeps, wrong length — RESOLVED 2026-11-06 (decided: fix)

`saveWavFile` (`MainWindow.cpp:1971`):
- The render engine never receives the interactive sweeps — no `setParamSweeps` call — so
  **an exported WAV has no sweeps at all**.
- Length is `samplesPerStep * 16 * patternCount` (`:1997`), ignoring `lastStep` and
  triplet: 4-step patterns render 4x too long (wrapping into the song again), 64-step ones
  stop a quarter of the way through.
- Parameters are copied from `getParam()` — the swept value until L lands.
- Default path `c:\cb303.wav` is normally not writable; start from the current song's name
  and folder instead.

**Approach.** One helper that copies the whole song (patterns, base params, all seven sweep
lists) into another engine, used here; it also replaces the four hand-written
UI-sweep → engine-sweep conversion loops, which is how this one got forgotten. Expose
`stepSamples` (or the U accumulator) publicly and sum per pattern for the length.

**Acceptance.** Exported WAV of a sweep song equals a harness render of the same song; length
equals the summed pattern durations for mixed lengths and triplet patterns.

**Done.** `pushSweeps()` / `pushAllSweeps()` are the only conversion now, used by the editor,
.roq load, pattern edits and the export. Length is `CBEngine::songLengthSamples()`; the
render streams to disk in 512-frame blocks and reports a failed write (deleting the partial
file) instead of announcing success. **Verified:** `songLengthSamples()` equals the
sequencer's own count (185 142 samples for a triplet 12 / 4 / 16 song at 48 kHz). *Not*
verified: an export from the running app compared against a harness render - the export
path is UI-driven.

## N. Pattern insert / delete shift sweeps and move the playhead — RESOLVED 2026-11-06 (decided: fix, clamp)

- `addPatternAfterCurrent` / `deleteCurrentPattern` (`MainWindow.cpp:1420`, `:1692`) never
  remap sweep endpoints, though sweeps address patterns by index (the Cut comment at
  `:1445` says exactly this). Every sweep after the edit point silently moves.
- Both call `engine_.selectPattern(editingIdx_)`, which is the **playback** index. During
  playback, Ctrl+Shift+P jumps playback to the new empty pattern; delete jumps it to
  whatever now sits at that index. (From reading; not measured.)

**Decided (Dengo):** on delete, endpoints inside the deleted pattern **clamp to the nearest
surviving boundary**, sweeps lying entirely inside it are removed, and the status line says
how many sweeps were adjusted or removed. On insert, endpoints at or after the new slot
shift by one.

**Also decided:** **Import .cb and New random pattern (Ctrl+N) clear the interactive
sweeps**, like Start new track — both are "new song" actions, and today they keep the old
song's sweeps, pointing at patterns that may not exist. Say so in their status messages.

**Approach.** Engine API `insertPatternAfter(idx)` / `deletePattern(idx)` that take the
index explicitly and adjust `patternIdx_` so the *playing* pattern stays the same pattern.

**Done, differently in one respect.** Adjusting `patternIdx_` from the UI thread races the
audio thread, which also writes it at every pattern change. Instead each pattern has a
stable id (`SongData::patIds`) and the audio thread finds the playing pattern again by id
whenever a commit lands - see `CLAUDE.md` invariant 6. **Verified:** playing index 2,
insert a pattern before it → 3, delete the first → 2; deleting the playing pattern continues
with the one in its place. Stress: 60 000 32-frame blocks against ~70 000 random inserts,
deletes, edits (with last-step slides and triplets), sweep writes and song replacements,
plus a transport thread: finite and inside ±1 throughout.

## O. Sweep editor — RESOLVED 2026-11-06 (decided: fix, plus per-sweep hold and swept labels)

1. **Two mnemonic collisions** *(measured)*: `Sweep &number` / `End patter&n` (Alt+N) and
   `Start s&tep` / `Pas&te` (Alt+T), `resource.rc:144/154`, `148/166`. Each cycles focus
   instead of activating. One assignment that works: `En&d pattern`, `Save to &file...`,
   `Pa&ste`. **Extend `tools/check_keyboard.py` to every DIALOGEX**, not just the main
   window — it passed with these in place. The oscillator and Acid FX dialogs are clean today.
2. **No way to edit an existing sweep.** Only Add and Remove; Add overlap-checks against the
   very sweep being viewed. Add an **Update** button that replaces the selected sweep,
   overlap-checked against the others.
3. **A backwards sweep is accepted and never plays.** `sweepsOverlap` swaps the ends, so Add
   accepts end-before-start, but the engine's `songPos < start || songPos > end` never
   matches it. Reject with a status message.
4. **Cancel behaves as OK** — Add/Remove mutate `paramSweeps_` in place. Snapshot on
   `WM_INITDIALOG`, restore on `IDCANCEL`.
5. Value and count labels change via `SetWindowTextW` without `EVENT_OBJECT_NAMECHANGE`.
6. A sweep naming a pattern that does not exist (old `.hape`, or orphaned) leaves
   `CB_SETCURSEL` failed, and Add then stores pattern −1. Validate in `readSweepFromDialog`.
7. **Per-sweep "hold end value" — decided (Dengo).** A checkbox in the editor: *Hold end
   value until the next sweep*. Off is today's behaviour (snap back to the slider). With it
   on, the end value stays until the next sweep on the same slider starts.
   **Format:** an appended eighth token on the sweep line (`... endVal hold`). Older readers
   use `istringstream >>` for seven fields and ignore the rest, so the line stays readable
   both ways — the step-line trick. `neededVersion` stamps **v13** only when some sweep
   holds. `.hape`'s pinned `kHapeVersion` moves too, but only when a held sweep is written
   (same "what the file needs" rule).
   Engine: "hold" means the most recent sweep that ended before `songPos` and holds applies
   when no sweep covers it. Order matters across a song wrap — define it as "since the song
   started", so pattern 1 before the first sweep always gets the slider value.
8. **Swept label on the main window — decided (Dengo).** A slider with sweeps says so in its
   value label: `Cutoff +15% (2 sweeps)`. Updated when sweeps change, never per step, so it
   does not chatter. Today nothing tells a screen-reader user that moving the slider is
   undone at the next step.

**Done, all eight.** The editor is now: number, start pattern / step / value, end pattern /
step / value, **Hold** (`Alt+H`), then Add / **Update** (`Alt+U`) / Remove / Copy /
**Pa&ste** / **Save to &file** / Load. `End pattern` moved to `Alt+D`. Hold is subclassed so
Enter toggles it - in a dialog, Enter otherwise presses OK and closes the editor. A sweep
naming a missing pattern is shown at the nearest real one with the status saying so.
**One deviation on 8:** the count goes in the slider's **name** (`Cutoff (2 sweeps)`, both
the label and the trackbar text), not its value label, because the name is what a screen
reader reads on focus and the value label is a separate static nobody lands on.
**Verified:** the new seventh check in `check_keyboard.py` failed on the old dialog (both
collisions) and passes now; `.roq` and `.hape` round-trip a held sweep and stamp 13; a
seven-token line loads as not held; songs without holds still write their old bytes. Hold in
the engine: between a held sweep ending at 20 % and the next sweep, the level holds at 0.200
of the slider's; without hold it snaps back. *Not* verified: any of the dialog by hand.

## P. Sweep pace — RESOLVED 2026-11-06 (decided: even per step)

Sweep progress is linear in *pattern units* today: each pattern counts as 1.0 whatever its
length or triplet flag (`songPos = patIdx + step / stepsPerPat`). A sweep over a 4-step
pattern then a 16-step pattern spends half its travel in the short one.

**Decided (Dengo): even per step.** Each step advances a sweep by the same amount, which is
also how the editor names endpoints. **Approach:** a prefix-sum of pattern lengths
(`patStartStep[]`) computed in `SongWriter::commit()` on the UI thread and stored in
`SongData`, so the audio thread stays allocation-free; endpoints and the current position
become absolute step numbers. Keep the per-endpoint clamp from B. Sweeps inside one pattern,
or across patterns of equal length, render exactly as before — say so in the acceptance test.

**Done.** **Verified:** a Volume sweep 0 → 100 over a 4-step and a 16-step pattern gives each
of the 20 steps exactly k/19 of full level (worst deviation 0.0000). For 16-step patterns the
old pattern-unit arithmetic was exact (steps are powers-of-two fractions), so the new integer
form gives identical values there; for other equal lengths it can differ in the last bit.

## Q. Slides across the bar line — RESOLVED 2026-11-06 (decided: glide across (option A))

*(measured)* A slide on a pattern's last step never glides, and the bar line clicks:
- `triggerStep` gets `nextStep` from the **same** pattern (`CBEngine.cpp:716`), so the
  slide decision looks at this pattern's step 1;
- the glide target peeks at the **next** pattern (`:1065-1071`) — and in loop-one-pattern
  mode that is the wrong pattern;
- neither matters, because `resetFilters()` at step 0 (`:719`) clears `bNoteSlide_` /
  `boxSlide_` and forces `boxGate_ = 0` and the filter memories while the gate is still
  fully open. Largest sample-to-sample jump at the wrap: 0.065 with the slide, 0.0078
  without.

Random patterns put a slide on the last step 5–60 % of the time depending on wildness, so
this is common. 2004 was broken here too, differently: its `CBReset` left `bNoteSlide` set
and zeroed `boxOscFrq`.

**Decided (Dengo): option A.** The slide glides into step 1 of the pattern that will actually
play next (same pattern when looping or in pattern-only mode). **Approach:** resolve "the
next step" once — next pattern's step 0 in FullSequence, own step 0 in CurrentPatternOnly —
and use it for `bSlideNext_`, the glide target and `audibleNoteLength`'s tie walk (which
must then walk into the next pattern, with its own `samplesPerStep` if the triplet flag
differs). At a pattern start that continues a tie, skip the parts of `resetFilters()` that
would break the note (filters, oscillator phases, gate, slide state); the rest of the reset
still runs. **Invariant 1 still holds** — the PWM LFO phase stays out of it.
**Acceptance:** a slide on the last step glides with no discontinuity at the wrap; patterns
without a last-step slide are byte-identical.

**Done.** `nextPlayedStep()` is the single look-ahead; the reset is skipped during a tie, and
in the end "the rest of the reset" turned out to be nothing - every field it touches belongs
to the note. **Verified:** largest sample jump at the bar line 0.064 against 0.065 within the
same note (it was 8× the within-note figure before); with the filter open, a slide up an
octave crosses zero 15 times in the first step against 21 for a retrigger at the target, so
the pitch arrives gradually. Looping one pattern with a last-step slide stays bounded. All
twelve no-last-slide golden modes byte-identical.

## R. MIDI input — RESOLVED 2026-11-06 (decided: remove)

*(measured)* `noteOn`/`noteOff` store `midiNote_`/`midiVel_` (`CBEngine.cpp:273-282`) and
`process()` never reads them: a MIDI keyboard produces silence. The README claims otherwise.

**Decided (Dengo): remove it.** `src/MidiInput.*`, its CMake entries, `winmm` if nothing else
needs it (the sequencer does not), the `WinMain` start-up and the `MainWindow` member, the
engine's `noteOn`/`noteOff`/`midiNote_`/`midiVel_`, and the README "MIDI in" bullet. The
engine change touches no DSP path, so all golden modes stay byte-identical.

**Done**, `winmm` included - nothing else used it. Golden modes byte-identical.

## S. Fuzz leaves DC at rest — RESOLVED 2026-11-06 (decided: fix)

*(measured)* `hardClip(v + bias, 0.55) - bias * 0.5` (`Distortion.cpp:122`) outputs +0.09
when the input is silent: mean 0.0900 over a second of rests. With the ring modulator on
(300 Hz, amount 1) that becomes a 300 Hz tone **during rests** at RMS 0.064 — louder than
the notes. The combo even calls Fuzz "gated".

**Approach.** A one-pole DC blocker (~10 Hz) after the Fuzz clipper — what a real pedal's
output coupling capacitor does. Keeps the asymmetric voicing during notes. Classic is
untouched; the other three models are untouched.
**Acceptance:** rests through Fuzz have |mean| < 1e-4; with the ring on, rest RMS < 1e-4.

**Done, without the DC blocker.** The offset was a constant - the clipper's output for
silence, `hardClip(bias) - bias/2` - so the fix subtracts exactly `hardClip(bias)` instead.
Same waveform, same asymmetry, zero at rest, and no filter state: a DC blocker's state would
be zeroed by the per-pattern reset and make a small thump on every bar line. The combo now
calls it "Fuzz (asymmetric, germanium)" - it was never gated. **Verified:** Fuzz + ring over a
bar of rests is **exactly** 0.0, not merely below 1e-4. A blocker for the *signal-dependent*
DC that asymmetric clipping makes while notes sound is a separate, audible choice, not made.

## T. Note-table typo inherited from 2004 — RESOLVED 2026-11-06 (decided: fix)

`cNoteFreq[52] = 2389.016` should be **2489.016** (`CBEngine.cpp:30`; the reference
`SilverBox.cpp:35` has the same value). The UI calls it D#4; it plays ~71 cents flat.
Chromatic random and transposition both reach it. Correct it and comment the deviation from
2004 at the table, so nobody "restores" it from the reference source.

**Done.** Verified: the pitch-52 render differs from HEAD; nothing else does.

## U. Exact tempo — RESOLVED 2026-11-06 (decided: fractional step counter)

Step length is truncated to whole samples (`stepSamples`, `CBEngine.cpp:525`), so tempo runs
slightly fast whenever `sr·60/(bpm·4)` is not whole: at 48 kHz / 140 BPM a step is 5142
instead of 5142.86 samples — ~50 ms of drift over 5 minutes against a DAW, and 12 triplet
steps (82 284) do not equal a 16-step bar (82 272). K's verification ran at 44.1 kHz / 140,
where both happen to be exact.

**Approach.** A fractional accumulator; each step's length is the rounded difference of
cumulative positions, so the bar total is exact. The gate threshold, `audibleNoteLength` and
the slide rate keep using the current step's integer length. The accumulator resets on
transport start, and on tempo change it re-anchors rather than jumping. **44.1 kHz / 140 must
stay byte-identical** (steps already whole) — that is the off-path proof.

**Done.** **Verified:** ten bars at 48 kHz / 140 BPM take 822 857 samples straight *and*
triplet, against an exact 822 857.1. Every 44.1 kHz golden mode is byte-identical.

## V. Audio device loss — RESOLVED 2026-11-06 (decided: reopen automatically)

Any WASAPI failure in `AudioEngine::renderThread` (unplugging headphones gives
`AUDCLNT_E_DEVICE_INVALIDATED`) `break`s the loop and the thread exits silently; the status
line keeps saying "Playing". **Decided (Dengo):** reopen the current default output device
and announce `Audio device changed — now <name> at <rate> Hz.` through `announceValue`. If the
sample rate changes, the engine gets the new rate on the audio thread before it next renders.
Also: `sampleRate_` and `lastError_` are read by the UI thread while the render thread writes
them; make the rate atomic and hand errors over safely.

**Done.** The render thread runs sessions: open the default endpoint, render, and on any
WASAPI failure - or a timeout on which the padding query fails, since a vanished device can
simply stop signalling - close everything, announce, and retry every half second. The rate
is an atomic, the name and error are copied out under a mutex `process()` never sees. Also:
an extensible mix format is now checked to be float, not just assumed. **Also, decided after the round (Dengo):** it follows the Windows default
output. An `IMMNotificationClient` raises a flag on a default-render change and the session
reopens on the new device at once, with no "lost" phase. **Not verified:** pulling a device
out or switching the default - both need hardware and a person.

## W. Small fixes — RESOLVED 2026-11-06

- **Slider positions drift on load** (simulated in float32): `syncControlsFromEngine`
  (`MainWindow.cpp:1369`) and the initial slider position (`:337`) truncate `v * 100`.
  Positions ±53 and ±59 come back as ±52 / ±58, and `updateSliderValue` then writes the
  drifted value into the engine. Use `lroundf`. Negative detune in the oscillator dialog has
  the same problem (`+0.5f` then truncation, `:914`).
- Status line says `step %d/16` whatever the pattern length (`:2388`).
- **Stale comments:** `CbStep::reverseStep` (`CBEngine.h:18`) and `stepReverse_` (`:303`)
  still describe the pre-J EnvMod sign flip; `dupletFactor` (`CBEngine.cpp:592`) says the
  sub follows the duplet detune, the code does not; the `CbParam` ranges call Tuning,
  Resonance and EnvMod 0..1.
- **Accent squelch slider goes down to 0 — decided (Dengo).** The engine and docs already
  say 0..2 with "0 = no stacking", but the slider reaches only 1..2 (`onSliderScroll`) and
  `syncControlsFromEngine` clamps below 1, so a loaded value under 1 is shown wrong and
  overwritten the moment the slider moves. Make the slider 0..200 with position = `sq * 100`,
  so 100 stays the stock default and the readout says what the engine gets. The file already
  stores the real value, so no format change and no saved song moves.
- **Snapshot hardening:** the writer side of the double-buffer relies on the mutex happening
  to act as a full fence between `commit()`'s `liveIdx_` store and the next writer's
  `readerIdx_` load. Make both `seq_cst`; it costs nothing on the UI thread.

**Done, all of W**, including the accent squelch slider at 0–200. **Verified** for the
rounding by the same float32 simulation that found it; the rest is by reading.

---

# Resolved

## E. Sweep parameter ordering — RESOLVED 2026-09-03, no code change needed

**Verdict: the two tables are both correct and must not be reconciled.** Documented in
place rather than "fixed"; comments added at both tables and in `CBEngine.h`.

Interactive sweeps use UI slider order `{Volume, Cutoff, Resonance, EnvMod, Decay, Accent,
Tuning}`; legacy `.cb` sweeps use 2002 tab order `{Volume, Tuning, Cutoff, Resonance,
EnvMod, Decay, Accent}`. Tuning is last in one and second in the other. Three independent
sources agree that the legacy order is the file's real order:

1. `reverse/parameter_map.md` — the Delphi `Form1.dfm` `TPageControl` tab declaration
   order: `TabSheet1/volume`, `tsTune`, `tsCutoff`, `tsRezo`, `tsEnvMod`, `tsDecay`,
   `tsAccent`.
2. `reverse/cb_schema.md` — Ghidra of `CREAKBOX.EXE`: "the seven TStringGrid tabs
   (Volume / Tune / Cutoff / Rezo / EnvMod / Decay / Accent)".
3. The loader: `CbPresetIO::loadCb` reads the tail grids sequentially into `bp0[t]`/`bp1[t]`
   with `t` = file order and no remapping, and `MainWindow` copies `bp*[i]` → `sweeps[i]`
   one-for-one. Index equals file tab index from disk all the way to the apply site.

"Correcting" either table would silently mis-map every sweep in all 56 shipped `.cb`
modules.

**Second finding, and the reason the originally planned test was impossible.** The plan
said to verify by loading a `.cb` with tuning sweeps and listening. That cannot work: the
legacy apply path is **inert**. `setSweeps` has exactly one caller
(`MainWindow.cpp:1470`), which passes `enabled=false` deliberately, and `sweepsEnabled_`
defaults to false — so `if (sweepsOn)` never runs. Legacy sweeps are parsed and
round-tripped but never heard.

The rationale is at the call site and in `reverse/cb_schema.md`: the per-tab value curves
are not fully decoded, and applying the raw values silences many real presets (row-0 values
of 2 against a 0–100 range drop Volume and Cutoff to near zero mid-pattern). Both the
inertness and the reason are now commented at the apply site so nobody deletes it as dead
code or re-enables it without decoding the curves first.

**If legacy sweeps are ever turned on**, that is its own piece of work: decode the
per-parameter curves (`sin(x·π/2)`, the 25–100 % remap for Rezo, etc. — partially recorded
in `cb_schema.md`), and decide whether they *replace* slider values as in 2002 or
*modulate* them, which is the more useful behaviour and what the call-site comment suggests.

**Verified:** comments-only change; the full round-2 engine suite still passes, including
all three bit-identical proofs, so the engine is unchanged.

**Dengo's ruling on why they stay parsed at all:** legacy sweeps live in the file because
they are needed to convert an original `.cb` should anyone ever have one with sweeps in it.
That is a conversion obligation, not a playback feature — which is precisely why parsing
them and never applying them is the correct state, and why the parse path must not be
deleted as dead code.

---

## A. Audio-thread allocation — RESOLVED 2026-09-03

**`process()` now performs zero heap allocations and takes no lock.** It previously
deep-copied the whole song plus both sweep systems under `patternMu_` on every callback.

**What changed.** Everything the audio thread reads about the song moved into a
`SongData` blob, double-buffered in `CBEngine`:

- `process()` publishes the buffer index it intends to read into `readerIdx_`, re-reads
  `liveIdx_`, and retries if a writer flipped in between. Publish-then-recheck is the
  load-bearing part: without it a writer could pick the very buffer the reader had just
  chosen but not yet announced. It then takes a `const SongData&` — no copy, no lock.
- Writers go through `SongWriter` (RAII): take `writerMu_` — which the audio thread never
  touches — wait for `readerIdx_` to leave the idle buffer, copy live into it, mutate,
  then `commit()` flips `liveIdx_`. All waiting is on the UI thread and is bounded by one
  audio block. No commit means nothing is published, which is what every bounds-check
  early return wants.
- `patternIdx_` became a plain `std::atomic<int>`. It is the one field *both* threads
  write (the sequencer advances it, the UI selects a pattern), so it deliberately sits
  outside `SongData` — and that is what removed the audio thread's **second** lock, the
  one inside the pattern-advance path.
- `pat` is now a `const CbPattern*` into the live buffer rather than a per-block copy.

Copying live into idle reuses the idle buffer's existing vector capacity, so steady-state
editing does not allocate on either thread.

**Verified** — and the bit-exact standard deliberately did not carry over, as predicted:

1. **Zero allocations.** `test_a alloc` installs an `operator new` hook armed only around
   `process()` and renders 400 blocks of a 32-pattern song with all seven sweep lists
   populated. Result: **0**. The **negative control matters more than the result** —
   the same test against the pre-change engine reports **3200 allocations over 400
   blocks, exactly 8 per block**, matching the diagnosis (one patterns vector plus seven
   sweep vectors). A test that cannot fail proves nothing; this one fails on the old code.
2. **No torn state.** `test_a stress` runs a writer that swings the song between 1 and 64
   patterns so the vector reallocates on essentially every commit, against a render thread
   using 32-frame blocks. 60 000 blocks against 11 004 concurrent commits: no non-finite
   samples, nothing outside ±1, no crash. Reallocation is what makes this sensitive — a
   reader holding a pointer into a buffer being rewritten would be dereferencing freed
   storage.
3. **Bit-exact under quiescence.** `compare_golden.py` renders all 11 harness modes and
   compares byte-for-byte against `golden_preA/`, captured from the engine immediately
   before the change. All 11 identical. With no writer there is no flip, so the audio
   thread sees exactly what it saw before and the DSP must agree exactly.

The round-1 and round-2 suites still pass in full, so nothing else moved.

**Still open, and adjacent:** `setRunning()` calls `resetFilters()` from the UI thread
while the audio thread may be mid-block, which races on the DSP state (`boxOsc_`, the
filter memories). That predates this work and is untouched by it. It is unlikely to be
audible — the values are overwritten immediately — but it is a genuine data race that a
sanitiser would flag, and the fix is a "please reset" atomic flag the audio thread acts on
at the top of its next block.
**Promoted to its own item — see I below.**

---

## B. Variable pattern length — RESOLVED 2026-09-04

Patterns are now **1 to 64 steps**, set per pattern on a **Pattern length** combo (`Alt+G`)
placed just before the step list in tab order. `.roq` **v10** carries it.

**Almost nothing had to change in the engine.** `CbPattern` always carried 64 steps and the
sequencer always looped at `lastStep`, derived song position from `lastStep + 1` and picked
the next step from it. Two things were missing: a control that could say anything but 16,
and a writer that could save it.

**What was actually broken.** `.roq` round-tripped the `lastStep` *number* while both loops
read and wrote exactly 16 step rows. A 23-step pattern would have saved its declared length
and silently lost steps 17–23 — worse than refusing, because the file still looks valid.

### The format: why v10 is not just another bump

Every bump up to v9 appended a field, which a reader can default. v10 changes the *number of
step rows in a pattern block*, and a row count is positional: an old reader handed a long
pattern runs off the end of it and every field after it — including the sweep block — is
garbage.

So **the writer stamps the lowest version that can represent the song**, not the newest
(`neededVersion` in `RoqIO.cpp`). A song whose patterns are all 16 steps writes exactly the
bytes v9 wrote and still opens in an older build; only a song that actually uses another
length declares v10 and gives that up. The header number means "what this file needs", not
"what wrote it". Verified against all 25 shipped modules: every one still writes as v9.

`.hape` was pinned to its own constant at the same time. It shares the counter's history but
its schema has never changed, and letting it ride the bump would have made sweep exports
unreadable by older builds to describe a change that did not happen to them.

### The two things that were not obvious

1. **A sweep endpoint has to be normalised by the length of the pattern it names**, not by
   the length of the one playing. That was the same number for every pattern until now.
   Once lengths differ, "pattern 0, step 15" divided by a 4-step pattern's length is song
   position 3.75, and the sweep stays active three patterns too long. Fixed with `posOf` in
   `process()`, which also clamps an endpoint into its own pattern, so shortening a pattern
   under an existing sweep does not fling the endpoint downstream.
2. **`sweepsOverlap` mapped (pattern, step) to `pat * 16 + step`.** With a longer pattern in
   play, pattern 0 step 20 and pattern 1 step 4 both map to 20, so the overlap check
   compares the wrong intervals. Stride is now 64 — the maximum length, which is all the
   mapping needs to stay collision-free and increasing.

Also length-aware now, each of which would otherwise quietly write to steps the pattern does
not play: transpose, randomise-current-pattern (it keeps the length — it replaces notes, not
shape), the per-step toggles, the step list, and the sweep editor's step combos, which are
sized to the pattern selected beside them and refill when that selection changes.

**Shortening is not destructive within a session** — the steps past the new end keep their
notes and lengthening brings them back. Saving is where they go, which is what makes a short
pattern short on disk. `.cb` stays 16 and cannot be otherwise; commented at `saveCb`.

### Verified

- **`len4`, `len23`, `period4`, `period7`, `period16` and `sweepnone` render byte-identical
  to the pre-change engine.** This is the strongest statement available here: those modes
  use no parameter that did not already exist, so an older binary renders them too, and
  bit-equality says the sweep-positioning edit did not leak into the ordinary path.
- **The sequencer loops where it says it does.** One note on step 0, rests elsewhere; the
  per-step energy is then periodic, and the measured period is 4, 7 and 16 respectively. The
  period search takes the *shortest* period that fits, not the best-scoring one — a
  4-periodic signal scores just as well at 8 and 12, and without that the test happily
  passes a pattern of the wrong length. It did, first time round.
- **A sweep stays inside its pattern.** A 16-step pattern followed by a 4-step one, with a
  Cutoff sweep over pattern 0 only: steps 2–4 of the short pattern are unaffected
  (5.8e−07), where the pre-change engine swept straight through them (1.4e−01). The first
  step of the short pattern *does* differ, by 1.5e−01 — that is the filter carrying state
  across the boundary, and it is checked for being a ring-out rather than assumed to be one:
  it collapses 2236x within the first eighth of the step, where a still-running sweep would
  persist.
- **Format.** A mixed 23/5/16 song round-trips all three lengths, all 23 steps of the long
  pattern with flags, the patterns *after* it still aligned, and the sweep block after them
  still read correctly — that last one is what catches a reader losing its place. Steps past
  a pattern's end load as rests. An all-16 song still declares v9 and still writes 16 rows.
  All 25 shipped modules load, re-save and remain v9.
- Everything from rounds 1–5 still passes: golden bit-exactness, J, G, bounds, `test_a`
  alloc and stress. Release builds with no warnings; the app starts and closes cleanly.

**Worth a listen, not a bug:** 15 against 16 is the obvious first thing to try — it walks the
accents around the bar. A short pattern under a long sweep is the cheap way to a phrase that
never repeats the same way twice. Whether the length combo announces usefully as you arrow
through 64 entries is a screen-reader question this cannot answer.

---

## C. `RoqSong::impulsive` — RESOLVED 2026-09-04

Moved into a clearly-marked legacy section at the end of `RoqSong`, with the reason it
cannot simply be deleted written next to it and at the write site.

**The obvious fix would have been a bug.** The original plan said "stop writing it". `.roq`
is a *positional* line format — `readFloat` consumes one line at a time in a fixed order —
so dropping the line shifts every field after it by one, and a reader would parse
`accentSquelch` as `impulsive` and mis-read the whole rest of the header. **The constant `0`
is load-bearing padding and has to keep being written.** The change is documentation only.

**Surveyed first, as Dengo asked.** All 25 shipped modules: two are v1 and predate the field
(`Crazy wow.roq`, `bubblebath.roq`), the other 23 are v2 or v3 and every one stores `0`. It
has never carried a non-zero value, so nothing is lost by ignoring it — which is exactly why
it can be documented as inert rather than having to be interpreted.

**Verified:** `test_roq` still round-trips every version and all 25 modules still load.

---

## F. Mnemonics that could never fire — RESOLVED 2026-09-04

`Alt+A`, `Alt+S` and `Alt+R` now reach the `&Accent` slider, the `Re&sonance` slider and the
`&Run` button, which is what their underlines always claimed.

### It needed both halves, and the second was the hidden one

Moving the accelerators off those chords was the obvious half. But **two of the three chords
were claimed by a second label as well**, and a duplicated mnemonic does not activate
anything either — Win32 cycles focus between the matches. So the duplicates had to go too:

| Chord | Was | Now |
|---|---|---|
| `Alt+A` | `IDM_STEP_TOGGLE_ACCENT`, plus `P&attern:` and `&Accent (this step)` | the `&Accent` slider, alone |
| `Alt+S` | `IDM_STEP_TOGGLE_SLIDE`, plus the `&Step sequencer` heading | the `Re&sonance` slider, alone |
| `Alt+R` | `IDM_EDIT_RANDOM_STEP` | the `&Run (F5)` button |

The step accelerators moved to one family: **`Ctrl+Shift+A` / `S` / `I` toggle accent, slide
and reverse on the focused step, `Ctrl+Shift+G` randomises it.** `Alt+I` (reverse) collided
with nothing, but it moved anyway — three sibling actions on two different modifiers is
worse to learn than one rule, and freeing `Alt+I` turned out to be needed below.

The controls that lost an underline — the pattern combo, the three per-step checkboxes, the
step-sequencer heading — have none now rather than a false one. All are Tab-reachable, the
checkboxes have the `Ctrl+Shift` chords from anywhere, and `Ctrl+Left/Right` changes pattern
without focusing the combo at all.

### Two more the new check found, which were not in this item

Writing the check first (`tools/check_keyboard.py`) turned up two live faults nobody had
listed:

- **`Alt+E` never reached the EnvMod slider.** The menu bar is consulted *before* any
  control, so it only ever opened the **Edit** menu. Nothing in "EnvMod" was free — until
  Wildness gave up `D` by moving to `W&ildness` (`Alt+I`, freed above). EnvMod is now
  `EnvMo&d`. A chain of three moves to fix one silent failure, which is what "the window is
  out of letters" actually costs.
- **`Alt+P` was on two menus**, `&Pattern` and `&Play`, so it cycled between them instead of
  opening either. `Play` is now `P&lay` (`Alt+L`).

And one benign ambiguity worth removing while there: **every parameter slider had its
mnemonic on two controls**, because the trackbar carried the same `&`-bearing text as its
label. Both matches happened to land on the same trackbar, so it worked — but "harmless
duplicate mnemonic" is precisely the shape of bug this window has been bitten by twice. The
trackbars now carry the name without the ampersand, which keeps the accessible name and
removes the ambiguity.

### The check is the real deliverable

`tools/check_keyboard.py` fails on all four ways a key can silently do nothing here, each
of which has shipped at least once:

1. a chord bound twice (first entry wins — `Ctrl+Shift+N` was);
2. an unmodified key bound as an accelerator (dead on every control — `Space` was);
3. two controls sharing a mnemonic (cycles focus — `Alt+V` did);
4. a mnemonic shadowed by an accelerator or by the menu bar (never fires — this item).

**No plain `Alt+letter` is bound as an accelerator any more**, and the check enforces it.
That is what makes class 4 unrepeatable rather than merely fixed.

**Verified:** 32 accelerator chords, no duplicates, no unmodified bindings, no plain
`Alt+letter`. 18 main-window mnemonics, all unique, none shadowed, none on a menu-bar letter.
Free letters left: **J, K, Z**.

---

## G. Distortion block — RESOLVED 2026-09-04

`src/Distortion.{h,cpp}`, a sibling module beside the untouched stock `dist.cpp`. Five
models — **Classic / Rat / Screamer / Dist+ / Fuzz** — with per-block coefficients
(`cbDistPrepare`) and a per-sample path (`cbDistProcess`): optional highpass, optional
pre-lowpass standing in for the RAT's LM308 slew limit, drive, per-type clip, post-lowpass
tone, level, NaN guard, clamp. `Ctrl+D` opens the dialog; the button carries no mnemonic
because the main window has no free letter (finding F).

**Type 0 (Classic) calls the stock `dist_dist()` verbatim.**

**Completed since:**

1. **`.roq` v9** — `distType`, `distGain`, `distTone`, `distLevel` appended after
   `resoBoost`, guarded `if (version >= 9)`, defaulting to Classic / 0 / 0.5 / 1.0.
   **v9 is distortion only.** B's variable-length patterns are a *later* bump, not a second
   definition of v9 — keeping them apart means neither has to wait for the other, and one
   version number never means two things.
2. **`resetTo303()`** returns type to Classic and gain/tone/level to neutral. Per-step
   reverse is still deliberately untouched — that is pattern data.
3. **F1 reference** lists `Ctrl+D`, and the `Alt+I` line now describes reverse step as an
   envelope that runs backwards rather than "closes then opens" (see J).
4. **Docs** — README has a "Distortion dialog" section with the model table and the level
   figures below.

**Verified** (`analyze_g.py`, one render per model rather than one combined pass):

1. **Classic really is inert.** `dist0` — Classic with gain 0.6, tone 0.5, level 1.0 —
   renders **byte-identical** to a plain default render. The three knobs do nothing while
   Classic is selected, proven rather than asserted. All 10 unchanged golden modes also stay
   byte-identical with `Distortion.cpp` linked in.
2. **Every model changes the sound, and they are distinct.** At gain 0.6, difference from
   Classic as a fraction of signal RMS: Rat 368 %, Screamer 134 %, Dist+ 361 %, Fuzz 438 %;
   and all six pairs of models differ from each other.
3. **Bounded at maximum drive.** 20 s renders at gain 1.0 for all five types: zero
   non-finite samples, peak ≤ 0.85, all audible.
4. **Format.** `.roq` v9 round-trips all four fields; every older module (v1/v2/v5/v6/v7)
   loads with the block at Classic/neutral; the file declares version 9.

**Levels, measured, because this is the thing that would surprise someone:**

| | RMS at gain 0 (the default) | vs Classic |
|---|---|---|
| Classic | 0.073 | — |
| Rat | 0.068 | 0.9× |
| Screamer | 0.025 | **0.3×** |
| Dist+ | 0.072 | 1.0× |
| Fuzz | 0.123 | 1.7× |

So **selecting a model is not a volume jump** — the big numbers in test 2 come from
deliberately winding the gain up, where hard clipping stops output depending on input and
the level lands ~6× higher. That is what the level knob is for.

**Worth a listen, not a bug:** Screamer at 0.3× is the quiet one. Its high-pass keeps bass
out of the clipper at 720 Hz, which is authentic Tube Screamer behaviour and is *why* it
makes a line cut — but on a bassline that is most of the energy, and it may read as "I
picked Screamer and it went quiet". Lowering that corner to ~250 Hz would make it a
bass-friendly variant rather than a faithful one. Dengo's call after hearing it.

**Also corrected while here:** the README and `CLAUDE.md` both claimed `.roq` was
"forward-tolerant in both directions". It is not, and never was. A build rejects a file
whose version exceeds its own (`unsupported .roq version`) rather than mis-parsing it —
correct behaviour for a positional format, but the docs overstated it. Only the *step line*
is tolerant both ways.

---

## H. Acid FX block — RESOLVED 2026-09-04

The distortion dialog is now **Acid FX** (`Alt+X`, still `Ctrl+D`), holding three effects in
series after the filter and the volume envelope: **distortion → compressor → ring
modulator**. New sources `src/AcidFx.{h,cpp}`; `.roq` **v11** carries the four parameters.

The rename paid for itself: "Distortion" had no free mnemonic letter left, so that button was
Tab-only. "Acid FX" freed `X`.

### H1. Compressor

Threshold and depth, exactly as specified. Depth 1.0 is a 3.3:1 ratio and no further — glue,
not a limiter. Auto make-up keeps it from reading as a volume knob: the curve pivots at full
scale, so quiet material comes up and the loudest material stays put.

**The follower has no attack time, and that turned out to be load-bearing rather than a
simplification.** The first version used a 5 ms attack, on the reasoning that letting the
transient through is what stops a compressor eating the accents it is meant to even out. It
also lets an individual sample sit *above* the envelope that graded it, and the make-up gain
then pushes it past full scale: **0.19 % of samples clipped at an ordinary setting**
(threshold 0.3, depth 0.8). Measured, not theorised — the bounds test caught it.

With a peak follower (`env = max(|x|, released)`) the bound becomes provable instead of
measured. Given `env >= |x|` and a curve that pivots at 1.0, an input inside ±1 cannot leave
the stage outside ±1, for any threshold and any depth. The test now asserts **zero** clipped
samples rather than `|x| <= 1`, which is a much sharper instrument.

The gain law is written in the linear domain (`g = 1 + kk*(thresh/e - 1)`) rather than as the
usual `pow(thresh/e, k)`. One divide per sample instead of a transcendental, and the make-up
gain is bounded by construction instead of running away as the threshold drops.

### H2. Ring modulator

Post-filter, in the FX block, carrier independent of pitch — the version Dengo picked.
Frequency 5 Hz–2 kHz on an exponential taper, amount from dry to pure ring. Hz is stored, not
the slider position, so the taper can be retuned later without repitching saved songs.

**The carrier restarts on every new note, at its peak.** Three reasons that happen to agree:

1. A free-running audio-rate carrier drifts against the bar, so a one-pattern loop stops
   repeating identically — which is this block's own admission criterion.
2. Restarting gives every note the same timbre instead of one that lands differently each
   time.
3. **cos**, not sin, so a freshly zeroed phase starts at the carrier's peak. Starting at a
   zero crossing would mute the attack transient at high amounts.

Note this is the **opposite** of the `pwmLfoPhase_` rule, and the difference is the rate: a
slow LFO's drift across the bar is the feature; an audio-rate carrier's drift is not heard as
movement at all.

### The one that had two defensible answers

**Whether to reset the compressor's follower at pattern start.** `resetFilters()` runs there,
and it is the obvious place. Resetting makes the loop bit-identical, which satisfies the
block's criterion literally — at the price of a burst of make-up gain on the first few
milliseconds of beat 1 of *every* bar.

It is not needed. A one-pole is **contracting**: the difference between two runs with
different initial states shrinks by the smoothing coefficient every sample, so it is gone
long before a loop is over. Measured at exactly **0.0** between loops 2 and 3. A phase
accumulator never forgets, which is why the ring carrier needs an explicit reset and this
does not. Both are now invariants in `CLAUDE.md`, side by side, because they look like the
same situation and are not.

### `.roq` v11 and a generalised versioning rule

v11 appends four floats. It is also the first *appended* block to use the content-derived
version rule B introduced for a non-append change, because the rule turned out to be worth
having generally: **a song that never opened the FX dialog still writes v9 or v10** and still
opens in an older build.

The trigger is "some field differs from its default", not "the effect would be audible". A
carrier frequency dialled in with the amount left at zero is still something the user set,
and dropping it to keep a version number down would be the wrong trade. `neededVersion` is
now the single place that decides, and the writer's field blocks mirror the reader's guards
exactly — a file stamped 9 carrying v11's lines would have them read as the pattern count.

### Verified

- **Each effect has its own off-path proof.** `compoff` moves the threshold with depth at 0;
  `ringoff` moves the carrier frequency with amount at 0. Each is **bit-identical** to the
  same patch on the pre-change engine. A single all-off render would not have shown that the
  *other* control of each pair is inert.
- **Plain `off` is still bit-identical to the pre-round-2 engine**, so the whole stack with
  everything switched off remains the 2004 sound.
- **The compressor does what it says.** Accents on steps 0/4/8/12: the accented-to-plain peak
  ratio falls from 3.21 to 2.17 (68 % of dry) — evened out, but the accents still stand out,
  which is the difference between glue and a limiter. RMS goes *up* 1.24×, so it is not
  simply quieter.
- **The ring modulator produces real sidebands.** At amount 1.0 and 300 Hz the 65 Hz
  fundamental drops to 1.6 % of its energy and a sideband appears at 365 Hz at 6×. That is
  ring modulation, not a tremolo.
- **Neither breaks the loop.** Loop 2 vs loop 3: ring 0.0, compressor 0.0. Negative control:
  the free-running PWM LFO differs by 8.2e−01 on the same measure, so the test is sensitive
  to what it claims to test.
- **Bounded, with headroom.** `compmax`, `ringmax` and everything-at-once into Fuzz at full
  gain: finite, audible, and **zero samples reaching the output clamp** — peaks 0.83, 0.78
  and 0.68.
- **Format.** All four fields round-trip; a carrier-only song still declares 11 and keeps its
  frequency; an untouched song still declares 9 and loads at defaults; a 21-step pattern and
  the FX block round-trip together.
- Everything from rounds 1–6 still passes, including `test_a alloc` — the new per-sample code
  allocates nothing. Release builds with no warnings; the app starts and closes cleanly.

**Recorded, not chosen:** the two ring-modulator homes that did not win are still worth
having on paper. Pre-filter, in the oscillator section, the sidebands would run through the
filter and envelope like every other source, so a cutoff sweep would shape the metallic
content — better behaved, more like part of the instrument. Ringing the main oscillator
against the duplet would reuse the duplet's detune as a pitch-tracking carrier interval and
its level as the balance, needing no new oscillator at all — the most musical of the three,
at the cost of the duplet no longer being available as a plain detuned layer.

---

## I. Transport reset raced the audio thread — RESOLVED 2026-09-04

`setRunning()` now writes two atomics and nothing else. The DSP work moved into
`applyTransportReset()`, which `process()` calls at the top of a block after an
`exchange()` on `transportReset_` — so the request is applied exactly once, on the thread
that owns the state, and a press arriving mid-block is kept for the next one rather than
lost. No lock and no allocation, so invariant 3 still holds. The reset also now lands on a
block boundary instead of mid-buffer, which is better behaviour than what it replaced rather
than merely safer.

### Two more races, one of them found by the test

- **`currentStepIndex()` returned `currentStep_` directly**, a non-atomic int the audio
  thread writes while the UI polls it for the step display. Making `currentStep_` atomic
  would have put an atomic load in the per-sample hot path to serve a UI that repaints at
  30 Hz. The audio thread publishes `displayStep_` once per **step** instead — one relaxed
  store every few thousand samples.
- **`setParam` wrote `boxSndType_`** from whichever thread moved the Waveform control. Found
  by the ownership check below, not by reading. It was also *dead*: `process()` derives the
  same value from the Waveform atomic at the top of every block, before anything reads it.
  Deleted.

### The acceptance test had to change

This item asked for a ThreadSanitizer run. **MSVC ships AddressSanitizer but not
ThreadSanitizer, and ASan does not detect data races**, so that test cannot be run as
written on this toolchain. Two things stand in for it, and the pair is arguably stronger:

1. **`tools/check_dsp_ownership.py`** — a static check that every non-atomic member is
   assigned only from `process`, `triggerStep`, `resetFilters`, `applyTransportReset` or
   construction. Rather than observing that no race happened during one run, it checks the
   property that makes a race impossible. This is what found `boxSndType_`.
2. **`test_a stress`** — now runs a third thread hammering the transport while the writer
   thread rewrites the song and the render thread renders. Measured 60 000 render blocks
   against 13 954 song commits and 14.4 million transport changes: no non-finite samples, no
   samples outside ±1.

**Verified:** all 11 golden modes remain bit-identical, so none of this changed the sound.

---

## J. Reverse step — RESOLVED 2026-09-04

**Reverse step now reverses the filter envelope in time.** It was a sign flip on EnvMod,
which inverts the envelope but does not reverse it: an upside-down decay moves fastest at
the note's *start*, where a reversed note should be slow first and peak at the end. Dengo
heard the difference; the numbers below confirm it.

**What changed** (`src/CBEngine.cpp`):

- `boxDecay_` starts at `4.0 * pow(decayMul, N)` for a reversed step and multiplies by
  `1.0 / decayMul` per sample, climbing back to 4.0 as the note ends. `4·r^(N−t) ==
  (4·r^N)·(1/r)^t`, so it is the forward curve read backwards — one different starting
  value and one different multiplier. **Decay length becomes attack length.**
- `decayMul` is hoisted out of the sample loop (it was computed inline per sample from the
  same block constant, so the value is unchanged), and `decayMulInv` beside it.
- `audibleNoteLength()` walks the slide chain forward to size `N` as the note's *audible*
  length — its own step plus every tied step, less the tail after the gate closes, using
  the **last** step of the tie for the accent-dependent threshold.
- The direction is latched inside `triggerStep()`'s `!bNoteSlide_` branch, so a tie is one
  envelope and a slid reversed note reverses across the whole note.
- A `> 4.0` clamp bounds the growth, and the seed is floored at `1e-30` so an underflowed
  `r^N` cannot strand the envelope at zero.
- **`stepReverse_` no longer appears in the cutoff expression at all** — that line is back
  to its pre-reverse-step form verbatim, which is the cleanest possible off-path.

**Verified:**

1. **Off-path bit-identical.** All 11 harness modes rendered against `golden_preA/`:
   **10 of 11 byte-for-byte identical**, and the only one that differs is `invert`, the one
   mode that sets `reverseStep`. As a free extra, that also proves the distortion module's
   Classic path is bit-identical, since `Distortion.cpp` is now linked into the harness.
2. **It is a reversal, not an inversion — the decisive test.** A dedicated `revshape` probe
   (every step identical but the one under test, high note so a 6 ms window holds many
   cycles) measured brightness as `rms(first difference) / rms(signal)` across the audible
   window, averaged over three loop repetitions, against a pre-J engine reconstructed by
   reverse-applying the patch:

   | | slice 1 → 8 | peak | rise done by half-way | mirror error |
   |---|---|---|---|---|
   | control (normal) | 0.386 → 0.163 | slice 1 | — | — |
   | old (sign flip) | 0.079 → 0.236 | slice 7 | **62 %** | 67 % |
   | new (time-reversed) | 0.163 → **0.383** | slice 8 | **35 %** | **11 %** |

   *Mirror error* is the strongest of these: a time reversal means slice `k` of the reversed
   step should equal slice `N−1−k` of the normal step. The new curve matches its own control
   read backwards to within **11 %** of the control's range; the sign flip scores **67 %**.
   Note also the endpoints — the reversed step ends at 0.383 where the normal one *starts*
   at 0.386, and begins at 0.163 where the normal one *ends* at 0.163.

   The old engine's 62 % front-loading matches the analytic prediction for
   `E·(2 − 4·r^t)` exactly, which is what says the measurement is faithful rather than lucky.
3. **Bounded.** 20 s renders with reverse on a slid step (`revtie` — step 3 slides into an
   accented step 4, exercising the tie walk and the accented gate threshold) and with all
   sixteen steps reversed (`revall`): zero non-finite samples, peak 0.789, and still
   audible. A backwards envelope multiplies *up* every sample, so runaway was the one real
   risk.
4. **Nothing else moved.** `test_a alloc` still reports **0 allocations** in `process()`;
   `test_a stress` passes 60 000 blocks against 10 632 concurrent commits; all 15 `.roq`
   checks pass, including v1/v2/v5/v6/v7 files loading with reverse off.

**Docs updated:** `README.md` gained "The envelope — and why there are four of them" (the
explanation Dengo asked for) and a rewritten reverse-step section; `CLAUDE.md` invariant 7
now describes the time reversal, with the sign flip recorded as the wrong-looking-right
alternative and the mirror test as the thing that tells them apart.

**One behaviour change worth a listen:** the direction latch moved inside the `!bNoteSlide_`
branch, so a tie now takes its direction from the step that started it. Previously every
tied step re-latched. This follows from the redesign — a reversed tie is one backwards
envelope — but it is a change to how long notes behave, so it is worth hearing before it is
taken as settled.

---

## K. Triplet feel — RESOLVED 2026-09-04

A **Triplet feel** checkbox beside Pattern length, per pattern, plus `Ctrl+T` and a Pattern
menu item. `.roq` **v12** carries it.

**What it does:** off, a step is a sixteenth. On, a step is an **eighth-note triplet** — a
quarter divided by three, 4/3 longer — so twelve steps fill exactly one 4/4 bar at the
*same tempo*.

| | 12 steps | 16 steps | 24 steps |
|---|---|---|---|
| straight | ¾ bar (the polyrhythm) | 1 bar | 1½ bars |
| triplet | **1 bar** | 1⅓ bars | **2 bars** |

**The number it deletes is 105.** Getting twelve steps to fill a bar at 140 BPM used to mean
typing 105 into the tempo box — a figure with nothing to do with the music, out of step with
every other device, recomputed by hand every time the real tempo moved. That is exactly what
Dengo meant by "you'd not have to work out the tempo for it", and it is the whole feature.

**Which triplet: the ×4/3 eighth**, chosen because it makes *12* — the number named first —
fill one bar, and twelve eighth-triplets over a 4/4 bar is the loping 12/8 shuffle that
"groovy 3/4 beat" describes. The ×2/3 sixteenth triplet is the more literal reading and would
make *24* the one-bar number instead, with 12 giving half a bar of faster rolling triplets.
**If the ear says otherwise it is one constant** — `tempo * 3.0` becomes `tempo * 6.0` in
`CBEngine::exactStepSamples()` (called `stepSamples()` until 2026-11-06), and `analyze_k.py`'s
expected periods halve. Recorded here so the change is
a two-line one and not an archaeology exercise.

Rejected: making the ratio depend on the pattern length so both 12 and 24 fill exactly one
bar. It would be magic — one checkbox meaning two things — and a 13-step pattern would have
no defensible answer.

### The trap, and it is a real one

`samplesPerStep_` is computed **once per block**, but the step duration is now a property of
the **pattern**, and the sequencer changes pattern *inside* the sample loop. Recomputing only
at the top of the block would run a triplet pattern following a straight one at the wrong
rate for up to a whole buffer: **a flam on beat 1, once per loop, that moves with the audio
buffer size** — the kind of bug that gets blamed on the sound card. `stepSamples()` is
therefore called in both places.

The test for it renders the same song at 32, 64, 512 and 1024 frames per block and requires
identical onsets. That mattered: the bug is invisible at any single block size, so a harness
with one hard-coded block would have passed a broken build. The harness gained a block-size
argument for this.

### `.roq` v12 costs nothing

The flag is a **second token on the pattern's `lastStep` line**, not a line of its own.
`readInt` parses with `std::stoi`, which reads the leading integer and stops — so a pre-v12
build reads `"11 1"` as `lastStep` 11 and never sees the flag. Same trick as the per-step
reverse column, and unlike v10 it loses no compatibility whatever. Still stamped v12 when
used, so the file is honest.

### Verified

- **Off is bit-identical.** `straight12` renders byte-for-byte the same on the pre-change
  engine. The straight branch is the original expression, not a multiply by 1.0 — house rule,
  and it makes the proof exact rather than a rounding coincidence.
- **The ratio is right, measured in samples against the bar.** 12 triplet steps = **75 600
  samples**, exactly what 16 straight steps take, exactly one bar at 140 BPM. 24 triplet
  steps = 151 200 = two bars, which is what pins it to the *eighth* triplet: a sixteenth
  triplet would have given 75 600 there. 12 straight steps = 56 700 = ¾ bar, the polyrhythm,
  measured for contrast.
- **The pattern boundary is clean.** A straight 16 followed by a triplet 12: every gap is
  75 600 samples, and the onsets are identical at all four block sizes.
- Finite, inside ±1, audible at every length. Everything from rounds 1–7 still passes,
  including `test_a alloc`. Release builds with no warnings.

### Two things the new keyboard check found on the way

Adding a menu item meant teaching `tools/check_keyboard.py` about **menu-internal
mnemonics**, which it had never covered — a popup is the same namespace problem one level
down. It immediately found three live collisions, all pre-existing, all silent:

- **Pattern menu:** `&Next` against `&New pattern` → `Ne&w pattern`.
- **File menu:** `Start &new track...` against `&New random pattern` → `Start new &track...`.
- **Edit menu:** `&Random pattern` against `T&ranspose current pattern` → `Tra&nspose`.

Each cycled focus between two items instead of activating either. The check now covers five
failure classes rather than four.

---

## Checked by use — CLOSED 2026-11-07

(Was "Needs a human — cannot be checked headlessly".) Every engine claim here was verified
numerically when it was built. The by-ear and screen-reader half is now done too, though not
as a checklist: by 2026-11-07 Dengo and the testers had made so many patterns that
everything below had been exercised in ordinary use, and nothing on it was reported wrong.

**What that settles for the open questions below:** each one is closed as *keep it as
built*. Three-key chords for the per-step toggles, the ×4/3 triplet, Screamer's 0.3× level,
the per-note ring restart and the 64-entry length combo were all used without complaint, and
"nobody asked for it to change" is the answer each question was waiting for. If any of them
does come up later, the reasoning for the alternative is still written down in its entry.

**One thing to keep watching:** the Cutoff sweep that once sounded flat within each pattern
was never reproduced. If it happens again, save the `.roq` straight away.

The entries are kept as they were written, for the reasoning in them.

- **Copy / Cut / Paste pattern — implemented 2026-09-05, reported dead by Dengo.** They had
  never been implemented: menu items, accelerators and an F1 entry all existed, and no `case`
  in the WM_COMMAND switch handled any of the three, so `DefWindowProc` swallowed them in
  silence. Now `Ctrl+C` / `Ctrl+X` / `Ctrl+V`, carrying steps *plus* length *plus* triplet
  flag, with Cut clearing in place rather than removing the slot (sweeps address patterns by
  index, so removing one would relocate every sweep after it). `tools/check_keyboard.py`
  gained a sixth check for this class — the other five all ask whether a keystroke arrives,
  and none could see that nothing was listening. **Worth confirming by hand** that paste into
  a pattern of a different length announces sensibly.
- **The per-step toggles never lost their keys, but they had become undiscoverable.** They
  moved to `Ctrl+Shift+A` / `S` / `I` in F and the checkbox labels said nothing about it, so
  a screen reader announced no key at all. The labels now name the chord, the way
  `Triplet feel (Ctrl+T)` already did. **Open question for Dengo:** whether three-key chords
  are right for the two most-used controls in the program. Two keys would mean taking `Alt+A`
  and `Alt+S` back from the Accent and Resonance sliders and binding them as accelerators,
  which re-opens the shadowing class that F closed — see the trade-off in `README.md`.

- **A Cutoff sweep once sounded flat within each pattern, jumping at every bar line**
  (2026-11-06, F5, one sweep spanning several patterns). It could not be reproduced: a
  headless probe tracing the swept value and per-step brightness, with and without accents,
  at a 480-frame block, climbs every step, and the bar line is no bigger a change than any
  note-to-note one. A later run sounded right. **If it happens again, save the `.roq`
  straight away**, so the exact song can be rendered.
- **The 2026-11-06 round (L–W), by ear and by screen reader.** Whether the sweep editor
  reads well with the Hold checkbox and Update button in it; whether `Cutoff (2 sweeps)` is
  announced usefully on focus or is noise; whether a slide over the bar line now sounds like
  one note; whether Fuzz still sounds like Fuzz without its offset (it should - only DC
  moved); and pulling headphones out mid-song to hear the device come back.
- **Listening — done 2026-09-04.** The resonance boost **works**, and the PWM LFO
  **works**; both are confirmed by ear, not just by numbers. **Reverse step was not what was
  wanted** — rebuilt as a time reversal, see J. The 303 envelope now has a written
  explanation in `README.md` ("The envelope — and why there are four of them").
- **Reverse step, again.** J is verified numerically but not by ear. Worth checking both
  that a reversed step now sounds reversed, and that reversed *slid* notes behave — the
  direction latch moved, so a tie now reverses as one note.
- **Listening, still outstanding:** the noise source, and the five distortion models from
  G — particularly whether Screamer's 0.3× level reads as character or as a fault.
- **Pattern length (B).** Whether odd lengths are musically useful in this instrument — 15
  against 16 walks the accents around the bar, a short pattern under a long sweep never
  repeats the same way twice. Also whether the pattern boundary is clean at short lengths:
  the filter carries state across it, which is audible as a ring rather than a click in the
  renders, but a 2-step pattern crosses that boundary ten times a second.
- **Triplet feel (K).** Built with the ×4/3 eighth triplet, so 12 steps fill one bar. The
  question is whether that is the division you actually reach for, or whether the ×2/3
  sixteenth (24 steps = one bar, faster and busier) is the more useful default. Now that it
  exists, comparing 12-triplet against 12-straight against a 4/4 reference should settle it
  in about a minute. Changing it is one constant.
- **The keyboard, after F.** Every chord in the table moved or was checked, so this is the
  round most likely to have broken muscle memory rather than code. Worth confirming that
  `Alt+A`, `Alt+S` and `Alt+R` now land on the Accent slider, the Resonance slider and the
  Run button; that `Ctrl+Shift+A` / `S` / `I` / `G` reach the focused step from anywhere;
  and that `EnvMo&d` and `W&ildness` announce sensibly on their new letters.
- **The acid FX block (H).** Three questions the numbers cannot answer. Whether the
  compressor at a middling depth actually reads as *glue* rather than as "everything got
  louder" — the auto make-up is doing real work and the RMS goes up 1.24×. Whether the ring
  modulator's per-note carrier restart sounds locked and deliberate or mechanical, since the
  alternative (free-running) was rejected on loop-identity grounds rather than by ear.
  And whether the 5 Hz end of the carrier range is useful as a per-note tremolo or just
  lands as a fault.
- **Screen reader.** That every control announces its name and value, and that the four
  mnemonics moved off their first letter (`Te&mpo`, `T&uning`, `Deca&y`, `Wil&dness`)
  announce sensibly rather than just uniquely. Since B, also whether a 64-entry length combo
  is pleasant to arrow through, or whether it wants an edit box or a coarser list — it is
  the first control here whose *number of choices* is the usability question.
- **Activation — confirmed.** Space, Enter and the mnemonic each toggle every checkbox.
- **Transport — confirmed.** F5/F6/F7/F8 do what their labels say, including F5 restarting
  mid-playback and F8 being harmless when already stopped.
- **Layout — working so far** (Dengo). The main window and both dialogs, including the
  oscillator dialog after two rounds of growth (252 → 314 dialog units). Worth re-checking
  once G's distortion dialog has settled, and again for B's length combo, which is the first
  thing placed to the right of the step editor block.

---

## Standing rules

In `CLAUDE.md`, repeated here because they are what breaks silently:

- `resetFilters()` must not touch `pwmLfoPhase_`.
- `wwelti_IIR_IIRlp3pole` stays byte-for-byte; new filter behaviour goes in a sibling.
- No allocation and no locking on the audio thread — A fixed this 2026-09-03; anything
  added to `process()` (G, H) has to keep it true. `test_a alloc` is the enforcement.
- A new feature's off-state must be bit-identical to the engine before it.
- Never bind a plain unmodified key as a global accelerator, and never reuse a mnemonic
  letter. Both fail identically and silently: "the key does nothing."
