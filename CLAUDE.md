# Working on Screambox

Formerly Creakbox; renamed 2026-11-06. **Credit the original wherever the name appears**:
the sound is Bass Assessor's CreakBox (2002) and its 2004 VST source, kept bit-identical on
purpose, and the rename exists to avoid confusion with that program, not to claim it. Two
things keep the old name and must go on keeping it: the `CreakboxRoq` / `CreakboxHape`
file magics (renaming them makes every saved song unreadable), and every reference to the
original - `CREAKBOX.EXE`, the `.cb` "CreakBox" header, `creakbox.txt`, `reverse/`.

Read `README.md` first for what the program is and how to build it, and `TODO.md` for what
is still outstanding. This file covers the things that are easy to get wrong and that
nothing will catch for you.

## Build

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

If configure fails complaining about a `CMakeSystem.cmake.in` under a CMake version that
is no longer installed, delete `build/CMakeCache.txt` and `build/CMakeFiles/` and
reconfigure. Leave the rest of `build/` alone — it holds `Release/`.

The release package lives in `dist/` (ignored by git): the exe, `README.md`,
`RELEASE-NOTES.txt` (`TESTING.txt` in a test build), `LICENSE.txt`, `license-bmd.txt` and
`creakbox.txt`. The `modules/` presets are not shipped.

The program version is `src/version.h` and nowhere else - `resource.rc` and the About box
both read it. It is unrelated to the `.roq` / `.hape` format versions; a release never bumps
those, and a format change never needs a release number to justify it.

## What is compiled and what is not

`CMakeLists.txt` is the authority. `src/` contains both the live standalone and the 2004
VST sources kept for reference; **`SilverBox*.cpp`, `SilverBoxEditor*`, `SilverBoxGUI*`,
`knobs.*`, `CPitchSlider.*` and `main.cpp` are not built**. Read them freely; do not edit
them to change behaviour.

Most of the 2004 files are marked read-only on disk deliberately. If a write fails with
`Permission denied` on one of them, that is the speed bump working — stop and check
whether you actually meant to edit a reference source. Clear the flag only on the
specific file you genuinely need, never in bulk.

## The accessibility rule

Every control is a standard system control preceded by a STATIC label. That is the entire
accessibility strategy, and it is why nothing here implements `IAccessible` or defines a
custom accessible object. The only accessibility API used is `NotifyWinEvent`.

- **Never disable a control to mean "this does nothing right now."** It announces as
  *unavailable* with no discoverable cause. Put it in the label text instead — the way
  the Acid FX gain, tone and level labels say "no effect" while the type is Classic, and
  the sweep editor's first LFO wave reads "Off: a straight sweep (frequency and phase
  unused)" rather than greying the fields beside it.
- **Set control text through `announceValue()`, not `SetWindowText`.** It fires
  `EVENT_OBJECT_NAMECHANGE` and `EVENT_OBJECT_VALUECHANGE`; without those a screen reader
  keeps reading stale state.
- **A selection you change in code needs its `WM_COMMAND` sent by hand**, then a notify.
  The note combobox's octave jump is the worked example.
- **Run `tools/check_keyboard.py` after any change to `resource.rc` or a control label.**
  It catches all seven ways a key can silently do nothing here, each of which has shipped
  at least once: a chord bound twice, an unmodified key bound as an accelerator, two controls
  sharing a mnemonic, a mnemonic shadowed by an accelerator or by the menu bar, two items in
  one menu popup sharing a mnemonic, two controls in one **dialog** sharing a mnemonic (the
  sweep editor had two, on Alt+N and Alt+T, while the checker looked only at the main
  window), and - the one the others cannot see - **a menu item or accelerator whose command
  id no `case` handles**, which `DefWindowProc` swallows in silence while the menu item
  stays enabled and F1 goes on documenting it. Copy, Cut and Paste pattern were all three
  dead that way from the beginning. The other checks ask whether the keystroke *arrives*;
  that one asks whether anything is listening when it does.
  **Precedence is menu bar, then accelerators, then mnemonics**, and every loser fails
  silently. `F`, `E`, `P`, `H` and `L` belong to the menus. No plain `Alt+letter` is bound
  as an accelerator any more, and keeping it that way is what makes the shadowing class
  unrepeatable - everything that acts on the focused step lives on `Ctrl+Shift+<letter>`.
- **A mnemonic that collides activates nothing.** Win32 cycles focus between the matches
  instead, which presents as "the key does nothing" rather than as an error. `Alt+V` did
  this between the Volume slider and the reverse-step checkbox for a while. **The main window
  is nearly out of free letters** — check uniqueness before adding one, and prefer moving
  a mnemonic to a later letter of the same word (`Te&mpo`, `Deca&y`, `T&uning`,
  `Wil&dness` all exist for this reason) over inventing a label. Reserved by the menu
  bar: F, E, P, H. An accelerator on the same chord pre-empts the mnemonic entirely,
  which is why `Alt+A` / `Alt+S` / `Alt+R` fire their accelerators and the identically
  lettered labels never see the key.
- **Never bind a plain, unmodified key as a global accelerator.** `TranslateAccelerator`
  runs before `IsDialogMessage`, so the accelerator wins everywhere and the key goes dead
  on every control in the window. `VK_SPACE` was bound to run/stop for a while and no
  checkbox or button in the window could be activated with Space as a result. Transport
  now lives on F5–F8, which no control wants.
- **Enter is not a checkbox key in Win32** — Space is. Enter goes to the default button,
  and this window has none. The checkboxes are subclassed
  (`makeCheckboxEnterActivatable`) so Enter toggles them too, matching Enter-on-a-slider
  opening the sweep editor. New checkboxes should get the same call.
- **A control with no mnemonic must name its chord in the label** - `Triplet feel (Ctrl+T)`,
  `Slide this step (Ctrl+Shift+S)`. The main window ran out of letters some time ago, so this
  is now the normal case rather than the exception, and an accelerator that appears nowhere in
  any label is one only F1 can find.
- **Prefer an accelerator to a mnemonic for anything used while another control has focus.**
  A mnemonic moves focus to the control it names; the per-step toggles are worked while
  arrowing the step list, so a mnemonic would throw you out of the list on every toggle.
- New controls need a label, a unique mnemonic or a chord named in the label, and a place in
  tab order that makes sense when you cannot see the window.

## Engine invariants

Breaking any of these is silent. Nothing fails to compile, nothing throws, and the bug
shows up later as "it sounds different now" with no obvious cause.

1. **`resetFilters()` must not touch the LFOs' state (`osc_`).** It zeroes `boxOsc_`,
   `dupletOsc_` and `subOscPhase_`, and runs at every pattern start. An LFO's phase belongs
   to the song position; adding it there — the natural-looking place — turns every LFO into
   a per-pattern retrigger and nothing will tell you. The phase is taken afresh only where
   playback does not simply move on to the next step (`lastSweepPos_` in
   `evaluateSweeps`: Play, a loop, the wrap to pattern 1), and accumulated in between.
   This used to read "`pwmLfoPhase_`": the PWM LFO was free-running until v15 folded it
   into oscillating sweeps (TODO Y3).
2. **`wwelti_IIR_IIRlp3pole` in `src/IIR.cpp` stays byte-for-byte as it is.** New filter
   behaviour goes in a sibling function (`_boost` is the existing example) and the caller
   picks. The stock path being unedited *is* the proof the stock sound is unchanged.
3. **No allocation and no locking on the audio thread, and DSP state is written on the
   audio thread only.** `process()` takes a const
   reference into a double-buffered `SongData` and touches no mutex and no allocator.
   Anything you add there must keep that true: no `std::vector`, no `std::string`, no
   `std::function`, no lock. The rule is enforceable — `test_a alloc` counts allocations
   inside `process()` with an `operator new` hook and asserts zero.
   **Writers use `SongWriter`**, which waits for the audio thread to leave the idle
   buffer, copies live into it, mutates, then flips `liveIdx_`. Never mutate the live
   buffer, and never take `writerMu_` from the audio thread.
   **The DSP fields are a separate question from the song data**, and the rule for them is
   simpler: only `process`, `triggerStep`, `resetFilters` and `applyTransportReset` may
   write them. A UI-thread caller that needs to change DSP state publishes an atomic request
   and lets the audio thread do the work - that is what `transportReset_` is, and it is why
   `setRunning()` no longer calls `resetFilters()` itself. MSVC has no ThreadSanitizer, so
   `tools/check_dsp_ownership.py` checks the property statically instead: it lists every
   non-atomic member and every function that assigns to it, and fails if anything else does.
   Run it after touching `CBEngine`. It found `boxSndType_` being set from `setParam` on its
   first run - a write that was both racy and dead, since `process` derives the same value
   from the Waveform atomic every block.
4. **A new feature's off-state must be bit-identical to the engine before it.** Not
   "close" — identical. Where a hoisted per-block computation has to become per-sample,
   keep the hoisted version as the off-path fast path.
5. **Skip work entirely when a feature is off**, don't just multiply by zero. The noise
   RNG must not advance at level 0, or invariant 4 stops holding for it.
6. Sweeps are CHOSEN once per sequencer step and MOVE every sample. `evaluateSweeps` runs at
   each step boundary: it picks the sweeps in force, sets each one's value for the step's
   first sample - exactly the value a sweep has always had there - and records what
   `glideSweeps()` needs to move it on. Within the step, `glideSweeps()` writes every moving
   value for the sample (the step count made fractional) and `Knobs` / `Voice` are derived
   again, every sample, for as long as `sweepsMoving_` says something moves (Dengo,
   2026-11-09: a sine on Cutoff was a staircase, and so was a ramp). **The off path is
   `sweepsMoving_ == false`**: no sweep moving, nothing per sample, and the render is
   bit-identical to the engine before the glide. It is never called on a step's first
   sample, which is what keeps step-start values exact. RELATIONS in `check_sound.py` hold
   a constant gliding sweep (main slider and geometric) equal to the slider at that value.
   - **Main PWM and Duplet PWM are different** (`CBEngine::sweepTargetPerSample()`): their
     LFO wave is computed in the duty-threshold code, in double, from the centre the Voice
     carries - the PWM LFO's own expressions. Writing it through `sweptVal_` instead would
     round it to float and the converted LFO would no longer be bit-identical.
   - **Straight and oscillating sweeps are chosen separately** in `evaluateSweeps`: the
     straight one (or the slider) is the centre, the LFO is added to it and clamped to the
     slider's range - unless it is a *between* LFO (`SweepOsc::between`), which swings
     between its two values and ignores the centre. All of it goes through `oscValueAt()`,
     except the PWM targets' threshold code. On a geometric target (Ring frequency) an
     around-LFO's depth is in cents and a between-LFO swings in equal ratios. The PWM LFO's expressions survive verbatim in the per-sample path (increment
     `rate / sampleRate_`, accumulate, wrap at 1, the triangle, the clamp, the threshold) -
     that is what makes an old song's LFO, converted on load, bit-identical in a fresh
     engine. Change them and `tests/songs/lfo_*` tell you.
   - **Nothing that changes at a step boundary may wait for the next block.** This is the
     one rule behind two bugs that each "moved with the audio buffer size". The knob values
     (`Knobs`, `deriveKnobs()`) used to be derived only at the top of `process()`, so a
     sweep's new value - written at a step boundary in the middle of a block - waited for
     the next block and every note started with the previous step's value. And the step
     length: `samplesPerStep_` used to be per block while the step duration is a per-PATTERN
     property, so a triplet pattern after a straight one ran at the wrong rate for up to a
     buffer - a flam on beat 1. Both are now recomputed at the step boundary as well as at
     the top of the block. **Sweeps are evaluated before `triggerStep()`**, so the reversed
     envelope's seed sees the swept Decay. The test is the same for both: render at 32, 64,
     512 and 1024 frames and require identical output; the bugs are invisible at any single
     block size.
   - **Sweep values never go into `params_`.** They live in `sweptVal_` / `sweptOn_`,
     written only by the audio thread, cleared on stop and start. `params_` is the user's
     setting and nothing else - when sweeps wrote into it, Save and the WAV export picked
     up whatever a sweep was doing as if it were the slider, and Stop left it there.
     **Anything that reads a sweepable parameter on the audio thread reads it through
     `effParam()`**, never `params_` directly, or a sweep on it does nothing.
   - **Sweep targets are numbered, and the numbers are file format.** 0-6 are the main
     sliders (their own decode in `evaluateSweeps`, untouched since before v14); 7-23 are
     squelch and the dialog sliders (`kSweepDests` in `CBEngine.cpp`, `kSweepUi` in
     `MainWindow.cpp`, the list in `CBEngine.h`). Append only. A new sweepable parameter
     gets the next number, a row in both tables, and - if the audio thread hoists it per
     block - a place in `Voice` (`voiceNow()`), which is recomputed at every step boundary
     for the rule above. Stored values are real units: percent, cents, and hundredths of a
     Hz (ring). The frequency target interpolates geometrically. The proof for a new
     target is that a constant sweep renders byte-identical to the slider at that value.
     **14-16 are retired** (the PWM LFO's rate and depths, gone with it in v15 before any
     release carried them): the numbers stay taken, `loadRoq()` drops sweeps on them, and
     nothing lists them (`CBEngine::sweepTargetRetired`).
   - **A sweep position is an absolute step number**, `patStart[p] + step`, with
     `patStart` a prefix sum computed in `SongWriter::commit()` (UI thread; the audio thread
     only reads it). A sweep therefore advances the same amount per step across patterns of
     any length. An endpoint is clamped into the pattern it names, so shortening a pattern
     under a sweep does not fling it downstream. Anything else that maps (pattern, step) onto
     a timeline needs the same care — `sweepsOverlap` in `MainWindow.cpp` uses a stride of
     64, the maximum pattern length, which is enough for ordering though not for distance.
   - **The step length is fractional.** `exactStepSamples()` is rarely whole; each step is
     `floor(exact + stepFrac_)` and passes the remainder on, so the tempo cannot drift.
     Where the exact length is whole (44.1 kHz at 140 BPM) `stepFrac_` stays 0 and every
     step is what it always was - that is the off-path proof. `songLengthSamples()` repeats
     the same arithmetic for the WAV export; change one and you must change the other (the
     harness checks they agree).
   - **Playback follows a pattern by identity, not index.** `SongData::patIds` gives each
     pattern a stable id; when a commit inserts or deletes patterns in front of the playing
     one, `process()` finds it again by id. Writers that replace the whole pattern list call
     `freshIds()`; `reindex()` regenerates ids if a writer forgot, rather than letting the
     vectors disagree.
7. **The acid FX block's state rules are not all the same, and the differences matter.**
   Distortion, compressor and ring modulator all sit after the volume envelope, in that
   order. The compressor is deliberately *before* the ring: its follower would otherwise
   chase the carrier's amplitude modulation and partly cancel the ring, so two amount
   controls would fight in a way neither label explains.
   - **The ring carrier's phase is re-zeroed on every new note**, in `triggerStep`. This is
     the opposite of the LFO phase rule above, and the reason is the rate: a slow LFO's
     drift across the bar is the feature, while an audio-rate carrier's drift is not heard
     as movement at all - only as an attack that lands differently every time, and as a
     one-pattern loop that quietly stops repeating identically. It uses **cos**, not sin, so
     a freshly zeroed phase starts at the carrier's peak and the note's attack survives.
   - **The compressor's follower must NOT be reset in `resetFilters()`.** That function runs
     at every pattern start, so zeroing it there puts a burst of make-up gain on beat 1 of
     every bar - an artifact manufactured once per loop to satisfy a criterion the follower
     already meets. A one-pole is *contracting*: two runs with different initial states
     converge within a fraction of a loop. A phase accumulator never forgets, which is why
     the two need opposite treatment. The harness measures both.
   - **The compressor's follower has no attack time, and that is load-bearing.** A smoothed
     attack lets a sample sit above the envelope that graded it, and the auto make-up then
     pushes it past full scale - 0.19 % of samples clipped at an ordinary setting when this
     was first written. With a peak follower (`env = max(|x|, released)`) the output bound
     is provable: given `env >= |x|` and a curve that pivots at 1.0, an input inside +-1
     cannot leave the stage outside +-1 for any threshold or depth. The test asserts zero
     clipped samples, not merely `|x| <= 1`.

8. **Reverse step must stay a *time reversal* of `boxDecay_`, not a sign flip on EnvMod.**
   The reversed step starts `boxDecay_` at `4.0 * pow(decayMul, N)` and multiplies by
   `1.0 / decayMul` per sample, so it climbs back to 4.0 as the note ends. That works
   because `4·r^(N−t) == (4·r^N)·(1/r)^t` — it is literally the forward curve read
   backwards. The obvious-looking alternative, negating EnvMod, still compiles, still
   sounds "not normal", and is **wrong**: an inverted decay moves fastest at the note's
   start, where a reversed one must be slow first and peak at the end. That was shipped
   for a while and did not sound reversed. The test that tells them apart is in the
   scratchpad harness (`analyze_j.py`): the reversed step's brightness curve must be the
   normal step's curve *read backwards*, within ~11 % of the control's range. An inversion
   scores ~67 % on the same measure.
   - **`N` is the note's audible length, not the step length** — its own step plus every
     step tied by a slide, less the tail after the gate closes (`audibleNoteLength`).
     Sizing it to the step instead puts the peak after the gate has shut, where it is
     silent.
   - **The direction is latched only on a genuine new note** (inside the `!bNoteSlide_`
     branch of `triggerStep`), unlike `boxAccent_` beside it. A tie is one envelope; a
     per-step latch would reverse direction mid-note with no fresh envelope to reverse.
   - **Keep the `> 4.0` clamp.** A backwards envelope multiplies *up* every sample, so if
     the tie walk and the real note length ever disagree it is an exponential runaway. The
     clamp turns that into a hold.
   - **It must stay confined to `tempcutoff`.** The amplitude path has never seen EnvMod,
     and a reversed step is meant to change the filter's direction, not its loudness.
     It also never reads or writes EnvMod at all, which is what makes it compose cleanly
     with sweeps aimed at that parameter.

9. **The per-pattern filter reset is skipped while a slide carries a note across the bar
   line** (`if (currentStep_ == 0 && !bNoteSlide_) resetFilters();`). The reset itself is
   2004's ("reset filter every 16 steps") and stays; but zeroing the gate, the filter
   memories and the oscillator under a note that is still sounding is a click, and it threw
   the glide away. Everything that looks ahead uses `nextPlayedStep()` - next pattern's step
   0 in FullSequence, the same pattern's in loop mode - so the slide decision, the glide
   target and `audibleNoteLength()`'s tie walk can no longer disagree at the bar line, which
   they did for as long as the reset hid it. Invariant 1 is untouched: the LFOs' `osc_`
   is reset by neither path.

## How engine changes get verified

**First, always: `python tools/check_sound.py`.** It renders 85 built-in cases, the songs in `tests/songs/`, plus every
module under `modules/` when that folder is present, and compares them with
`tests/golden/sound.manifest`. For each render it checks:
- that renders at 32, 64, 512 and 1024 frames per block are identical;
- that the song's `.roq` read back renders the same;
- that `.roq` and `.hape` re-save to the same bytes;
- that `process()` makes zero allocations.

On a mismatch it names the first differing sample as a pattern and step. Build its driver
with `cmake --build build --config Release --target screambox_render`.
- **A change that should not move the sound must leave it all PASS.**
- **A change that should move the sound** moves exactly the lines it should and no others.
  Then run `--update` in the same commit, and say in the commit message which lines moved
  and why. Never run `--update` to make a failure go away.
- **Add a case to `tests/render.c` for every new feature**, with its own off-path proof (see
  below).

The driver is C and talks only to `tests/sb_api.h`, so a C port links the same driver and
the same manifest unchanged; that is its purpose (TODO Y4). `sb_engine_load_song()` in
`tests/sb_api_cpp.cpp` mirrors what `MainWindow::loadRoqFile()` does to the engine, so keep
the two in step. `tests/songs/` holds songs written by OLDER builds, rendered from their
files like the modules: what a newer build makes of them on load (the v15 PWM LFO fold) is
tested on bytes those builds really wrote. Add one when a load path starts converting
something.

For a new feature, the manifest proves the off path but not the feature itself. The fuller
method below still applies to the feature:

The standard here is a bit-exact comparison, and it is worth the setup:

1. Reconstruct the previous engine by writing a script that *reverse-applies* your patch
   to copies of the engine sources in a scratch directory. Assert afterwards that none of
   your new identifiers survive in the reconstruction — that assertion is what makes the
   baseline trustworthy.
2. Build a small headless render harness against both engines, with the new parameters
   behind an `#ifdef` so the same source builds both.
3. Render and compare bytes.

Two things that matter for the harness:

- **The baseline must be a matched control.** Any setting a test mode changes that is
  *not* one of the new parameters has to be set outside the `#ifdef`, so the old binary
  renders the same patch. Otherwise you are comparing your feature against defaults.
- **Give each feature its own off-path proof.** One combined "everything off" render lets
  one feature's regression hide behind another's guard.

Test-design traps already paid for once:

- **Zero-crossing rate is useless as a brightness measure** — it is amplitude-blind, so a
  nearly-shut filter's residual ripple still crosses zero constantly and reads as bright.
  Use an energy-normalised measure such as `rms(first difference) / rms(signal)`.
- **Envelope tests need a fast decay.** At the default Decay 0.70, `boxDecay_` only
  travels 4.0 → ~1.97 within a step, so `(boxDecay_ - 2)` never leaves the neighbourhood
  of zero and normal vs. inverted curves converge instead of crossing. Lower knob = faster
  decay.
- **Pick a step with no slide and no accent** for per-step tests. `triggerStep()` skips
  the envelope reset on a slide, so a slid step has no fresh envelope to act on.
- **Total RMS hides small additions.** To measure what a new source contributes, difference
  against the baseline render rather than measuring the level of the whole signal.

## File formats

`.roq` and `.hape` are versioned, plain ASCII, and — with one deliberate exception —
append-only. Preserve that:

**Be precise about which direction is tolerant.** Old files load into new builds — that is
the property to protect. New files do *not* load into old builds: the reader rejects a
version above its own with `unsupported .roq version`, because the header is positional and
a partial read would shift every field after the missing one. Failing loudly is correct;
do not "fix" it into a best-effort parse. Only the **step line** is tolerant both ways, and
only because it grows by appending tokens.

- **Append, never reorder or reshape.** Guard new fields with `if (version >= N)`; give
  every new field a default that means "off" so older files load sounding identical.
- **The version stamped is the lowest one that can represent the song, not the newest.**
  `neededVersion` in `RoqIO.cpp` decides. A song that uses nothing newer than v9 writes
  exactly the bytes v9 wrote and still opens in an older build. Five rules feed it so far:
  a pattern that is not 16 steps forces v10, any acid-FX field that differs from its default
  forces v11, any pattern in triplet mode forces v12, any sweep that holds its end value
  forces v13, any sweep on a target above 6 forces v14, and any oscillating sweep forces
  v15. v14 changed no bytes of the layout; it exists because an older build *silently
  drops* such a sweep, and a song that quietly sounds different is what the header is for.
  v15 is the same argument: an older build would play an LFO as a straight sweep between
  its two depths. **Extend that function rather than
  bumping a constant**, and make the writer's field blocks `if (version >= N)` mirror the
  reader's exactly - a file stamped 9 that carries v11's lines would have those read as the
  pattern count.
  The trigger for an optional block is "differs from its default", not "would be audible":
  a setting the user made is not something to drop to keep a version number down.
- **The one non-append change is v10's step-row count, and note how it was paid for.**
  Variable-length patterns made a pattern block hold `lastStep + 1` rows instead of 16.
  A row count is positional: an old reader handed a long pattern runs off the end of it and
  every field afterwards is garbage. So the writer stamps the *lowest* version that can
  represent the song (`neededVersion` in `RoqIO.cpp`) rather than always stamping the
  newest — an all-16-step song still writes v9 and still opens in an older build, and only
  a song that actually uses another length declares v10 and gives that up. **If you ever
  add another non-append change, do the same thing: make the header say what the file
  *needs*, so compatibility is lost only by the files that really lost it.** And check the
  shipped modules still write as the old version; the test for that walks all 25.
- **`.hape` has its own pinned version** (`kHapeVersion`) even though it shares the
  counter's history. Its schema did not change until v13; stamping a sweep export with a
  number no older build accepts, to describe a change that did not happen to it, is a
  compatibility break for nothing. When v13 did change it (the hold token), it got the same
  content rule as `.roq`: 9 unless some sweep holds, 13 only then (`kHapeHoldVersion`).
  v14's new sweep targets deliberately do **not** raise it: the sweep editor's Load keeps
  only the current slider's sweeps and says how many it skipped, so an older build reports
  a squelch sweep as skipped instead of misplaying it - there is nothing silent to prevent.
  An LFO does raise it, to 15 (`kHapeOscVersion`): it is on the slider being edited, so an
  older build would keep it and misplay it.
- **A load path may convert, but only what it must.** v15's `foldPwmLfo()` turns a running
  PWM LFO into oscillating sweeps and zeroes its three header values; a song whose LFO
  rate is 0 is left exactly as it was, inert depths and all, so it re-saves to its own
  bytes. One of the testers' modules is such a song, which is how this was found.
- **Extend a line with an extra token** rather than changing its shape — an older
  three-token parser then still reads the first three correctly. This is how the per-step
  reverse-step column was added.
- **Store real units, not UI positions.** An LFO's rate is stored in Hz or in steps per
  cycle, as typed, and its phase in cycles, so a UI can change how it shows them without
  moving saved songs. Floats on the sweep line are written as the shortest text that reads
  back as the same float (`floatToken`), by trying precisions with `snprintf` - which a C
  port can reproduce byte for byte.
- `.cb` is import-only in practice. `saveCb()` exists but nothing in the UI reaches it, so
  new per-step data does not need to round-trip through the 2002 format. It is also fixed
  at 16 steps per pattern and cannot be otherwise — no length field, and the reader counts
  lines.

## Conventions

- Match the surrounding comment density. This codebase explains *why*, especially where
  something looks wrong but is deliberate — keep doing that, and keep the warnings next
  to the code they protect.
- Prefer a second function over a flag inside a hot, verified function.
- Reference-only sources and `reverse/` are documentation; `reverse/signal_flow.md`,
  `parameter_map.md` and `cb_schema.md` are worth reading before engine or format work.
