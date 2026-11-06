#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "IIR.h"
#include "AcidFx.h"
#include "Distortion.h"

// Step of a pattern — mirrors the 2004 source's t_CB_Step.
struct CbStep {
    int  pitch = 0;    // 0 = rest, 1..60 = C0..B4 per cNoteFreq table
    bool slide = false;
    bool accent = false;
    // "Reverse step": plays this note's filter envelope backwards in time -
    // slow first, brightest as the note ends - so one step sounds reversed
    // against its neighbours. A time reversal of boxDecay_, NOT a sign flip on
    // EnvMod; see CLAUDE.md invariant 8 for why the difference matters.
    bool reverseStep = false;
};

// One pattern: 1..64 steps (lastStep + 1), 16 by default.
struct CbPattern {
    int lastStep = 15;
    // Per pattern, not per song, so a triplet fill can sit inside a straight
    // track. A step becomes an eighth-note triplet (a quarter / 3) instead of a
    // sixteenth (a quarter / 4), i.e. 4/3 longer - so twelve steps fill exactly
    // one 4/4 bar at the SAME tempo. Without it, twelve sixteenths is three
    // quarters of a bar and loops as a 3-against-4 polyrhythm; getting the
    // shuffle instead used to mean typing 105 BPM to play at 140.
    bool triplet = false;
    CbStep steps[64]{};
};

// Parameter identifiers for the user-facing surface. Matches the 2002 UI
// groupings, not the exhaustive VST ~200-param list.
enum class CbParam {
    Volume,
    Waveform,   // 0 = saw, 1 = square
    Tuning,     // -1..+1, 0 = centre
    Cutoff,     // 0..1
    Resonance,  // -1..+1 (negative = Frank's "inverted" filter)
    EnvMod,     // -1..+1
    Decay,      // 0..1
    Accent,     // 0..1
    Tempo,      // BPM
    RunStop,    // 0 / 1
    AccentSquelch, // 0..2, default 1.0 = Frank's original TB-303 accent buildup.
                   // 0 = no stacking, each accent equal; >1 amplifies residual
                   // for progressively squelchier consecutive accents.
    // Oscillator-dialog parameters (set by IDD_OSCILLATOR). Defaults match
    // the original TB-303 voicing — at zero/50% they are transparent.
    PWM,            // 0..1, default 0.5 = 50 % duty for the MAIN square.
    SubOscLevel,    // 0..1, default 0 = off. Sine one octave below mixed in.
    DupletWaveform, // 0 = saw, 1 = square (default 1).
    DupletPWM,      // 0..1, default 0.5 = 50 % duty for the DUPLET square (independent of main PWM).
    DupletDetune,   // -1..+1 → ±1200 cents detune for the duplet oscillator.
    DupletLevel,    // 0..1, default 0 = off. Mix level for the duplet.
    // One shared triangle LFO driving both PWM destinations: a single rate,
    // an independent depth per destination. Sweeps are evaluated once per
    // sequencer step and top out near 10 Hz, so they cannot reach these
    // rates — this is the engine's only continuous per-sample modulator.
    PwmLfoRate,        // 0..100, stored as Hz. 0 = off. Taper lives in the UI.
    PwmLfoDepth,       // 0..1, default 0 = off. Depth into the MAIN PWM.
    PwmLfoDupletDepth, // 0..1, default 0 = off. Depth into the DUPLET PWM.
    NoiseLevel,     // 0..1, default 0 = off. White noise mixed into the
                    // oscillator sum, so it is shaped by the filter and gate.
    ResoBoost,      // 0 / 1, default 0. Lifts the top of the resonance range
                    // past the filter's stability point (Devilfish-style).
    // Distortion stage, after the filter and volume envelope — where a pedal
    // sits relative to a real 303. Type 0 (Classic) is the stock 2004 clipper
    // and the default, so the shipped sound is unchanged.
    DistType,       // 0..4, see CbDistType. 0 = Classic.
    DistGain,       // 0..1, default 0 = least drive.
    DistTone,       // 0..1, default 0.5. Up is brighter.
    DistLevel,      // 0..1, default 1.0 = unity output.
    // The rest of the acid FX block, in chain order after the distortion:
    // compressor, then ring modulator, then the output clamp. Both default to
    // off, and off means skipped, not multiplied by a neutral value.
    CompThreshold,  // 0..1, default 0.5. Where compression starts; up = later.
    CompDepth,      // 0..1, default 0 = off. Up to a 3.3:1 ratio at 1.0.
    RingHz,         // Hz, stored directly, default 100. Carrier frequency;
                    // independent of the note, which is what makes it clang.
    RingAmount,     // 0..1, default 0 = off (dry). 1 = pure ring.
    NumParams,
};

// Thread-safe synth engine. Contains the full 2004 Frank Reitz / "wwelti"
// TB-303 DSP lifted from src/SilverBox.cpp, converted from VST plumbing to a
// standalone class. Pattern/params are updated from the UI thread; audio
// pulls them via atomics on the render thread.
class CBEngine {
public:
    CBEngine();

    // Called by audio thread. `out` is interleaved stereo float32.
    void process(float* out, int frames);

    // Parameter access — atomic float store/load.
    void  setParam(CbParam p, float v);
    float getParam(CbParam p) const;

    // Pattern / sequencer state.
    void setPattern(const CbPattern& p);      // single-pattern convenience
    void setPatterns(const std::vector<CbPattern>& v);  // multi-pattern order
    CbPattern getPattern() const;             // currently-visible pattern
    CbPattern getPatternAt(int idx) const;    // specific pattern
    void setPatternAt(int idx, const CbPattern& p);  // edit a specific pattern
    // Sets the PLAYBACK position. Only the transport calls this (F6 / F7 start
    // at the edited pattern); editing has its own index in MainWindow.
    void selectPattern(int idx);
    // Structural edits take the index explicitly and never touch the playback
    // position. While playing, the audio thread follows the pattern it was
    // playing by identity (SongData::patIds), so inserting or deleting a
    // pattern elsewhere does not make the playhead jump.
    int  insertPatternAfter(int idx);         // returns the new pattern's index
    bool deletePattern(int idx);              // false if only one remains
    int  currentPatternIndex() const;
    int  patternCount() const;
    // Reads displayStep_, not currentStep_. The sequencer's own counter is
    // audio-thread-only state read once per SAMPLE, so making it atomic would
    // put an atomic load in the hot loop to serve a UI that repaints at 30 Hz.
    // The audio thread publishes it once per STEP instead, which costs one
    // relaxed store every few thousand samples and makes the UI's read legal
    // rather than merely benign.
    int  currentStepIndex() const {
        return displayStep_.load(std::memory_order_relaxed);
    }  // 0..lastStep, or -1 if stopped

    // Legacy per-tab 2-breakpoint sweeps (from 2002 .cb format). Kept for
    // round-tripping but not applied by default — every caller passes
    // enabled=false today, so the apply path in process() is inert.
    //
    // Indexed in 2002 .cb TAB order: Volume, Tuning, Cutoff, Resonance,
    // EnvMod, Decay, Accent. That is deliberately different from ParamSweep
    // below, which uses UI slider order. Verified against the Delphi form,
    // a Ghidra decode of the original exe, and the loader; see the long note
    // at the apply site in CBEngine.cpp before touching either table.
    struct Sweep {
        int order0 = 0, off0 = 0, val0 = 100;
        int order1 = 9999, off1 = 0, val1 = 100;
    };
    void setSweeps(const Sweep sweeps[7], bool enabled);

    // New-style interactive sweeps from the sweep editor UI. Each sweep
    // spans (startPat, startStep) to (endPat, endStep), interpolating
    // a raw slider value (-100..+100 bipolar, 0..100 unipolar) between them.
    // Indexed by UI slider order: 0=Volume, 1=Cutoff, 2=Resonance, 3=EnvMod,
    // 4=Decay, 5=Accent, 6=Tuning. Note Tuning is LAST here and SECOND in the
    // legacy .cb Sweep above; the two orderings are both correct and must not
    // be reconciled. This is the order `.roq` and `.hape` store as sliderIdx.
    struct ParamSweep {
        int startPat = 0, startStep = 0, startVal = 0;
        int endPat   = 0, endStep   = 15, endVal  = 0;
        // After the sweep ends, keep its end value until the next sweep on the
        // same slider starts, instead of snapping back to the slider. Off is
        // the original behaviour.
        bool hold = false;
    };
    void setParamSweeps(int sliderIdx, const std::vector<ParamSweep>& sweeps);

    // Transport.
    enum class PlayMode { FullSequence, CurrentPatternOnly };
    void setRunning(bool run, bool restartFromFirstPattern = true);
    void setPlayMode(PlayMode m);
    PlayMode playMode() const { return playMode_.load(); }
    bool isRunning() const;

    // Four flavors of random-pattern generator, each with its own pitch
    // palette:
    //   Nicely     — C-minor pentatonic (current rebuild pool), acid-bass friendly
    //   Retro      — 2002 Delphi "Chaos nicely" Eb-blues pool (Eb/F#/G/G#/Bb/C#)
    //   VST        — 2004 VST PatternRandom2 C-major pentatonic
    //   Chromatic  — no scale restriction, any of the 60 pitches
    enum class RandomFlavor { Nicely, Retro, VST, Chromatic };

    // Build a random 16-step pattern. `wildness` in 0..1 scales rest / slide
    // / accent density (0 = very sparse, 0.5 ≈ 2002 defaults, 1 = chaotic).
    // `lastStep` defaults to 15 so existing callers get the 16-step pattern
    // they always got; pass a pattern's own lastStep to re-randomise it
    // without changing its length.
    static CbPattern makeRandomPattern(RandomFlavor flavor, float wildness, int lastStep = 15);
    // Generate one step with the same pool + density rules. Used for the
    // "Random focused step" action.
    static CbStep    makeRandomStep(RandomFlavor flavor, float wildness);

    // Audio engine provides sample rate on start.
    void setSampleRate(double sr);

    // The exact (fractional) length of one step, in samples. The sequencer
    // carries the fraction from step to step, so the whole-sample step lengths
    // it actually plays average to this exactly and the tempo does not drift.
    static double exactStepSamples(double sampleRate, double tempo, bool triplet);
    // Samples the whole song takes once through from pattern 1, computed with
    // exactly the arithmetic the sequencer uses. For the WAV export.
    static long long songLengthSamples(const std::vector<CbPattern>& pats,
                                       double sampleRate, double tempo);

private:
    void resetFilters();

    // What the transport has asked for but the audio thread has not applied
    // yet. setRunning() used to call resetFilters() directly, on whichever
    // thread pressed the button, while process() might be halfway through a
    // block - the last data race in the engine, on a dozen non-atomic DSP
    // fields. The request is now a single atomic and the audio thread does the
    // work itself, at a block boundary, which is also better behaviour than a
    // reset landing mid-buffer.
    enum class TransportReset : int {
        None = 0,
        StartFromFirst,   // play from pattern 1
        StartFromCurrent, // play, leaving the pattern index alone
        Stop,
    };
    std::atomic<int> transportReset_{int(TransportReset::None)};
    void applyTransportReset(TransportReset r);
    void triggerStep(const CbStep& s, const CbStep& next);
    double noteFreq(int pitch) const;

    double sampleRate_ = 48000.0;
    // The user's settings, and ONLY the user's settings. Sweeps used to write
    // their values in here, which made getParam() - and so Save, the WAV
    // export and the sliders - pick up whatever a sweep happened to be doing
    // as if it were the slider. Sweep values now live in sweptVal_ below,
    // which only the audio thread touches.
    std::atomic<float> params_[static_cast<int>(CbParam::NumParams)]{};

    // ---------------- Song data: double-buffered, never locked by audio -----
    //
    // Everything the audio thread needs to read about the song, in one blob so
    // it can be swapped atomically. process() used to copy all of this into
    // locals under a mutex on EVERY callback — eight heap allocations inside
    // the real-time path, under a lock the UI thread also took. Now the audio
    // thread takes a const reference to whichever buffer is live and touches
    // no mutex and no allocator at all.
    struct SongData {
        std::vector<CbPattern> patterns{CbPattern{}};   // never empty
        Sweep sweeps[7]{};
        bool  sweepsEnabled = false;
        std::vector<ParamSweep> paramSweeps[7];
        // Derived by commit(), never edited directly. patStart[p] is the
        // absolute step number pattern p starts at (size patterns + 1), so a
        // sweep can advance evenly per step across patterns of different
        // lengths. patIds gives every pattern a stable identity, which is how
        // the audio thread keeps playing the same pattern when one is
        // inserted or deleted in front of it. serial changes on every commit.
        std::vector<int>      patStart;
        std::vector<uint32_t> patIds;
        uint32_t nextPatId = 1;
        uint32_t serial    = 0;
    };
    // Recomputes patStart, and patIds if a writer replaced the pattern list
    // without maintaining them. Called by commit() and the constructor.
    static void reindex(SongData& d);
    // Fresh identities for every pattern: the song was replaced wholesale.
    static void freshIds(SongData& d);

    // The protocol, single-writer (UI) / single-reader (audio):
    //
    //   audio  publishes the index it is about to read into readerIdx_, then
    //          re-reads liveIdx_ and retries if it changed. Publish-then-check
    //          is what makes it safe: without the re-read, a writer could pick
    //          the same buffer as its next target in the window between the
    //          reader loading the index and announcing it.
    //   writer takes writerMu_ (which the AUDIO THREAD NEVER TOUCHES), waits
    //          for readerIdx_ to leave the idle buffer, copies live -> idle,
    //          mutates it, then flips liveIdx_. All waiting happens on the UI
    //          thread, and the wait is bounded by one audio block.
    //
    // Copying live -> idle reuses the idle buffer's existing vector capacity,
    // so steady-state editing does not allocate on either thread.
    SongData song_[2];
    std::atomic<int> liveIdx_{0};
    std::atomic<int> readerIdx_{-1};
    mutable std::mutex writerMu_;   // UI-side only. Never lock this from audio.

    // RAII writer. Copies live into the idle buffer, hands it over to be
    // mutated, and publishes on commit(). Without a commit() nothing is
    // published, so an early-returning mutator costs a copy and changes
    // nothing — which is what every bounds-check path wants.
    class SongWriter {
    public:
        explicit SongWriter(CBEngine& e);
        SongData& data() { return e_.song_[next_]; }
        void commit();
        ~SongWriter() = default;
    private:
        CBEngine& e_;
        std::lock_guard<std::mutex> lk_;
        int next_;
    };
    friend class SongWriter;

    // Read the live song from the UI thread. Safe without the writer lock —
    // writers only ever mutate the idle buffer — but callers that need a
    // consistent multi-field view should hold writerMu_.
    const SongData& liveSong() const { return song_[liveIdx_.load(std::memory_order_acquire)]; }

    // Playback position. Written by BOTH threads (the audio thread advances it,
    // the UI selects a pattern), so it is deliberately NOT part of SongData —
    // it is a lone atomic instead, which is what removed the audio thread's
    // second lock inside the sequencer tick.
    std::atomic<int> patternIdx_{0};

    std::atomic<bool> running_{false};
    std::atomic<int>  displayStep_{-1};   // published for the UI; see currentStepIndex()
    std::atomic<PlayMode> playMode_{PlayMode::FullSequence};

    // Sequencer bookkeeping (audio thread).
    int    currentStep_ = 0;
    int    samplesIntoStep_ = 0;
    int    samplesPerStep_ = 0;    // length of the CURRENT step, whole samples
    // The fraction of a sample the steps before this one have not yet been
    // paid. Each step is floor(exact + stepFrac_) long and passes the rest on,
    // so step lengths average to the exact value and the tempo cannot drift.
    // Where the exact length is whole (44.1 kHz at 140 BPM) this stays 0 and
    // every step is the length it always was.
    double stepFrac_ = 0.0;
    // Which song buffer the audio thread last saw, and the identity of the
    // pattern it was playing - see SongData::patIds.
    uint32_t lastSongSerial_ = 0;
    uint32_t playingPatId_   = 0;

    // Sweep overrides, indexed by CbParam. Evaluated at each step boundary and
    // folded into the knob values at once; cleared on stop and on start.
    // Audio thread only (tools/check_dsp_ownership.py).
    float sweptVal_[static_cast<int>(CbParam::NumParams)]{};
    bool  sweptOn_[static_cast<int>(CbParam::NumParams)]{};

    // DSP state — names preserved from SilverBox.cpp for readability.
    double boxOscFrq_ = 0.0;
    double boxOsc_ = 0.0;
    double dupletOsc_ = 0.0;   // independent phase accumulator for duplet voice
    double subOscPhase_ = 0.0; // phase accumulator for sine sub oscillator
    // PWM LFO phase, 0..1. FREE-RUNNING BY DESIGN — deliberately NOT reset in
    // resetFilters() alongside the oscillator phases above, so the modulation
    // drifts across pattern boundaries instead of relocking every bar.
    // Resetting it here would silently turn this into a per-pattern retrigger.
    double pwmLfoPhase_ = 0.0;
    // xorshift32 state for the noise source. Fixed seed so renders are
    // reproducible; only advanced when the noise level is above zero, which
    // keeps the level-0 path bit-identical to the pre-noise engine.
    uint32_t noiseState_ = 0x9E3779B9u;
    // Latched by triggerStep() on a genuine new note (not on tied steps):
    // runs boxDecay_ backwards for the whole note - see the use site.
    bool   stepReverse_ = false;
    double boxSlide_ = 1.0;
    double boxDecay_ = 2.5;
    double boxMEG_ = 0.0;          // main envelope generator
    double boxGate_ = 0.0;
    double boxVolEnv_ = 1.0;
    double boxAccentFrq_ = 0.0;
    double boxAccentEnv_ = 0.0;
    double tempAccentFrq_ = 0.0;
    double tempAccentEnv_ = 0.0;
    bool   boxAccent_ = false;
    bool   bNoteSlide_ = false;
    bool   bSlideNext_ = false;
    int    gateStat_ = 1;          // 0 = open, 1 = shut
    int    boxSndType_ = 1;        // 0 = saw, 1 = square — driven by Waveform param

    CbDistState distState_{};   // filter memory for the non-Classic models
    CbCompState compState_{};   // the compressor's envelope follower
    // Ring carrier phase, in cycles. Re-zeroed on every new note (triggerStep),
    // NOT free-running - see the note there and in resetFilters().
    double ringPhase_ = 0.0;

    tbiquad lp2_{};      // main 3-pole resonant low-pass
    tbiquadFIR lp3FIR_{};// anti-alias FIR
    tbiquad hp1_{};      // high-pass brightener
};
