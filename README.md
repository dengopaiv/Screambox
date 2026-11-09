# Screambox

**Screambox is built on CreakBox.** A standalone Windows bassline synth and step sequencer,
built to be **fully usable from the keyboard and by a screen reader**. It started from a
TB-303-style voice and still has one at its core, but it has grown well past imitating
the 303 — variable-length and triplet patterns, reverse step, sweeps and LFOs on every
slider, noise, and an acid FX block are all things the original box never had.

The DSP is the original 2002–2004 Frank Reitz ("Bass Assessor") CreakBox / SilverBox
engine, lifted verbatim. What was rewritten is everything around it: the VST plumbing
and the bitmap-knob GUI are gone, replaced by a plain Win32 application built out of
standard system controls.

**About the name.** Until November 2026 this program was called Creakbox, after the
original. It was renamed so that nobody mistakes it for Bass Assessor's 2002 CreakBox or
assumes he made or endorsed it - not to take the credit for the sound. The filter, the
oscillators, the envelope and the accent come from CreakBox and its 2004 VST source
(bioroid media development, with wwelti's DSP optimisations), kept bit-identical on
purpose; the About box says so, and so does this file. The file formats keep their
`CreakboxRoq` / `CreakboxHape` headers, so songs and sweep files saved under the old name
open unchanged.

**Version 2.0.0, 7 November 2026** - the first release as Screambox, after a tested build
of 6 November. 1.0.0 was released as Creakbox on 3 October 2026; the numbering carries on
across the rename. The number lives in `src/version.h`, which feeds both the exe's file
properties and the About box.

---

## Lineage

| | |
|---|---|
| `CREAKBOX.EXE` | The original 2002 Delphi freeware release. Kept in the tree as reference and as the source of truth for `.cb` behaviour. |
| `src/SilverBox*.cpp`, `knobs.*`, `CPitchSlider.*`, `main.cpp` | The 2004 C++ VST source. **Reference only — not compiled.** |
| `src/CBEngine.*`, `src/MainWindow.*`, … | The current standalone. This is what `CMakeLists.txt` builds. |
| `src/IIR.*`, `src/dist.*`, `src/freq.*`, `src/core.h` | Original DSP, reused as-is by the standalone. |

The sound is deliberately *not* a reinterpretation. The filter, oscillators, envelope
and accent behaviour are the 2004 code paths; changes to the engine are held to a
standard of keeping every new feature's "off" state bit-identical to the engine that
preceded it (see [Engine invariants](#engine-invariants)).

Most of the original 2004 files carry the read-only attribute on disk. That is
intentional — it is a speed bump against editing reference sources by accident.

---

## Building

Requirements: Windows, MSVC (Visual Studio 2022 or newer), CMake 3.20+. No external
dependencies — everything links against the Win32 SDK.

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The result is `build/Release/Screambox.exe`. The C/C++ runtime is linked statically, so
the exe runs on its own, with no Visual C++ redistributable to install.

C++17, `/permissive-`, `/utf-8`, Unicode build (`UNICODE`/`_UNICODE`), `WIN32` subsystem.

> **If configure fails** with a message about a missing `CMakeSystem.cmake.in` under a
> CMake version you no longer have installed, the cache is stale. Delete
> `build/CMakeCache.txt` and `build/CMakeFiles/` and reconfigure. Do not delete the rest
> of `build/` — it also holds `Release/`.

---

## Running

- **Audio out:** WASAPI shared mode on the default Windows endpoint, on its own render
  thread (`src/AudioEngine.cpp`). The device's own sample rate is used; nothing is
  resampled. **Losing the device is survivable:** unplug the headphones and the render
  thread reopens whatever the default output now is, retrying until one exists, and the
  status line announces `Audio device lost` and then `Audio device changed, now <name> at
  <rate> Hz`. It used to stop rendering for good and keep saying "Playing". It also follows
  the Windows default: pick another output in Windows and playback moves there, with the
  same announcement.
- **No MIDI.** There used to be a MIDI input that stored incoming notes and never played
  them; it was removed rather than left advertising something it did not do.
- **Modules:** `modules/` holds 56 `.cb` presets from 2006–2010 plus a handful of `.roq`
  songs. They are the developer's local library and are not shipped with the program.

---

## The design rule that governs everything

> Every control is a **standard system control** preceded by a **STATIC label**.

Nothing here implements `IAccessible` or defines a custom accessible object, and nothing
should. Screen readers announce name and value correctly because the controls are real
trackbars, comboboxes, checkboxes and list boxes with real preceding labels. The only
accessibility API in the codebase is `NotifyWinEvent` — used to fire name-change and
value-change events when a control's text is updated programmatically, which is the one
case the system cannot infer on its own.

Three consequences worth internalising before touching the UI:

1. **Don't grey out a control to signal "inert".** A disabled control announces as
   *unavailable* with no discoverable reason. Say it in the label instead. The Acid FX
   dialog does this: while the distortion type is Classic, the gain, tone and level labels
   retitle themselves to say they currently have no effect — the controls stay reachable
   and stay explicable.
2. **Programmatic text changes must go through `announceValue()`**, which sets the text
   and then fires `EVENT_OBJECT_NAMECHANGE` / `EVENT_OBJECT_VALUECHANGE`. Calling
   `SetWindowText` directly leaves a screen reader reading stale state.
3. **A synthesised selection change needs its `WM_COMMAND` sent too.** The note combobox
   shows the pattern: its Page Up/Down octave jump sets the selection, posts
   `CBN_SELCHANGE` to the parent itself, and then notifies.

---

## Control surface

### Main window

| Control | Notes |
|---|---|
| Waveform | Saw / Square |
| Tempo | BPM |
| Transport | A Run/Stop toggle button; the keyboard has four separate actions on F5–F8 (song / pattern / from-current / stop) |
| Pattern selector | The editing index is **decoupled** from the playback index, so edits stay put while the sequence runs |
| Volume, Cutoff, Resonance, EnvMod, Decay, Accent, Tuning | Seven trackbars. Arrows ±1, PgUp/PgDn ±10, Home/End to the rails. **Resonance, EnvMod and Tuning are bipolar (−100…+100)** — negative values give Frank's "inverted" filter character. |
| Resonance boost | Checkbox under the Resonance slider. Devilfish-style; see below. |
| Random wildness | Density for the random-pattern generators (0 = sparse, ~0.5 = 2002 defaults, 1 = chaos) |
| Accent squelch | Residual-stacking amount, 0–200 %. `100 %` = Frank's original TB-303 accent buildup, `0` = no stacking, above 100 = progressively squelchier consecutive accents. The slider position is the engine value × 100. |
| Pattern length | 1–64 steps, per pattern. Sits just before the step list in tab order. See below. |
| Triplet feel | Per pattern, beside the length. `Ctrl+T`. Makes a step an eighth-note triplet, so 12 steps fill a bar. |
| Step editor | One step selector + pitch combo (Rest, C0–B4) + slide / accent / reverse-step checkboxes |
| Oscillator… | Opens the oscillator dialog |
| Acid FX… | Opens the FX dialog — distortion, compressor, ring modulator. `Alt+X` or `Ctrl+D`. |
| Modulation… | Every sweepable slider in one list with its sweeps and LFOs, and LFO presets. `Ctrl+M`. See *LFOs*. |

**Press Enter on any of the seven parameter sliders, or on Accent squelch, to open its sweep
editor.** The sliders in the Oscillator and Acid FX dialogs work the same way.

### Oscillator dialog

Everything here is beyond the stock 303, and everything defaults to transparent.

- **Main PWM** — square duty cycle, 50 % = stock.
- **Sub sine level** — sine one octave down, mixed in. Default 0.
- **Noise level** — white noise into the oscillator sum. Default 0. It sits *before* the
  filter and the gate, so it is shaped by the filter sweep and rests stay silent. The
  trade-off is that at the low cutoffs where a 303 usually lives the filter eats most of
  it — correct for an oscillator-section noise source, but it means the control feels
  weak down there. A raw hiss layer on top would be a different (post-filter) feature.
- **Duplet** — a second oscillator with its own waveform, its own PWM, ±1200 cents detune
  and its own level. Default level 0.
- **The PWM LFO that used to sit here** is an LFO on Main PWM and on the duplet's PWM now
  (`.roq` v15): press **Enter** on either slider and choose an **LFO wave**, or use the
  Modulation dialog's *The old PWM LFO* preset. A song that used it opens with it converted
  and sounds the same. See *LFOs* below.
- **"Everybody needs a 303"** — resets **every sound parameter** to stock voicing, the
  main-window sliders included, not just this dialog. It leaves patterns, sweeps and
  transport state alone, and it deliberately does **not** touch per-step reverse flags:
  those are pattern data, not sound parameters, and a reset must not silently edit
  patterns.

### Acid FX dialog

Three effects in series after the filter and the volume envelope — where a pedal sits
relative to a real 303's output. Chain order is **distortion → compressor → ring modulator**.

Everything in here obeys one admission rule: **an effect must not break the loop.** No delay
lines, no reverb tails, nothing time-based that would stop a one-pattern loop from repeating
identically. Anything that needs to know where the bar is belongs in sweeps instead.

#### Distortion

A pedal in front of the amp: it sits where the stock 2004 clipper always sat. Five models,
one type combo and three knobs — **gain**, **tone**, **level**.

| Type | What it models | Character |
|---|---|---|
| **Classic** | The stock 2004 `dist_dist()`, byte-for-byte | The CreakBox sound. Default. |
| **Rat** | ProCo RAT: a slew-limited op-amp into symmetric hard-clipping diodes, then a passive low-pass | The acid distortion. Roughly three quarters of pre-2000 acid records went through one. |
| **Screamer** | Tube Screamer: soft clipping in the feedback loop with the bass kept *out* of the clipper | Mid-forward and comparatively polite; thins a bassline out, which is why it cuts. |
| **Dist+** | MXR Distortion+: hard clipping with no slew limiting | Brighter and rawer than the RAT — that contrast is why both are here. |
| **Fuzz** | Germanium-ish asymmetric clipping at very high gain | Snarl and even harmonics. The stock clipper is asymmetric too (+0.8 / −0.78), so this is in keeping. Silent on rests: it used to leave +0.09 of DC there, which the ring modulator turned into a tone between the notes. |

**Classic is not a model, it is the original code path.** Type 0 calls the untouched
`dist_dist()` and the three knobs do nothing at all — verified byte-for-byte, not asserted.
That is what makes the whole block free: a song that never opens this dialog is bit-identical
to the engine before the block existed.

**Tone is the conventional way round — up is brighter.** A real RAT's "Filter" knob is wired
backwards, which is a famous piece of trivia and a bad property for a control that announces
its value to a screen reader, so the sense is corrected here.

**The knobs stay enabled while Classic is selected**, and say so in their labels instead
(`Gain (no effect — Classic)`). Disabling them would announce as *unavailable* with no
discoverable cause — the accessibility rule above.

**On levels.** At the default gain of 0 the models sit between 0.3× and 1.7× the Classic
output, so picking one is not a volume jump. Wind the gain up and hard clipping does what
hard clipping does — around 6× at gain 0.6, because the output level stops depending on the
input. That is what the **level** knob is for. Screamer is the quiet one (≈0.3×): its
high-pass keeps the bass out of the clipper, which on a bassline is most of the energy.

#### Compressor

Two controls, **threshold** and **depth**. Glue that evens out accents, not a limiter —
depth 1.0 is a 3.3:1 ratio and no further. Auto make-up keeps it from reading as a volume
knob: the transfer curve pivots at full scale, so quieter material comes up and the loudest
material stays where it was.

The follower has an instant attack and a 120 ms release. That is not a simplification. With
a smoothed attack an individual sample can sit above the envelope that graded it, and the
make-up gain then pushes it past full scale — measured at 0.19 % of samples clipped at an
ordinary setting. With a peak follower the output bound becomes provable rather than
measured: an input inside ±1 cannot leave the stage outside ±1, at any threshold and any
depth.

Depth at 0 skips the follower entirely, and the threshold label says so.

#### Ring modulator

Two controls, **frequency** (5 Hz–2 kHz, exponential) and **amount** (dry to pure ring). The
carrier is a sine independent of the note, which is what makes it clangorous and inharmonic
rather than well-behaved — the sound people mean by "ring mod on a 303". Below ~30 Hz it is
a per-note tremolo; the low hundreds are where a bassline turns metallic; the top is bell
territory.

**The carrier restarts on every new note**, at its peak. Three reasons, and it is worth
knowing they point the same way: a free-running audio-rate carrier would drift against the
bar, so a one-pattern loop would stop repeating identically — which this block's admission
rule forbids; restarting gives every note the same timbre instead of one that lands
differently each time; and starting at the carrier's peak rather than at a zero crossing
keeps the note's attack transient intact. Tied notes keep the phase they started with.

Note the deliberate contrast with an LFO's phase, which is never reset at a note or a bar:
that one is a slow modulator whose whole point is moving across the bar, at a rate where
movement is audible *as* movement. It follows the song position instead (see *LFOs*).

Amount at 0 skips the multiply entirely, and the frequency label says so.

### Sweeps — the modulation system

Sweeps are the canonical way to modulate anything in Screambox. Every continuous slider
carries its own list of sweeps - the seven parameter sliders, Accent squelch, and every
slider in the Oscillator and Acid FX dialogs; only the switches (waveforms, distortion
type, resonance boost) and Tempo do not. A sweep interpolates a slider value between a
`(pattern, step)` start point and a `(pattern, step)` end point. They can be copied and
pasted between parameters - by slider position, so a Cutoff sweep across most of its slider
pasted onto Ring frequency travels most of that one - saved on their own as `.hape`, and
are stored in every `.roq`.

Press **Enter** on a slider to open its sweep editor - on the main window and inside the
two dialogs alike. In the dialogs Enter used to press Close; **Alt+C** and **Esc** still
close them.

How they behave, precisely:

- **Evaluated at each step, in force from the step's first sample.** The value used to be
  picked up only at the next audio buffer, so every note started with the previous step's
  value for up to a buffer - longer on a sound card with bigger buffers. On Tuning that was
  a pitch blip at every note; on Cutoff it landed on the attack. A sweep now renders the
  same at every buffer size.
- **Even per step.** A sweep moves the same amount at every step it covers, across
  patterns of any length. (It used to count each pattern as one unit, so a sweep over a
  4-step and a 16-step pattern spent half its travel in the short one.)
- **After the end, the slider - or a hold.** By default the parameter returns to its
  slider when the sweep ends. Tick **Hold the end value until the next sweep** in the
  editor and it keeps the end value until the next sweep on that slider starts.
- **The sliders stay yours.** A sweep never changes what the slider is set to, and saving
  while a sweep is running saves the slider, not the sweep's momentary value. (It used to
  save the sweep's value as the slider setting.)
- **A swept slider says so in its name** - `Cutoff (2 sweeps)`, `Gain (1 sweep)` - which
  is what a screen reader reads on focus, since moving it does nothing a sweep will not
  override at the next step.
- **Frequencies glide in equal ratios.** Ring frequency sweeps geometrically - 100 Hz to
  1600 Hz passes 200, 400 and 800 at even spacing - which is what moving its exponential
  slider at an even speed does.
- **A sweep can switch an effect on.** Compressor depth and Ring amount are "off" at 0, and
  a sweep that raises them turns the effect on for as long as it does - while the slider,
  and the label that says "no effect", go on describing the slider.
- **Patterns move, sweeps follow.** Inserting a pattern moves every sweep after it along
  with its music; deleting one moves them back, clamps an endpoint that was inside the
  deleted pattern to its nearest neighbour, and removes a sweep that lay wholly inside it.
  Starting a new song - New random pattern, Import legacy .cb, Start new track - clears
  the sweeps, since they named the old song's patterns.

**The editor.** Pick a sweep by number, change it and press **Update**; **Add** makes a
new one. A sweep whose end is before its start is refused with a reason (it could never
play), as is one that overlaps another on the same slider. **Cancel** really cancels:
every change made since the dialog opened is undone. A new sweep starts at the slider's
current setting at both ends. The value sliders move like the slider they stand for -
cents for Duplet detune, the dialogs' exponential tapers for the two frequencies, 0-200 %
for Accent squelch.

**Shapes.** Below the buttons, **Shape kind** (`Alt+K`), **Cycles** (`Alt+Y`) and
**Generate shape** (`Alt+G`) fill the range set above - start to end, start value to end
value - with a shape repeated that many times:

- **Ramp** - start value to end value, every cycle; repeated, a sawtooth.
- **Up and down** - start to end and back, every cycle; repeated, a triangle.
- **Alternate** - the first half of each cycle at the start value, the second at the end.

What Generate adds are ordinary sweeps - as few straight ones as draw the shape exactly,
one value per step - so each piece can be edited, removed or copied like any other, and
nothing about a shape reaches the engine or the file. The shape is refused, with a reason,
if it would overlap a sweep already there, or if the range is too short for that many
cycles. Hold, if ticked, applies to the shape's last piece. Where you want one editable
record instead of many pieces, use an LFO.

### LFOs — sweeps that oscillate

There is no separate LFO module: **an LFO is a sweep with a wave** (`.roq` v15). Choose an
**LFO wave** (`Alt+W`) in the sweep editor - triangle, sine, square, saw up, saw down - and
the sweep stops setting the value and starts *moving* it:

- **It moves the value around whatever the slider would otherwise be** - the slider itself,
  or a straight sweep covering the same step. So a ramp and a wobble run at once: a Cutoff
  sweep from 10 to 70 with a sine LFO on top wobbles around the rising ramp. One straight
  sweep and one LFO may cover the same step; they never count as overlapping each other.
- **The two values become its depth** (the captions say *Start depth* and *End depth*),
  travelling from one to the other along the sweep as a straight sweep's value does - an
  LFO that fades in. The result stays inside the slider's range.
- **Frequency** (`Alt+Q`), **Measured in** (`Alt+M`) **steps per cycle**, which follows the
  tempo, or **Hz**, which does not; and the **Starting phase** in degrees (`Alt+I`). Every
  wave starts at its centre going up (the square at its top), so 0 means the same for all.
- **Hold** keeps it going at its end depth until the next LFO on that slider. An LFO that
  starts at the top of the song and holds is "always on", including through patterns added
  later.
- **Its phase follows the song position, not a free-running clock.** Every play of a song
  and every WAV export of it sound the same, and a looped pattern repeats exactly.
- **Main PWM and Duplet PWM move every sample**, so an LFO there runs smoothly up to audio
  rates - what the old PWM LFO did. **Every other slider moves once per step**, like any
  sweep: an LFO there is sampled at the step rate, and a fast one comes out in stairs.

**The Modulation dialog** (`Ctrl+M`, or the *Modulation* button) lists every slider that
can be swept, each read with what it carries - `Cutoff (2 sweeps, 1 LFO)`. **Enter** opens
the selected slider's sweep editor; **LFO preset** (`Alt+P`) and **Add LFO to the whole
song** (`Alt+A`) place a held, whole-song LFO on it - slow, bar, half-bar and beat wobbles,
a trill, rising and falling saws, and *The old PWM LFO*. A preset's depth is a fraction of
the slider's range, so one list serves every slider. What a preset makes is an ordinary
LFO: edit it, save it in a `.hape`, or remove it in the sweep editor.

**The PWM LFO this replaces** - one triangle at a rate in Hz, a depth into each PWM - opens
from an older song as exactly that: a triangle LFO in Hz on Main PWM and on Duplet PWM, from
the top of the song, holding. Rendered from the start, as a WAV export renders, it is
**bit-identical** to the old LFO (the tester proves it on songs the old build saved). It
differs on purpose after the song wraps back to pattern 1: the old LFO ran free, so every
play caught it somewhere else; the new one starts again with the song.

### Resonance boost

The filter clamps its resonance input at 1.0 and the knob already arrives at about 0.99,
so "more resonance" cannot be done by rescaling the knob — the clamp has to be crossed.
The filter has **no saturating nonlinearity in its feedback path**, so past the stability
point its state grows without bound and eventually goes NaN, permanently.

The switch therefore selects a *sibling* filter function,
`wwelti_IIR_IIRlp3pole_boost`, which raises the clamp slightly and bounds the resonant
integrator so the oscillation settles into a limit cycle. The stock
`wwelti_IIR_IIRlp3pole` is not edited at all, which is what makes "boost off" provably
identical to before.

The knob mapping *extends the ceiling* rather than lifting the whole range: a quartic
term is added to the resonance, inaudible through the usable travel and biting only at
the top. Nothing below roughly 70 % of travel changes audibly; self-oscillation starts
around 88–90 %.

### Pattern length

Each pattern is 1 to 64 steps, set on the **Pattern length** combo beside the step editor.
The step list rebuilds to match, so it always shows exactly the steps the pattern plays.

The sequencer never assumed 16. `CbPattern` has always carried 64 steps and the engine has
always looped at `lastStep`, derived song position from `lastStep + 1`, and picked the next
step from it. What was missing was a way to *say* anything but 16 — and, more importantly,
a way to save it: until `.roq` v10 the writer emitted exactly 16 step rows however long the
pattern claimed to be, so a 23-step pattern would have saved its declared length and
silently lost steps 17–23.

Three things worth knowing in use:

- **Shortening is not destructive within a session.** The steps past the new end keep their
  notes, and lengthening again brings them back. *Saving* is where they go: `.roq` writes
  only the live steps, which is what makes a short pattern short on disk.
- **Randomising the current pattern keeps its length.** It replaces the notes, not the
  shape. (Starting a *new* random pattern still gives you 16.)
- **Sweeps follow the pattern they name.** The sweep editor's step lists are sized to the
  pattern selected beside them, and change when you change that pattern. A sweep endpoint is
  positioned relative to its own pattern's length, so a sweep over a 16-step pattern ends
  where that pattern ends even when a 4-step pattern is playing.

Patterns of different lengths in one song is the point of the feature: 15 against 16 walks
the accents around the bar, and a short pattern against a long sweep is the cheap way to get
a phrase that never repeats the same way twice.

### Triplet feel

A checkbox beside Pattern length, **per pattern**, so a triplet fill can sit inside an
otherwise straight track. `Ctrl+T` from anywhere.

Off, a step is a **sixteenth** — sixteen of them to a 4/4 bar. On, a step is an **eighth-note
triplet**, a quarter divided by three, which is 4/3 longer. So:

| | 12 steps | 16 steps | 24 steps |
|---|---|---|---|
| **straight** | ¾ bar | 1 bar | 1½ bars |
| **triplet** | **1 bar** | 1⅓ bars | **2 bars** |

Twelve straight sixteenths is three quarters of a bar, so looping it against anything in 4/4
walks the downbeat around and re-aligns every three bars — a 3-against-4 polyrhythm. Useful,
and not a shuffle. Twelve *triplet* steps fill the bar exactly, which is the loping 12/8
groove.

**The point of the control is that it does that at the same tempo.** Getting the shuffle
before meant lying about the BPM: to make 12 steps fill one bar at 140 you had to type
**105** — a number with nothing to do with the music, out of step with every other device,
and recomputed by hand every time the real tempo moved. That arithmetic is what the checkbox
deletes.

Nothing else in the engine knows what a step is — the gate threshold, the note length a
reversed envelope is sized to, and the slide rate all derive from one number — so they follow
a triplet pattern without being told.

**The tempo is exact.** A step is rarely a whole number of samples - at 48 kHz and 140 BPM it
is 5142.86 - and the sequencer used to drop the fraction, running about 0.02 % fast: 50 ms
adrift of a DAW after five minutes, and twelve triplet steps 12 samples shorter than a
16-step bar. It now carries the fraction from step to step, so the bar is exact to a sample
at any rate. Where the step is already whole (44.1 kHz at 140 BPM) nothing changed at all.

### The envelope — and why there are four of them

Screambox inherits the 303's envelope arrangement verbatim, and it does not look like an
ADSR. There is no attack control, no sustain and no release. There is one **Decay** knob,
and it drives four separate pieces of state that all run at once. Anything described as
"changing the envelope" has to say *which* of the four it means, so they are set out here.

**1. `boxDecay_` — the filter envelope. This is the one people mean.**

Set to `4.0` at every new note (`triggerStep`, skipped on a slide) and then, every sample:

```cpp
boxDecay_ *= (knobDecay * 0.0005 + 0.9995);
```

A pure exponential decay toward zero. The multiplier runs from `0.9995` at knob 0
(τ ≈ 2000 samples ≈ 45 ms — fast) to exactly `1.0` at knob 1.0, where the envelope does not
decay at all and simply holds at 4.0. **Lower knob = faster decay**, which is worth saying
out loud because it catches people writing tests.

It reaches the filter here:

```cpp
tempcutoff = boxCutoff * getTab2Interpolated(
    boxMEG_ + (EnvMod * -2.0) + (boxDecay_ * EnvMod) + accent);
```

The two EnvMod terms collapse to `EnvMod * (boxDecay_ - 2.0)`. So the envelope is
**bipolar around `boxDecay_ == 2.0`**: the contribution starts at `+2·EnvMod`, crosses zero
when the decay passes 2.0, and heads for `−2·EnvMod`. The filter does not merely approach
the base cutoff — it sweeps *through* it and out the other side. That crossing is the
character of the sound.

How far it actually gets in one step matters. At Decay 0.70 and 130 BPM a 16th step is
about 5088 samples, and `boxDecay_` only travels 4.0 → ~1.9 in that time. The audible part
of a normal note is the **first, steepest section** of the curve, and the note is gated off
long before the envelope finishes.

**2. `boxMEG_` — the slow drift.** Reset to 0 at each note; ramps toward `−1.0` at
`0.0000082` per sample, or toward `−1.75` at `0.00009` when the step is accented. It is
added straight into the same cutoff index, so it is a gradual darkening of the whole filter
across the note. On a plain 16th it covers ~4 % of its travel — nearly nothing. On an
accented one, ~37 %. That asymmetry is a large part of why an accent does not just sound
louder.

**3. `boxVolEnv_` — the amplitude envelope, which barely does anything.** Set to 1.0 at each
note, decaying at `0.00001` per sample: τ ≈ 100 000 samples ≈ 2.3 s, far longer than any
step. Over one 16th it drops about 5 %. **A 303 note is essentially flat in amplitude.**

**4. `boxGate_` — what actually ends the note.** Rises to 1.0 with coefficient `0.9`
(about three samples — instant) and falls to 0.0 with coefficient `0.004` (τ ≈ 250 samples
≈ 5.7 ms — fast, but slow enough not to click). The sequencer closes it **partway through
the step, not at the end**:

```cpp
double threshold = (samplesPerStep_ * (1.0 / 1.9)) * (1.0 - (boxAccent_ ? 0.25 : 0.0));
if (remaining < threshold && gateStat_ == 0) gateStat_ = 1;
```

A plain note therefore sounds for about **47 %** of its step and an accented one for about
**61 %** — accents are longer as well as louder. On a slide the gate is never closed and
the note runs unbroken into the next step - **including across the bar line**, into step 1
of whichever pattern plays next (or of the same pattern, when looping it). That used to
fail: the filter reset that runs at every pattern start (inherited from 2004) cut the held
note off with a click and threw the glide away. It is now skipped while a slide carries a
note across.

**The summary worth carrying:** the loudness contour of a 303 note is *instant on, flat,
abrupt-but-smooth off*, and every expressive thing is happening in the filter. That is why
EnvMod, Decay and Accent are all filter controls, why the amplitude has no controls at all,
and why "reverse the envelope" is a statement about `boxDecay_` and nothing else.

---

### Reverse step (per-step reversed filter envelope)

**What it is for.** A normal step gives the filter opening then closing — the classic
*biu*. A reversed step plays that same envelope backwards, closing then opening, with the
bright moment arriving at the note's end — *bwi*. Before this flag a pattern committed to
one or the other. The point of **reverse step** is mixing them inside a single pattern:

```
biu  biu  biu  bwi  biu  bwi  biu  biu
```

One step sounding reversed against its neighbours, as if someone had flipped that step and
left the rest alone.

**Mechanically, it reverses the envelope in time.** The filter envelope `boxDecay_` normally
starts at 4.0 and decays by a fixed multiplier every sample (see "The envelope" above). A
reversed step starts it where the forward envelope would have *ended* and multiplies by the
**reciprocal**, so it climbs back to 4.0 and arrives there exactly as the note stops
sounding:

| | start of note | per sample |
|---|---|---|
| normal | `boxDecay_ = 4.0` | `*= r` |
| reversed | `boxDecay_ = 4.0 * pow(r, N)` | `*= 1.0 / r` |

Because `4·r^(N−t) == (4·r^N)·(1/r)^t`, that is genuinely the same curve read backwards —
not an upside-down one. The distinction is audible and it is the whole feature. An inverted
envelope moves fastest at the note's *start* and then flattens, which sounds like a filter
wobble; a reversed one is slow at first, accelerates, and peaks at the end, which is what a
backwards note sounds like. **Decay length becomes attack length.**

`N` is the note's *audible* length — its own step plus any steps tied to it by a slide, less
the tail of the final step after the gate closes. Aiming at the step boundary instead would
put the peak after the note had already gone silent.

**What it does not touch.** The amplitude path is untouched: loudness on a reversed step is
bit-for-bit what it would otherwise be, and the accent term stays additive. Only the filter
mirrors. It also never reads or writes EnvMod, so a sweep ramping EnvMod across the bar
behaves identically on reversed and normal steps.

**Slides carry the whole note.** The direction is latched only when a note actually starts,
not on every tied step, so a slid reversed note reverses across the entire tie rather than
changing direction partway through. That is the behaviour that makes a long reversed note
sound like a reversed long note.

**Two deliberate consequences:** the randomisers never set the flag, so random patterns come
out all-biu and every bwi is one you placed; and `resetTo303()` will not clear it, because
it is pattern data, not a sound parameter.

---

## File formats

All three are plain text, so they can be hand-inspected and diffed.

### `.roq` — song (read/write)

Estonian *röök*, "roar". Bundles patterns, every slider value and every sweep. Magic
`CreakboxRoq` (from before the rename, and kept: every saved song depends on it), then a
version integer, then one value per line.

Every version bump up to v9 only appended, so **old files load into new builds** with the
newer fields defaulting to their off/neutral values — a v1 song sounds today exactly as it
did.

**v10 is the exception, and it is handled by the version number rather than by the reader.**
Variable-length patterns changed the *number of step rows* in a pattern block, which is
positional and cannot be absorbed the way an appended field can. So the writer stamps the
lowest version that can represent the song: a song whose patterns are all 16 steps writes
exactly the bytes v9 wrote and still opens in an older build, and only a song that actually
uses another length declares v10. The header number means "what this file needs", not "what
wrote it".

v11 applies the same rule to an *appended* field for the first time, because the rule turned
out to be worth having generally: a song that never opened the Acid FX dialog still writes
v9 or v10 and still opens in an older build. The trigger is "some field differs from its
default", not "the effect would be audible" — a carrier frequency dialled in with the amount
left at zero is still something the user set, and dropping it to keep a version number down
would be the wrong trade.

v12 costs nothing at all, and it is worth seeing why. The triplet flag is appended as a
**second token on the pattern's `lastStep` line** rather than given a line of its own.
`readInt` parses with `std::stoi`, which reads the leading integer and stops — so an older
build reads `"11 1"` as `lastStep` 11 and simply never sees the flag. Same trick as the
per-step reverse column, and the reason to prefer extending a line over adding one.

The other direction is narrower than it sounds, and worth being precise about. A build
rejects a file whose version is higher than its own, with `unsupported .roq version` — it
does **not** try to read it and mis-parse. The header is positional (one value per line, in
a fixed order), so a partial read would silently shift every field after the missing one;
refusing is the right behaviour and it fails loudly. What *is* tolerant in both directions
is the **step line**, which grows by appending tokens rather than changing shape.

| Ver | Adds |
|---|---|
| 1 | patterns + 9 params + sweeps |
| 2 | `impulsive` (now deprecated, always written as 0) |
| 3 | `accentSquelch` |
| 4 | `pwm`, `subOscLevel`, `dupletWaveform`, `dupletDetune`, `dupletLevel` |
| 5 | `dupletDetune` becomes bipolar ±1200 cents; v4 values are rescaled on read |
| 6 | `dupletPWM` |
| 7 | `pwmLfoRate` (Hz), `pwmLfoDepth`, `pwmLfoDupletDepth` |
| 8 | `noiseLevel`, `resoBoost`, and a **fourth per-step column** (reverse step) |
| 9 | `distType`, `distGain`, `distTone`, `distLevel` |
| 10 | variable-length patterns — a pattern block holds `lastStep + 1` step rows instead of a fixed 16. **Written only when some pattern is not 16 steps.** |
| 11 | `compThreshold`, `compDepth`, `ringHz`, `ringAmount`. **Written only when one of them differs from its default.** |
| 12 | per-pattern triplet, as a second token on the pattern's `lastStep` line. **Written only when some pattern uses it**, and readable by older builds either way. |
| 13 | per-sweep "hold end value", as an eighth token on the sweep line. **Written only when some sweep holds.** An older build reading the line takes the first seven tokens and simply does not hold. |
| 14 | sweeps on Accent squelch and the Oscillator and Acid FX sliders - sweep targets 7 to 23 (listed in `CBEngine.h`). Not a byte of the layout changed; it is a version because an older build *silently drops* a sweep whose target is above 6, and would play the song without it. **Written only when some sweep has such a target**, so an older build refuses the song out loud instead. |
| 15 | LFOs: four tokens after the hold token on an oscillating sweep's line - wave, rate unit (0 steps per cycle, 1 Hz), rate, starting phase in cycles. **Written only when some sweep oscillates**; an older build would play one as a straight sweep between its two depths. The same version retires the PWM LFO: its three header lines stay (the header is positional) and are written as 0, and a song that used it opens with it converted. Sweep targets 14 to 16 (the LFO's own rate and depths) were retired with it before any release carried them. |

The v8 step line is `pitch slide accent reverse`. Adding a token rather than changing the
line's shape is what keeps it readable in both directions: an older three-token parser
extracts the first three and ignores the rest.

### `.hape` — sweeps only (read/write)

A sweep collection on its own, for reusing a set of sweeps across songs. Magic
`CreakboxHape`. Saved and loaded from inside the sweep editor. Stamped 9 - readable by any
build - unless a sweep in it holds its end value, which stamps 13 and adds the hold token,
the same "what the file needs" rule `.roq` follows; one carrying an LFO is stamped 15,
since an older build would keep it and play it as a straight sweep. v14's new sweep targets
do not raise it: the editor keeps only the sweeps for the slider being edited and says how many it
skipped, so an older build handed a squelch sweep reports it skipped rather than
misplaying anything.

### `.cb` — 2002 module (import only)

The original Delphi format: CRLF, Windows-1252, 49 lines per pattern. **Fixed at 16 steps
per pattern** — the format has no length field and the reader counts lines, so a pattern of
any other length cannot be expressed in it at all. `saveCb()` exists
in the source but nothing in the UI reaches it — the File menu offers only **Import
legacy .cb**. Treat the format as read-only. Its schema is documented in
`reverse/cb_schema.md`.

### WAV render

**File → Save to WAV** renders the full song order once through, offline, into a 16-bit
WAV at the audio device's sample rate. It uses a detached engine instance, so the live
one keeps playing, and gives it the whole song: patterns, every parameter and every sweep.
The length is the song's real length - pattern lengths and triplet feel included - computed
with the sequencer's own arithmetic. (Until 2026-11 it rendered no sweeps at all and assumed
every pattern was 16 straight steps.) It streams to disk, so a long song needs no memory to
speak of, and a failed write is reported rather than leaving a truncated file.

---

## Keyboard

`F1` in the app shows the built-in reference. The bindings, as actually registered in
`src/resource.rc`:

**File** — `Ctrl+N` new random pattern · `Ctrl+Shift+N` start new track · `Ctrl+O` open
`.roq` · `Ctrl+S` save `.roq` · `Ctrl+I` import `.cb` · `Ctrl+W` save WAV · `Ctrl+D`
distortion panel

**Edit** — `Ctrl+L` clear pattern · `Ctrl+C` / `Ctrl+X` / `Ctrl+V` copy / cut / paste
pattern · `Ctrl+E` random (nicely) · `Ctrl+R` random (retro 2002) · `Ctrl+Shift+R` random
(VST) · `Ctrl+Shift+E` random (chromatic)

**Transpose** — `Ctrl+Up` / `Ctrl+Down` ±1 semitone · `Ctrl+Shift+Up` / `Ctrl+Shift+Down`
±1 octave (current pattern; whole-song transpose is menu-only)

**Step flags, from anywhere** — `Alt+A` accent · `Alt+S` slide · `Alt+I` reverse step

**Dialogs** — `Ctrl+D` distortion panel · `Ctrl+M` modulation

**Pattern** — `Ctrl+Left` previous · `Ctrl+Right` next · `Ctrl+Shift+P` new ·
`Ctrl+Shift+Del` delete

**Transport** — `F5` play current song from pattern 1 · `F6` loop the current pattern ·
`F7` play from the current pattern to the end · `F8` stop

F5–F8 are unconditional, not toggles, so F5 while already playing restarts the song and
F8 is safe to press when stopped. The on-screen button is still a toggle, and is
equivalent to F5 / F8.

**`Space` is deliberately not a transport key.** It was bound to run/stop, and because
accelerators are processed before `IsDialogMessage`, that swallowed the keystroke before
any control could see it — Space was dead on every checkbox and button in the window.
Transport moved to F5–F8 to give it back.

**Focused step**, from anywhere in the window: `Ctrl+Shift+A` accent · `Ctrl+Shift+S` slide
· `Ctrl+Shift+I` reverse · `Ctrl+Shift+G` randomise. One modifier for one idea — everything
that acts on the step the step list has focused.

**Sliders** — arrows ±1 · PgUp/PgDn ±10 · Home/End to the rails · **Enter opens the sweep
editor**, in the Oscillator and Acid FX dialogs too (close those with `Alt+C` or `Esc`)

**Checkboxes** — `Space` **or Enter** toggles. Space is the system behaviour (it only
works now that transport has vacated it); Enter is a subclass, because
`IsDialogMessage` routes Enter to the default button and this window has none, so without
it Enter on a checkbox would do nothing while Enter on a slider opens a dialog.

### Mnemonics

A mnemonic that appears on two controls does **not** activate either — Win32 cycles focus
between the matches, which presents as "the key does nothing". Every live mnemonic on the
main window is now unique:

| | | | |
|---|---|---|---|
| `Alt+W` Waveform | `Alt+M` Tempo | `Alt+Q` Accent squelch | `Alt+I` Wildness |
| `Alt+V` Volume | `Alt+C` Cutoff | `Alt+D` EnvMod | `Alt+Y` Decay |
| `Alt+U` Tuning | `Alt+B` Boost resonance | `Alt+O` Oscillator… | `Alt+X` Acid FX… |
| `Alt+T` Step selector | `Alt+N` Note | `Alt+A` Accent | `Alt+S` Resonance |
| `Alt+R` Run/Stop | `Alt+G` Pattern length | | |

**Triplet feel** has no mnemonic — every letter of it is taken — and lives on `Ctrl+T`;
**Modulation…** likewise, on `Ctrl+M`.

Several moved off their first letter to break a collision, which is why they read oddly:
**Te&#95;m&#95;po**, **Deca&#95;y&#95;**, **T&#95;u&#95;ning**, **W&#95;i&#95;ldness** and
**EnvMo&#95;d&#95;**. A working awkward letter beats a natural one that does nothing.

**Three orders of precedence decide who gets a key, and every loser fails silently.** The
menu bar is consulted first, then accelerators, then mnemonics. So a main-window mnemonic on
`F`, `E`, `P`, `H` or `L` can never fire (those belong to the menus), and a mnemonic
shadowed by an accelerator on the same chord can never fire either.

Both used to happen here. `Alt+A`, `Alt+S` and `Alt+R` were bound as accelerators, so the
`&Accent` slider, `Re&sonance` slider and `&Run` button advertised keys they could never
receive; those accelerators moved to `Ctrl+Shift+A` / `Ctrl+Shift+S` / `Ctrl+Shift+G`.
`&EnvMod` advertised `Alt+E`, which only ever opened the Edit menu. **No plain `Alt+letter`
is bound as an accelerator any more**, which is what makes that whole class unrepeatable,
and `tools/check_keyboard.py` fails the build's keyboard check if one comes back.

Controls with no mnemonic left — the pattern combo, the three per-step checkboxes — say so
by having no underline, which is honest. **Their label names the chord instead**: `Slide this
step (Ctrl+Shift+S)`, `Triplet feel (Ctrl+T)`. A key nothing announces is a key nobody finds,
and the label is the only place a screen reader will read it.

A mnemonic would be the wrong shape for the per-step toggles in any case. They are used
*while arrowing through the step list*, and a mnemonic moves focus to the control it names —
so every toggle would throw you out of the list and cost a Tab to get back. An accelerator
fires without touching focus.

### Random-pattern flavours

Four pitch pools: **Nicely** (C-minor pentatonic), **Retro 2002** (the Delphi build's
Eb blues pool), **VST** (the 2004 C-major pentatonic) and **Chromatic** (all 60 pitches).
The wildness slider scales rest / slide / accent density for all of them.

---

## Repository layout

```
src/            standalone sources (compiled) + 2004 VST sources (reference only)
                  Distortion.* and AcidFx.* are the acid FX block; dist.cpp is the
                  untouched 2004 clipper the Classic model still calls
tools/          two standalone checks; no build, no dependencies
reverse/        reverse-engineering notes on the 2002/2004 originals
resource/       original 2002 resources
creakbox.txt    Bass Assessor's original 2002 release note
LICENSE         BSD 3-Clause, for this extension
license-bmd.txt the 2004 bioroid media development license for the VST sources in src/
```

Kept in the working tree but deliberately **not** in the repository: `modules/` (the `.cb`
presets and `.roq` songs), `CREAKBOX.EXE` (the original 2002 Delphi build), `cb303.wav`,
`reverse/ghidra_proj/` and `build/`. References to them elsewhere in this file and in
`CLAUDE.md` describe the local tree.

Run both checks after touching the things they cover. Each is stdlib-only Python and takes
under a second:

```
python tools/check_keyboard.py        after any resource.rc or control-label edit
python tools/check_dsp_ownership.py   after any CBEngine edit
```

They exist because everything they catch is **silent** — no error, no warning, no log line.
The keyboard one covers the seven ways a key can quietly do nothing here; the ownership one
checks that non-atomic DSP state is written only on the audio thread, which is the property
ThreadSanitizer would have verified if MSVC shipped it.

`reverse/` is worth reading before changing anything engine-adjacent:

- `cb_schema.md` — the `.cb` file schema, derived by inspecting the preset library
- `parameter_map.md` — every control the 2002 Delphi UI exposed
- `signal_flow.md` — per-sample signal flow, 2002 exe vs. 2004 C++ source

---

## Engine invariants

These are load-bearing. Breaking one is silent — nothing fails to compile and nothing
throws.

1. **`resetFilters()` must not touch the LFOs' phases (`osc_`).** It zeroes the
   oscillator phases and runs at every pattern start. Adding the LFO phases there — which
   is exactly where they look like they belong — silently converts every LFO into a
   per-pattern retrigger. Their phase belongs to the song position.
2. **`wwelti_IIR_IIRlp3pole` stays byte-for-byte as it is.** New filter behaviour goes
   into a sibling function. The stock path being unedited is the proof that the stock
   sound is unchanged.
3. **No allocation and no locking on the audio thread.** `process()` reads a const
   reference into a double-buffered snapshot and touches no mutex and no allocator. The
   rule is enforced, not hoped for: a test counts allocations inside `process()` with an
   `operator new` hook and asserts zero.
4. **Every new feature's off-state must be bit-identical** to the engine before it. This
   is testable, not aspirational: build the engine twice — current, and the previous
   engine reconstructed by reversing the patch — render both headlessly, and compare the
   bytes. The PWM LFO round and the noise / boost / reverse round were both verified this
   way, and the current build with every addition switched off is still bit-identical to
   the original pre-LFO engine. Since 2026-11-09 `tools/check_sound.py` makes the
   comparison one command (see `CLAUDE.md`).
5. **Skip work entirely when a feature is off**, rather than multiplying by a neutral
   value. The noise RNG must not advance at level 0, and neither the compressor's follower
   nor the ring carrier's phase may advance at amount 0 — otherwise invariant 4 stops
   holding the moment anyone touches the control and puts it back.
6. **State that never forgets needs resetting; state that forgets does not.** The ring
   carrier's phase is re-zeroed on every new note, because a phase accumulator drifts
   forever and would stop a one-pattern loop repeating identically. The compressor's
   envelope follower is deliberately *not* reset at pattern start, because a one-pole
   contracts — it converges within a fraction of a loop from any state, and resetting it
   would manufacture a burst of make-up gain on beat 1 of every bar. The LFOs are the
   third case: their phase is a function of the song position, taken afresh only where
   playback jumps (Play, a loop, the wrap to pattern 1) and accumulated in between.

---

## Known issues and loose ends

Tracked in full, with approach and acceptance tests, in **[`TODO.md`](TODO.md)**.

- **Legacy `.cb` sweeps are parsed and round-tripped but never applied.** The apply path is
  deliberately inert — the per-tab value curves are not fully decoded and applying the raw
  values silences many real presets. Both the inertness and the reason are commented at the
  call site so it is not deleted as dead code or switched on without decoding them first.

---

## License

The original CreakBox was released as freeware by Bass Assessor in 2002, free to use in
your own tracks (see `creakbox.txt`). Screambox, the extension of it, is released under
the BSD 3-Clause License — see `LICENSE`.

The 2004 VST sources in `src/` (and the DSP the standalone reuses from them) are
© 2004 bioroid media development under the BSD-style terms in `license-bmd.txt`, which
must travel with them.
