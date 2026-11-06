// Creakbox DSP engine — lifted from src/SilverBox.cpp (Frank Reitz / Bioroid
// 2004, BSD-2-Clause) with wwelti's DSP-loop optimizations, stripped of VST
// plumbing and adapted to a standalone realtime audio engine.
//
// The synth core (oscillator, 3-pole resonant LP via IIR.cpp, envelope,
// distortion via dist.cpp) is preserved; timing and parameter routing now
// come from this class's own state rather than the VST host.

#include "CBEngine.h"
#include "IIR.h"
#include "dist.h"
#include "core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <thread>

namespace {

// 60-note frequency table for pitches 1..60. Index 0 = rest.
// Copied from src/SilverBox.cpp (cNoteFreq[] at line 28), with ONE deliberate
// correction: entry 52 (D#4 in the UI's naming) is 2489.016, not the 2389.016
// the 2004 table has. That was a typo - it sat between 2349.318 and 2637.020,
// ~71 cents flat - and every other entry is a clean equal-tempered value. Do
// not "restore" it from the reference source.
const double cNoteFreq[61] = {
    0,
    130.813, 138.591, 146.832, 155.563, 164.814, 174.614, 184.997, 195.998, 207.652, 220.000, 233.082, 246.942,
    261.626, 277.183, 293.665, 311.127, 329.628, 349.228, 369.994, 391.995, 415.305, 440.000, 466.164, 493.883,
    523.251, 554.365, 587.330, 622.254, 659.255, 698.456, 739.989, 783.991, 830.609, 880.000, 932.328, 987.767,
   1046.502,1108.731,1174.659,1244.508,1318.510,1396.913,1479.978,1567.982,1661.219,1760.000,1864.655,1975.533,
   2093.005,2217.461,2349.318,2489.016,2637.020,2793.826,2959.955,3135.963,3322.438,3520.000,3729.310,3951.066,
};

// 2^x lookup table from src/SilverBox.cpp. Range [-8, +8], 32 steps per unit.
constexpr int kFunTab2Range = 16;
constexpr int kFunTab2First = -8;
constexpr int kFunTab2Acc   = 32;
constexpr int kFunTab2Size  = kFunTab2Range * kFunTab2Acc;
double funTab2[kFunTab2Size + 1];
bool funTab2Ready = false;

void initFunTab2() {
    if (funTab2Ready) return;
    for (int i = 0; i < kFunTab2Size + 1; ++i) {
        double v = double(i) / kFunTab2Acc + kFunTab2First;
        funTab2[i] = std::pow(2.0, v);
    }
    funTab2Ready = true;
}

inline double getTab2Interpolated(double val) {
    double t = (val - kFunTab2First) * kFunTab2Acc;
    if (t < 0)              t = 0;
    if (t > kFunTab2Size-1) t = kFunTab2Size - 1;
    int i = static_cast<int>(t);
    double rest = t - i;
    return funTab2[i] + (funTab2[i + 1] - funTab2[i]) * rest;
}

constexpr double kTwoPi = 6.28318530717958647692;

} // namespace

CBEngine::CBEngine() {
    initFunTab2();
    initFunTab1();  // used inside wwelti_IIR_IIRlp3pole
    dist_initdist();

    // Sensible defaults. Bipolar params (Tuning, Cutoff, Resonance, EnvMod)
    // use range -1..+1 with 0 = neutral — matches the UI's bipolar sliders.
    // Unipolar params (Volume, Decay, Accent) use 0..1.
    params_[int(CbParam::Volume)].store(1.0f);
    params_[int(CbParam::Waveform)].store(1.0f);   // square
    params_[int(CbParam::Tuning)].store(0.0f);     // bipolar, 0 = no detune
    params_[int(CbParam::Cutoff)].store(0.15f);    // small positive
    params_[int(CbParam::Resonance)].store(1.0f);  // max resonance
    params_[int(CbParam::EnvMod)].store(0.10f);    // slight positive
    params_[int(CbParam::Decay)].store(0.90f);
    params_[int(CbParam::Accent)].store(1.0f);
    params_[int(CbParam::Tempo)].store(140.0f);
    params_[int(CbParam::RunStop)].store(0.0f);
    params_[int(CbParam::AccentSquelch)].store(1.0f);
    // Oscillator extras default to transparent (same sound as original 303).
    params_[int(CbParam::PWM)].store(0.5f);
    params_[int(CbParam::SubOscLevel)].store(0.0f);
    params_[int(CbParam::DupletWaveform)].store(1.0f);  // square
    params_[int(CbParam::DupletPWM)].store(0.5f);
    params_[int(CbParam::DupletDetune)].store(0.0f);
    params_[int(CbParam::DupletLevel)].store(0.0f);
    params_[int(CbParam::PwmLfoRate)].store(0.0f);        // Hz; 0 = LFO off
    params_[int(CbParam::PwmLfoDepth)].store(0.0f);
    params_[int(CbParam::PwmLfoDupletDepth)].store(0.0f);
    params_[int(CbParam::NoiseLevel)].store(0.0f);
    params_[int(CbParam::ResoBoost)].store(0.0f);
    params_[int(CbParam::DistType)].store(0.0f);   // Classic = the stock clipper
    params_[int(CbParam::DistGain)].store(0.0f);
    params_[int(CbParam::DistTone)].store(0.5f);
    params_[int(CbParam::DistLevel)].store(1.0f);
    params_[int(CbParam::CompThreshold)].store(0.5f);
    params_[int(CbParam::CompDepth)].store(0.0f);   // 0 = compressor off
    params_[int(CbParam::RingHz)].store(100.0f);    // Hz, stored directly
    params_[int(CbParam::RingAmount)].store(0.0f);  // 0 = ring modulator off
    // Mirror into baseParam_ so sweep fallbacks start at the same defaults.
    for (int i = 0; i < int(CbParam::NumParams); ++i) {
        baseParam_[i].store(params_[i].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    }

    // Generate a random pattern on every startup so the synth makes noise
    // even before the user loads a .roq file. Nicely flavour at medium
    // wildness matches the 2002 Delphi "Chaos nicely" defaults.
    // Seed the live buffer directly; no writer needed before any thread exists.
    // The idle buffer is left at its default and is overwritten from live by
    // the first SongWriter, so the two never diverge.
    song_[0].patterns.assign(1, makeRandomPattern(RandomFlavor::Nicely, 0.5f));

    resetFilters();
}

// Random pattern helpers. Shared RNG seeded with a time-based default.
static std::mt19937& cbRng() {
    static std::mt19937 rng(static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    return rng;
}

namespace {

// Pitch pools for the three scale-restricted flavors. Chromatic has no
// explicit pool (it rolls 1..60 directly).
static const int kPoolNicely[] = {
    13, 16, 18, 20, 23,  // C1, Eb1, F1, G1, Bb1
    25, 28, 30, 32, 35,  // C2, Eb2, F2, G2, Bb2
    37                   // C3
};
// 2002 Delphi "Chaos nicely" pool. Matches the switch in
// method_Chaospatternnicely1Click.c: (rand+1)*15 + offset with offset
// values {1, 4, 5, 6, 8, 11}. Case 0 is repeated 5× in the switch table
// so Eb appears more often — preserved here by listing Eb three times at
// the front of the pool.
static const int kPoolRetro[] = {
    16, 28, 40,          // Eb1, Eb2, Eb3
    16, 28, 40,          // Eb repeated (matches Delphi case-0 weight)
    19, 31, 43,          // F#1, F#2, F#3
    20, 32, 44,          // G1, G2, G3
    21, 33, 45,          // G#1, G#2, G#3
    23, 35, 47,          // Bb1, Bb2, Bb3
    14, 26, 38           // C#1, C#2, C#3
};
static const int kPoolVST[] = {
    13, 25, 37,          // C1, C2, C3
    15, 27, 39,          // D1, D2, D3
    17, 29, 41,          // E1, E2, E3
    18, 30, 42,          // F1, F2, F3
    20, 32, 44,          // G1, G2, G3
    24, 36, 48           // B1, B2, B3
};

struct DensityRoll {
    float restP, slideP, accentP;
};

// Wildness (0..1) → per-step probabilities. Non-linear so wildness=0.5
// lands close to the 2002 Delphi's observed ~10% rest, 25% slide/accent.
DensityRoll densitiesFor(float wildness) {
    if (wildness < 0.0f) wildness = 0.0f;
    if (wildness > 1.0f) wildness = 1.0f;
    DensityRoll d;
    const float inv = 1.0f - wildness;
    d.restP   = 0.60f * inv * inv;
    d.slideP  = 0.05f + 0.55f * std::pow(wildness, 1.5f);
    d.accentP = 0.15f + 0.40f * wildness;
    return d;
}

const int* poolForFlavor(CBEngine::RandomFlavor f, int& outCount) {
    using RF = CBEngine::RandomFlavor;
    switch (f) {
        case RF::Nicely: outCount = (int)(sizeof(kPoolNicely)/sizeof(int)); return kPoolNicely;
        case RF::Retro:  outCount = (int)(sizeof(kPoolRetro) /sizeof(int)); return kPoolRetro;
        case RF::VST:    outCount = (int)(sizeof(kPoolVST)   /sizeof(int)); return kPoolVST;
        case RF::Chromatic:
        default:         outCount = 0;                                     return nullptr;
    }
}

} // namespace

CbStep CBEngine::makeRandomStep(RandomFlavor flavor, float wildness) {
    auto& rng = cbRng();
    const DensityRoll d = densitiesFor(wildness);
    std::uniform_real_distribution<float> r(0.0f, 1.0f);
    CbStep s;
    if (r(rng) < d.restP) {
        s.pitch = 0;
    } else if (flavor == RandomFlavor::Chromatic) {
        std::uniform_int_distribution<int> pick(1, 60);
        s.pitch = pick(rng);
    } else {
        int count = 0;
        const int* pool = poolForFlavor(flavor, count);
        std::uniform_int_distribution<int> pick(0, count - 1);
        s.pitch = pool[pick(rng)];
    }
    s.slide  = r(rng) < d.slideP;
    s.accent = r(rng) < d.accentP;
    return s;
}

CbPattern CBEngine::makeRandomPattern(RandomFlavor flavor, float wildness, int lastStep) {
    CbPattern p;
    if (lastStep < 0)  lastStep = 0;
    if (lastStep > 63) lastStep = 63;
    p.lastStep = lastStep;
    // Only the live steps. The rest keep CbStep's defaults, so a shortened
    // pattern has rests waiting behind its end rather than stale random notes.
    for (int i = 0; i <= lastStep; ++i) {
        p.steps[i] = makeRandomStep(flavor, wildness);
    }
    return p;
}

void CBEngine::setPlayMode(PlayMode m) {
    playMode_.store(m);
}

void CBEngine::setSampleRate(double sr) {
    sampleRate_ = sr > 0.0 ? sr : 48000.0;
}

void CBEngine::setParam(CbParam p, float v) {
    baseParam_[int(p)].store(v, std::memory_order_relaxed);
    params_[int(p)].store(v, std::memory_order_relaxed);
    if (p == CbParam::RunStop)   running_.store(v > 0.5f, std::memory_order_relaxed);
    // boxSndType_ is deliberately NOT set here. It used to be, from whichever
    // thread moved the Waveform control - a write to non-atomic DSP state from
    // outside the audio thread, exactly the class of race finding I was about.
    // It was also dead: process() derives boxSndType_ from the Waveform atomic
    // at the top of every block, before anything reads it, so removing the
    // write changes nothing except who is allowed to touch the field.
}

// ---------------------------------------------------------------------------
// Song double-buffering. See the protocol note in CBEngine.h.
// ---------------------------------------------------------------------------
CBEngine::SongWriter::SongWriter(CBEngine& e)
    : e_(e), lk_(e.writerMu_), next_(1 - e.liveIdx_.load(std::memory_order_acquire)) {
    // Wait for the audio thread to stop reading the buffer we are about to
    // overwrite. Bounded by one audio block (~11 ms at 512 frames / 44.1 kHz),
    // and this is the UI thread, which is allowed to wait. The audio thread
    // never waits for us.
    while (e_.readerIdx_.load(std::memory_order_acquire) == next_)
        std::this_thread::yield();
    e_.song_[next_] = e_.song_[e_.liveIdx_.load(std::memory_order_relaxed)];
}

void CBEngine::SongWriter::commit() {
    if (e_.song_[next_].patterns.empty())        // invariant the reader relies on
        e_.song_[next_].patterns.push_back(CbPattern{});
    e_.liveIdx_.store(next_, std::memory_order_release);
}

void CBEngine::setParamSweeps(int sliderIdx, const std::vector<ParamSweep>& sweeps) {
    if (sliderIdx < 0 || sliderIdx >= 7) return;
    SongWriter w(*this);
    w.data().paramSweeps[sliderIdx] = sweeps;
    w.commit();
}

float CBEngine::getParam(CbParam p) const {
    return params_[int(p)].load(std::memory_order_relaxed);
}

void CBEngine::setPattern(const CbPattern& p) {
    SongWriter w(*this);
    w.data().patterns.assign(1, p);
    w.commit();
    patternIdx_.store(0, std::memory_order_relaxed);
}
void CBEngine::setPatterns(const std::vector<CbPattern>& v) {
    SongWriter w(*this);
    if (v.empty()) w.data().patterns.assign(1, CbPattern{});
    else           w.data().patterns = v;
    w.commit();
    patternIdx_.store(0, std::memory_order_relaxed);
}
CbPattern CBEngine::getPattern() const {
    std::lock_guard<std::mutex> g(writerMu_);
    const auto& pats = liveSong().patterns;
    if (pats.empty()) return CbPattern{};
    int idx = std::clamp(patternIdx_.load(std::memory_order_relaxed), 0, (int)pats.size() - 1);
    return pats[idx];
}
CbPattern CBEngine::getPatternAt(int idx) const {
    std::lock_guard<std::mutex> g(writerMu_);
    const auto& pats = liveSong().patterns;
    if (idx < 0 || idx >= (int)pats.size()) return CbPattern{};
    return pats[idx];
}
void CBEngine::setPatternAt(int idx, const CbPattern& p) {
    SongWriter w(*this);
    if (idx < 0 || idx >= (int)w.data().patterns.size()) return;  // no commit: nothing published
    w.data().patterns[idx] = p;
    w.commit();
}
void CBEngine::selectPattern(int idx) {
    std::lock_guard<std::mutex> g(writerMu_);
    const int n = (int)liveSong().patterns.size();
    if (n <= 0) return;
    patternIdx_.store(std::clamp(idx, 0, n - 1), std::memory_order_relaxed);
}
int CBEngine::insertPatternAfterCurrent() {
    SongWriter w(*this);
    auto& pats = w.data().patterns;
    int at = patternIdx_.load(std::memory_order_relaxed) + 1;
    if (at < 0) at = 0;
    if (at > (int)pats.size()) at = (int)pats.size();
    pats.insert(pats.begin() + at, CbPattern{});
    w.commit();
    patternIdx_.store(at, std::memory_order_relaxed);
    return at;
}
bool CBEngine::deleteCurrentPattern() {
    SongWriter w(*this);
    auto& pats = w.data().patterns;
    if (pats.size() <= 1) return false;                           // no commit
    const int at = std::clamp(patternIdx_.load(std::memory_order_relaxed), 0, (int)pats.size() - 1);
    pats.erase(pats.begin() + at);
    const int newSize = (int)pats.size();
    w.commit();
    if (patternIdx_.load(std::memory_order_relaxed) >= newSize)
        patternIdx_.store(newSize - 1, std::memory_order_relaxed);
    return true;
}
int CBEngine::currentPatternIndex() const {
    return patternIdx_.load(std::memory_order_relaxed);
}
int CBEngine::patternCount() const {
    std::lock_guard<std::mutex> g(writerMu_);
    return static_cast<int>(liveSong().patterns.size());
}
void CBEngine::setSweeps(const Sweep sweeps[7], bool enabled) {
    SongWriter w(*this);
    for (int i = 0; i < 7; ++i) w.data().sweeps[i] = sweeps[i];
    w.data().sweepsEnabled = enabled;
    w.commit();
}
// Everything this function used to do to the DSP state now happens on the
// audio thread instead; see applyTransportReset() and the note on
// transportReset_ in the header. Only the two atomics are written here.
//
// A second request arriving before the first is applied simply replaces it,
// which is right: the audio thread should act on what the transport wants NOW,
// not replay a queue of button presses.
void CBEngine::setRunning(bool run, bool restartFromFirstPattern) {
    const TransportReset r = run
        ? (restartFromFirstPattern ? TransportReset::StartFromFirst
                                   : TransportReset::StartFromCurrent)
        : TransportReset::Stop;
    transportReset_.store(int(r), std::memory_order_release);
    running_.store(run, std::memory_order_relaxed);
    params_[int(CbParam::RunStop)].store(run ? 1.0f : 0.0f);
}

// Audio thread only. Same work setRunning() used to do inline, in the same
// order, so a render started immediately after a transport change is unchanged.
void CBEngine::applyTransportReset(TransportReset r) {
    if (r == TransportReset::Stop) {
        // On stop: silence the oscillator and clear any pending slide so
        // a glide in progress doesn't keep multiplying boxOscFrq into
        // sub-audio or supersonic territory.
        resetFilters();
        boxOscFrq_ = 0.0;
        boxSlide_ = 1.0;
        currentStep_ = -1;
        displayStep_.store(-1, std::memory_order_relaxed);
        return;
    }
    currentStep_ = -1;
    displayStep_.store(-1, std::memory_order_relaxed);
    samplesIntoStep_ = 0;
    if (r == TransportReset::StartFromFirst)
        patternIdx_.store(0, std::memory_order_relaxed);
    resetFilters();
}
bool CBEngine::isRunning() const {
    return running_.load(std::memory_order_relaxed);
}

double CBEngine::noteFreq(int pitch) const {
    if (pitch <= 0 || pitch > 60) return 0.0;
    return cNoteFreq[pitch];
}

void CBEngine::resetFilters() {
    distState_.reset();
    // NEITHER compState_ NOR ringPhase_ belongs here, for two different reasons.
    //
    // ringPhase_ is re-zeroed per NOTE, in triggerStep(). Adding it here as
    // well would be harmless but would misrepresent where it is owned.
    //
    // compState_ must not be reset per pattern at all. resetFilters() runs at
    // every pattern start, so zeroing the envelope follower here would put a
    // burst of make-up gain on the first few milliseconds of beat 1 of every
    // bar - an artifact manufactured once per loop to satisfy a criterion the
    // follower already meets. It meets it because a one-pole is CONTRACTING:
    // the difference between two runs with different initial states shrinks by
    // the smoothing coefficient every sample, so it is gone long before a loop
    // is over. A phase accumulator never forgets, which is why the ring carrier
    // needs an explicit reset and this does not. The loop-identity test in the
    // harness measures exactly this.
    IIR_resetIIR(lp2_);
    IIR_resetFIR(lp3FIR_);
    IIR_resetIIR(hp1_);
    dist_initdist();
    boxOsc_ = 0.0;
    dupletOsc_ = 0.0;
    subOscPhase_ = 0.0;
    boxMEG_ = 0.0;
    boxVolEnv_ = 1.0;
    boxAccentEnv_ = 0.0;
    boxAccentFrq_ = 0.0;
    tempAccentEnv_ = 0.0;
    tempAccentFrq_ = 0.0;
    boxDecay_ = 2.5;
    boxGate_ = 0.0;
    gateStat_ = 1;
    bNoteSlide_ = false;
    bSlideNext_ = false;
    boxSlide_ = 1.0;
}

// How many samples a note starting at `step` will actually *sound* for: its
// own step, plus every step tied to it by a slide, less the tail of the final
// step after the gate closes. Only "reverse step" needs this - a backwards
// envelope has to know where its peak must land, and the peak has to land when
// the note stops being audible, not when the step ends. Aiming at the step
// boundary instead would put the brightest moment after the gate had already
// shut, which is silent and therefore pointless.
static int audibleNoteLength(const CbPattern& pat, int step, int samplesPerStep) {
    const int n = pat.lastStep + 1;
    int tied = 1;
    int i = step;
    // Same condition triggerStep() uses for bSlideNext_, walked forward. The
    // guard is not paranoia: a pattern with a slide on every step would
    // otherwise never terminate.
    for (int guard = 0; guard < n; ++guard) {
        const int next = (i < pat.lastStep) ? i + 1 : 0;
        if (!(pat.steps[i].slide && pat.steps[i].pitch != 0 && pat.steps[next].pitch != 0))
            break;
        i = next;
        ++tied;
    }
    // triggerStep() re-latches boxAccent_ on every tied step, so the gate
    // threshold that actually ends the note belongs to the *last* step of the
    // tie, not the first. Accented steps hold the gate open ~25% longer.
    const double thresh = ((double)samplesPerStep * (1.0 / 1.9))
                        * (1.0 - (pat.steps[i].accent ? 0.25 : 0.0));
    const double len = (double)tied * (double)samplesPerStep - thresh;
    return (len > 1.0) ? (int)len : 1;
}

void CBEngine::triggerStep(const CbStep& currStep, const CbStep& nextStep) {
    // From src/SilverBox.cpp CBProcess() "START A NEW NOTE" branch, ~line 2706.
    if (!bNoteSlide_) {
        boxOscFrq_ = noteFreq(currStep.pitch);
        boxOsc_ = 0.0;
        boxDecay_ = 4.0;
        boxMEG_ = 0.0;
        boxVolEnv_ = 1.0;
        gateStat_ = (boxOscFrq_ == 0.0) ? 1 : 0;  // 0 = open, 1 = shut (rest)
        // Latched only on a genuine new note, unlike boxAccent_ below. A
        // reversed step is one backwards envelope stretched across the whole
        // note, so a tied note takes its direction from the step that started
        // it; re-latching per tied step would reverse direction mid-note with
        // no fresh envelope to reverse. This is why it sits inside the
        // !bNoteSlide_ branch and boxAccent_ does not.
        stepReverse_ = currStep.reverseStep;
        // Ring carrier restarts with every new note. Note the contrast with
        // pwmLfoPhase_, which must NOT be reset: that one is a slow modulator
        // whose whole point is drifting across the bar, and it is at a rate
        // where drift is audible AS drift. The ring carrier is at audio rate,
        // where a drifting phase is not heard as movement at all - only as an
        // attack that lands differently every time, and as a one-pattern loop
        // that quietly stops repeating identically, which this block's whole
        // admission criterion forbids. Tied notes keep the phase they started
        // with, same reasoning as stepReverse_ above.
        ringPhase_ = 0.0;
    }

    boxAccent_ = currStep.accent;
    if (boxAccent_) {
        boxAccentEnv_ = 0.6;
        // Classic TB-303 accent buildup scaled by the squelch knob. At sq=1
        // this reduces to Frank's original `tempAccentFrq_ + 2.0`. Scaling
        // the whole expression (rather than only the residual term) makes the
        // knob audibly dramatic: sq=0 zeros the filter-envelope boost from
        // the accent (volume still kicks, but no wah), sq=2 doubles it.
        const float sq = params_[int(CbParam::AccentSquelch)].load(std::memory_order_relaxed);
        boxAccentFrq_ = (tempAccentFrq_ + 2.0) * sq;
    }

    // Slide flag only takes effect if both current and next pitch are non-zero.
    bSlideNext_ = currStep.slide && currStep.pitch != 0 && nextStep.pitch != 0;
}

// Samples per sequencer step. The straight branch is written exactly as it
// always was rather than as a multiply by 1.0, so the off path is bit-identical
// by construction instead of by rounding luck - and a second expression beats a
// factor inside a verified one.
//
// Nothing else in the engine knows what a step is: the gate threshold,
// audibleNoteLength(), the reversed envelope and the slide rate all derive from
// this number, so they follow a triplet pattern automatically.
static int stepSamples(double sampleRate, double tempo, bool triplet) {
    const int n = triplet
        ? static_cast<int>((sampleRate * 60.0) / (tempo * 3.0))   // eighth triplet
        : static_cast<int>((sampleRate * 60.0) / (tempo * 4.0));  // sixteenth
    return (n < 1) ? 1 : n;
}

void CBEngine::process(float* out, int frames) {
    // Before anything reads the sequencer position: the transport may have been
    // pressed since the last block, and this is the thread that owns the state
    // it changes. exchange() so a request is applied exactly once, and so a
    // press arriving during this block is kept for the next one rather than
    // lost. No lock and no allocation, so invariant 3 still holds.
    const int pendingReset = transportReset_.exchange(int(TransportReset::None),
                                                      std::memory_order_acquire);
    if (pendingReset != int(TransportReset::None))
        applyTransportReset(TransportReset(pendingReset));

    const float fVolume   = params_[int(CbParam::Volume)].load(std::memory_order_relaxed);
    const float fTempo    = params_[int(CbParam::Tempo)].load(std::memory_order_relaxed);
    const float fWaveform = params_[int(CbParam::Waveform)].load(std::memory_order_relaxed);
    const float fTuning   = params_[int(CbParam::Tuning)].load(std::memory_order_relaxed);
    const float fCutoff   = params_[int(CbParam::Cutoff)].load(std::memory_order_relaxed);
    const float fRezo     = params_[int(CbParam::Resonance)].load(std::memory_order_relaxed);
    const float fEnvMod   = params_[int(CbParam::EnvMod)].load(std::memory_order_relaxed);
    const float fDecay    = params_[int(CbParam::Decay)].load(std::memory_order_relaxed);
    const float fAccent   = params_[int(CbParam::Accent)].load(std::memory_order_relaxed);
    const float fPWM        = params_[int(CbParam::PWM)].load(std::memory_order_relaxed);
    const float fSubLevel   = params_[int(CbParam::SubOscLevel)].load(std::memory_order_relaxed);
    const float fDupletWv   = params_[int(CbParam::DupletWaveform)].load(std::memory_order_relaxed);
    const float fDupletPWM  = params_[int(CbParam::DupletPWM)].load(std::memory_order_relaxed);
    const float fDupletDet  = params_[int(CbParam::DupletDetune)].load(std::memory_order_relaxed);
    const float fDupletLvl  = params_[int(CbParam::DupletLevel)].load(std::memory_order_relaxed);
    const float fLfoRate    = params_[int(CbParam::PwmLfoRate)].load(std::memory_order_relaxed);
    const float fLfoDepth   = params_[int(CbParam::PwmLfoDepth)].load(std::memory_order_relaxed);
    const float fLfoDupDep  = params_[int(CbParam::PwmLfoDupletDepth)].load(std::memory_order_relaxed);
    const float fNoiseLvl   = params_[int(CbParam::NoiseLevel)].load(std::memory_order_relaxed);
    const bool  bResoBoost  = params_[int(CbParam::ResoBoost)].load(std::memory_order_relaxed) > 0.5f;
    const int   iDistType   = (int)(params_[int(CbParam::DistType)].load(std::memory_order_relaxed) + 0.5f);
    // Coefficients are derived once per block, never per sample — same tactic
    // as the gate and envelope constants below.
    CbDistCoeffs distC{};
    if (iDistType != 0)
        cbDistPrepare(distC, iDistType,
                      params_[int(CbParam::DistGain)].load(std::memory_order_relaxed),
                      params_[int(CbParam::DistTone)].load(std::memory_order_relaxed),
                      params_[int(CbParam::DistLevel)].load(std::memory_order_relaxed),
                      sampleRate_);
    // Same tactic again: derived once per block, and only when the effect is
    // actually on, so the off path costs a single comparison.
    CbCompCoeffs compC{};
    CbRingCoeffs ringC{};
    const bool bComp = params_[int(CbParam::CompDepth)].load(std::memory_order_relaxed) > 0.0f;
    const bool bRing = params_[int(CbParam::RingAmount)].load(std::memory_order_relaxed) > 0.0f;
    if (bComp)
        cbCompPrepare(compC,
                      params_[int(CbParam::CompThreshold)].load(std::memory_order_relaxed),
                      params_[int(CbParam::CompDepth)].load(std::memory_order_relaxed),
                      sampleRate_);
    if (bRing)
        cbRingPrepare(ringC,
                      params_[int(CbParam::RingHz)].load(std::memory_order_relaxed),
                      params_[int(CbParam::RingAmount)].load(std::memory_order_relaxed),
                      sampleRate_);
    const int   dupletSndType = fDupletWv > 0.5f ? 1 : 0;
    // Detune factor: -1..+1 = ±1 octave → frequency factor 2^detune.
    // 0 = unison, +1 = octave up, -1 = octave down, arbitrary fractions in
    // between. Shared between the duplet osc and sub sine, so the sub
    // follows the duplet's detune too (but still one octave below that).
    const double dupletFactor = std::pow(2.0, fDupletDet);
    // PWM thresholds for square, one per oscillator. Widened to ~1..99 %
    // duty at the extremes so the pulse narrows into near-silent spikes.
    //
    // These stay hoisted out of the sample loop and are used verbatim while
    // the PWM LFO is off, which keeps the stock-303 path bit-identical. When
    // the LFO runs the thresholds are recomputed per sample instead (see the
    // loop below) — a block is longer than one cycle at the top of the rate
    // range, so per-block modulation would alias.
    const double pwmThresh       = pi + (fPWM       - 0.5) * pi * 1.98;
    const double dupletPwmThresh = pi + (fDupletPWM - 0.5) * pi * 1.98;
    const double pwmLfoInc       = (fLfoRate > 0.0f) ? (double)fLfoRate / sampleRate_ : 0.0;
    boxSndType_ = fWaveform > 0.5f ? 1 : 0;

    // Volume scaling. Slider=100 lands at ~0.14 pass-through — chosen so
    // even accented peaks stay below the dist() clipper at default settings,
    // giving the user a clean bed to add outboard distortion later. Saw
    // waveform runs slightly hotter than square, compensated with *1.5.
    double knobVolume = fVolume * 0.14;
    if (boxSndType_ == 0) knobVolume *= 1.5;
    double knobAccent = fAccent * 0.65;
    double knobDecay  = fDecay;
    // Hoisted out of the sample loop; it was computed inline per sample before,
    // from the same block-constant knobDecay, so the value is unchanged. The
    // reciprocal is what runs the envelope backwards for a reversed step:
    // 4*r^(N-t) == (4*r^N) * (1/r)^t, so a time-reversed decay costs one
    // different starting value and one different multiplier, nothing else.
    const double decayMul    = knobDecay * 0.0005 + 0.9995;
    const double decayMulInv = (decayMul > 0.0) ? (1.0 / decayMul) : 1.0;
    // Tuning is bipolar in the rebuild: fTuning already comes in as -1..+1
    // with 0 = neutral (VST used 0..1 with 0.5 as centre; slider widget
    // now provides the bipolar range directly).
    double knobTune   = fTuning;
    double knobEnvMod = fEnvMod;
    double knobCutoff = (std::pow(2.0, fCutoff * 0.5) - 1.0);
    knobCutoff = (std::pow(2.0, knobCutoff * 0.8) - 0.987);
    double pre_knobRezo = (std::sin(fRezo * halfpi) * 75.0 + 25.0) / 100.0;
    if (bResoBoost) {
        // Extend the ceiling rather than lifting the whole range: a quartic
        // term is negligible through the usable part of the travel and only
        // bites at the end, so the knob keeps its familiar feel and only the
        // top ~10 % is new. Full travel lands near 1.06 at the filter, which
        // is past the stability point - hence the _boost filter variant with
        // its bounded resonant integrator.
        pre_knobRezo += 0.07 * (double)fRezo * (double)fRezo * (double)fRezo * (double)fRezo;
    }

    const bool running = running_.load(std::memory_order_relaxed);

    // Acquire the live song WITHOUT locking or copying. Publish the index we
    // intend to read, then re-read liveIdx_ and retry if a writer flipped in
    // between; that ordering is what stops a writer from choosing this very
    // buffer as its next target. See the protocol note in CBEngine.h.
    int songIdx;
    do {
        songIdx = liveIdx_.load(std::memory_order_acquire);
        readerIdx_.store(songIdx, std::memory_order_seq_cst);
    } while (songIdx != liveIdx_.load(std::memory_order_acquire));

    // Clears readerIdx_ on every exit path, so a writer is never left waiting
    // on a block that has already finished.
    struct ReaderGuard {
        std::atomic<int>& slot;
        ~ReaderGuard() { slot.store(-1, std::memory_order_release); }
    } readerGuard{readerIdx_};

    const SongData& song            = song_[songIdx];
    const std::vector<CbPattern>& pats = song.patterns;
    const Sweep* sweeps                = song.sweeps;
    const std::vector<ParamSweep>* paramSweeps = song.paramSweeps;
    const bool sweepsOn                = song.sweepsEnabled;

    // Writers guarantee patterns is never empty; this is belt-and-braces for
    // the real-time path, and deliberately allocation-free if it ever fires.
    static const CbPattern kEmptyPattern{};
    int patIdx = patternIdx_.load(std::memory_order_relaxed);
    if (patIdx < 0 || patIdx >= (int)pats.size()) patIdx = 0;
    // A pointer into the live buffer, not a per-block copy of the pattern.
    const CbPattern* pat = pats.empty() ? &kEmptyPattern : &pats[patIdx];

    // Samples per 16th note
    samplesPerStep_ = stepSamples(sampleRate_, fTempo, pat->triplet);

    // Envelope constants (from SilverBox.cpp wwelti branch)
    double boxMEG_c0, boxMEG_c1;
    if (boxAccent_) { boxMEG_c0 = -1.75;  boxMEG_c1 = 0.00009; }
    else            { boxMEG_c0 = -1.0;   boxMEG_c1 = 0.0000082; }

    double two_power_knobTune = std::pow(2.0, knobTune);

    double gate_c0, gate_c1;
    if (gateStat_ == 0) { gate_c0 = 1.0; gate_c1 = 0.9; }
    else                { gate_c0 = 0.0; gate_c1 = 0.004; }

    for (int i = 0; i < frames; ++i) {
        // --- sequencer tick ---
        if (running) {
            if (currentStep_ < 0 || samplesIntoStep_ >= samplesPerStep_) {
                samplesIntoStep_ = 0;
                currentStep_ = (currentStep_ + 1);
                if (currentStep_ > pat->lastStep) {
                    // In FullSequence mode advance through all patterns in
                    // order, looping at the end. In CurrentPatternOnly mode
                    // stay on the initial pattern forever.
                    if (playMode_.load() == PlayMode::FullSequence && !pats.empty()) {
                        patIdx = (patIdx + 1) % (int)pats.size();
                        pat = &pats[patIdx];
                        // Plain atomic store — this used to take patternMu_,
                        // the second lock the audio thread held per block.
                        patternIdx_.store(patIdx, std::memory_order_relaxed);
                        // Step duration is a property of the PATTERN now, and
                        // the pattern changes here, inside the sample loop -
                        // while samplesPerStep_ is otherwise computed once per
                        // block. Leaving it stale until the next block boundary
                        // would run a triplet pattern following a straight one
                        // at the wrong rate for up to a whole buffer: a flam on
                        // the first note, once per loop, that moves with the
                        // audio buffer size. Recompute it here.
                        samplesPerStep_ = stepSamples(sampleRate_, fTempo, pat->triplet);
                    }
                    currentStep_ = 0;
                }
                int nextStep = (currentStep_ < pat->lastStep) ? currentStep_ + 1 : 0;
                // Publish for the UI. Once per step, not per sample.
                displayStep_.store(currentStep_, std::memory_order_relaxed);
                if (currentStep_ == 0) resetFilters();
                triggerStep(pat->steps[currentStep_], pat->steps[nextStep]);

                // Reverse step: run the filter envelope backwards by starting
                // boxDecay_ where the forward envelope would have *ended* and
                // growing it back to 4.0 over the note. Not a sign flip - an
                // inverted decay is fastest at the note's start, where a
                // genuinely reversed note is slow first and peaks at the end.
                // Seeded here rather than in triggerStep() because the tie
                // walk needs the pattern, and skipped on a slide continuation
                // because the head of the tie already sized the envelope for
                // the whole note.
                if (stepReverse_ && !bNoteSlide_ && gateStat_ == 0) {
                    const int len = audibleNoteLength(*pat, currentStep_, samplesPerStep_);
                    double start = 4.0 * std::pow(decayMul, (double)len);
                    // Degenerate corner - very slow tempo, fastest decay, a long
                    // tie - can underflow r^N to zero, and zero times anything
                    // stays zero, which would strand the envelope at silence.
                    // Flooring makes it peak early and hold instead, which is
                    // the graceful failure rather than the silent one.
                    if (!(start > 1e-30)) start = 1e-30;
                    boxDecay_ = start;
                }

                // Interactive sweeps from the sweep-editor UI. For each slider
                // find the first sweep whose [start, end] song-position range
                // covers the current step; if found, override params_ with the
                // interpolated value. Otherwise restore params_ from baseParam_
                // so the slider's own setting applies.
                {
                    // ORDER IS UI SLIDER ORDER, and it deliberately differs from
                    // the legacy .cb table further down (search kIds). Do NOT
                    // "reconcile" the two — see the note at that second table
                    // for the evidence. This one is indexed by the main window's
                    // left-to-right slider order, documented in RoqIO.h, and it
                    // is what .roq sweeps and the sweep editor's sliderIdx mean.
                    static const CbParam kIds[7] = {
                        CbParam::Volume, CbParam::Cutoff, CbParam::Resonance, CbParam::EnvMod,
                        CbParam::Decay, CbParam::Accent, CbParam::Tuning
                    };
                    const int stepsPerPat = pat->lastStep + 1;
                    const float songPos = (float)patIdx + (float)currentStep_ / (float)stepsPerPat;
                    // A sweep endpoint has to be normalised by the length of the
                    // pattern *it names*, not by the length of the one playing.
                    // With every pattern 16 steps those were the same number and
                    // the distinction did not exist; once lengths vary, dividing
                    // "pattern 0 step 8" by a 4-step pattern's length yields 2.0
                    // and silently relocates the endpoint two patterns along.
                    // Clamping the step also keeps an endpoint inside its pattern
                    // after the user shortens one underneath an existing sweep.
                    const int patCount = (int)pats.size();
                    auto posOf = [&](int p, int st) {
                        const int n = (p >= 0 && p < patCount) ? pats[p].lastStep + 1 : 16;
                        if (st < 0)  st = 0;
                        if (st >= n) st = n - 1;
                        return (float)p + (float)st / (float)n;
                    };
                    for (int k = 0; k < 7; ++k) {
                        const auto& list = paramSweeps[k];
                        bool applied = false;
                        for (const auto& sw : list) {
                            const float start = posOf(sw.startPat, sw.startStep);
                            const float end   = posOf(sw.endPat,   sw.endStep);
                            if (songPos < start || songPos > end) continue;
                            float t = (end > start) ? (songPos - start) / (end - start) : 0.0f;
                            float raw = (float)sw.startVal + ((float)sw.endVal - (float)sw.startVal) * t;
                            // Raw matches the UI sliders (-100..+100 bipolar,
                            // 0..100 unipolar). Map to the param's native 0..1
                            // (unipolar) or -1..+1 (bipolar) range.
                            float v;
                            const CbParam id = kIds[k];
                            if (id == CbParam::Resonance || id == CbParam::EnvMod
                             || id == CbParam::Tuning) {
                                v = raw / 100.0f;  // bipolar
                            } else {
                                v = raw / 100.0f;  // unipolar (Volume/Cutoff/Decay/Accent)
                                if (v < 0.0f) v = 0.0f;
                            }
                            params_[int(id)].store(v, std::memory_order_relaxed);
                            applied = true;
                            break;
                        }
                        if (!applied) {
                            params_[int(kIds[k])].store(
                                baseParam_[int(kIds[k])].load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
                        }
                    }
                }

                // Apply per-pattern sweep: each tab has 2 breakpoints with
                // position = order + offset/100. Linearly interpolate; below
                // the first breakpoint hold at bp0's value; above bp1 hold at
                // bp1's value (or the UI knob if bp1.order == 9999 forever).
                // NOTE: `sweepsOn` is currently always false. The only caller of
                // setSweeps() is the .cb import path in MainWindow, which passes
                // enabled=false on purpose (see the rationale at that call site:
                // the per-tab value curves are not fully decoded, and applying
                // the raw values silences many real presets — reverse/cb_schema.md
                // under "not safe to apply by default"). So this block is loaded,
                // round-tripped and inert. It is kept because it is the decoded
                // 2002 behaviour and the starting point for turning legacy sweeps
                // back on; it is NOT dead code to be deleted casually.
                if (sweepsOn) {
                    // Song position in pattern units (e.g. 0.5 = halfway through pattern 0).
                    const float songPos = (float)patIdx
                                        + (float)currentStep_ / (float)(pat->lastStep + 1);
                    // ORDER IS 2002 .cb TAB ORDER, and it is deliberately NOT the
                    // UI slider order used by the interactive-sweep table above.
                    // Tuning is second here and last there. This was verified
                    // 2026-09-03 against three independent sources that agree:
                    //
                    //   1. reverse/parameter_map.md — the Delphi Form1.dfm
                    //      TPageControl tab declaration order: TabSheet1/volume,
                    //      tsTune, tsCutoff, tsRezo, tsEnvMod, tsDecay, tsAccent.
                    //   2. reverse/cb_schema.md — Ghidra of CREAKBOX.EXE: "the
                    //      seven TStringGrid tabs (Volume / Tune / Cutoff / Rezo /
                    //      EnvMod / Decay / Accent)".
                    //   3. The loader itself: CbPresetIO::loadCb reads the tail
                    //      grids sequentially into bp0[t]/bp1[t] with t = file
                    //      order and no remapping, and MainWindow copies bp*[i]
                    //      into sweeps[i] one-for-one. So index == file tab index
                    //      all the way from disk to here.
                    //
                    // Changing this table to match the other one would silently
                    // mis-map every sweep in all 56 shipped .cb modules.
                    static const CbParam kIds[7] = {
                        CbParam::Volume, CbParam::Tuning, CbParam::Cutoff, CbParam::Resonance,
                        CbParam::EnvMod, CbParam::Decay, CbParam::Accent
                    };
                    for (int k = 0; k < 7; ++k) {
                        const auto& sw = sweeps[k];
                        // Effective positions. 9999 means "forever" so clamp
                        // to a far-future value so the breakpoint never becomes
                        // the "next" target.
                        const float pos0 = (sw.order0 >= 9999) ? 1e9f : (sw.order0 + sw.off0 / 100.0f);
                        const float pos1 = (sw.order1 >= 9999) ? 1e9f : (sw.order1 + sw.off1 / 100.0f);
                        float raw;
                        if (songPos <= pos0)        raw = (float)sw.val0;
                        else if (songPos >= pos1)   raw = (float)sw.val1;
                        else if (pos1 > pos0) {
                            float t = (songPos - pos0) / (pos1 - pos0);
                            raw = sw.val0 + (sw.val1 - sw.val0) * t;
                        } else                      raw = (float)sw.val0;
                        // Raw value maps linearly: v = raw / 100. The 2002
                        // UI accepted any signed integer so negatives on
                        // Rezo / Cutoff / EnvMod / Tune reach the engine as
                        // negatives, producing Frank's "inverted" filter
                        // behaviour. Only Volume is floored at 0 to avoid
                        // phase cancellation at the mix bus.
                        float v = raw / 100.0f;
                        if (kIds[k] == CbParam::Volume && v < 0.0f) v = 0.0f;
                        params_[int(kIds[k])].store(v, std::memory_order_relaxed);
                    }
                }

                // Refresh gate constants after triggerStep() updated gateStat_.
                if (gateStat_ == 0) { gate_c0 = 1.0; gate_c1 = 0.9; }
                else                { gate_c0 = 0.0; gate_c1 = 0.004; }
                if (boxAccent_) { boxMEG_c0 = -1.75;  boxMEG_c1 = 0.00009; }
                else            { boxMEG_c0 = -1.0;   boxMEG_c1 = 0.0000082; }
            }
        }

        // --- DSP per sample (from wwelti loop) ---
        boxMEG_ += (boxMEG_c0 - boxMEG_) * boxMEG_c1;
        boxGate_ += (gate_c0 - boxGate_) * gate_c1;
        boxVolEnv_ += (0.0 - boxVolEnv_) * 0.00001;

        if (boxAccentFrq_ > 0.0) boxAccentFrq_ -= (2.0 / 4500.0);
        else                     boxAccentFrq_ = 0.0;
        if (boxAccentEnv_ > 0.0) boxAccentEnv_ -= 0.00015;

        // LPF smoothing: tempAccent* follows boxAccent* slowly. Critical for
        // two reasons — (a) it's what actually carries residual between
        // accents so successive accents stack audibly, and (b) without it
        // the filter cutoff would jump on every trigger and click.
        tempAccentFrq_ += (boxAccentFrq_ - tempAccentFrq_) * 0.00144;
        tempAccentEnv_ += (boxAccentEnv_ - tempAccentEnv_) * 0.03;
        if (stepReverse_) {
            boxDecay_ *= decayMulInv;
            // 4.0 is where a forward envelope begins, so it is the ceiling a
            // backwards one may reach. The clamp matters when the tie walk and
            // the actual note length disagree - a slide across a pattern
            // boundary, say - and turns an exponential runaway into a hold.
            if (boxDecay_ > 4.0) boxDecay_ = 4.0;
        } else {
            boxDecay_ *= decayMul;
        }

        double boxCutoff = knobCutoff + (boxOscFrq_ / sampleRate_) * 0.33;
        // The two EnvMod terms collapse to knobEnvMod * (boxDecay_ - 2.0).
        // boxDecay_ runs 4.0 -> 0 across a normal note, so that factor sweeps
        // +2 -> -2 and the filter sweeps *through* the base cutoff rather than
        // down to it. "Reverse step" does not appear here at all any more: it
        // reverses boxDecay_'s direction of travel above, which is what makes
        // it compose with sweeps on EnvMod - the parameter is never touched.
        double tempcutoff = boxCutoff * getTab2Interpolated(
            boxMEG_ + (knobEnvMod * -2.0) + (boxDecay_ * knobEnvMod) + tempAccentFrq_ * knobAccent);

        // Oscillator phase. Frank's VST hardcoded 176400 as the denominator
        // (88200 * 2). At 44.1 kHz runtime, his code produces output at
        // freq/4 of cNoteFreq (so cNoteFreq[1]=130.813 plays at 32.7 Hz,
        // standard C1 bass register). To preserve that behaviour at any
        // sample rate, scale the denominator to sampleRate * 4.
        const double phaseInc = twopi * (1.0 / (sampleRate_ * 4.0)) * boxOscFrq_ * two_power_knobTune;
        boxOsc_ += phaseInc;
        if (boxOsc_ > twopi) boxOsc_ -= twopi;
        // Duplet runs at main frequency × detune factor.
        dupletOsc_ += phaseInc * dupletFactor;
        if (dupletOsc_ > twopi) dupletOsc_ -= twopi;
        // Sub-oscillator runs one octave below the main pitch.
        subOscPhase_ += phaseInc * 0.5;
        if (subOscPhase_ > twopi) subOscPhase_ -= twopi;

        // Close gate near end of step unless sliding.
        if (!bSlideNext_ && running) {
            double remaining = samplesPerStep_ - samplesIntoStep_;
            double threshold = (samplesPerStep_ * (1.0 / 1.9)) * (1.0 - (boxAccent_ ? 0.25 : 0.0));
            if (remaining < threshold && gateStat_ == 0) {
                gateStat_ = 1;
                gate_c0 = 0.0; gate_c1 = 0.004;
            }
        }

        // --- shared PWM LFO: one triangle, one rate, two depths ---
        // Free-running: the phase is never reset, so repeated notes and
        // repeated patterns catch the LFO at different points. Both duty
        // thresholds fall back to the hoisted block constants when the LFO
        // is off, so rate == 0 costs one predictable branch and nothing else.
        double thrMain   = pwmThresh;
        double thrDuplet = dupletPwmThresh;
        if (pwmLfoInc > 0.0) {
            pwmLfoPhase_ += pwmLfoInc;
            if (pwmLfoPhase_ >= 1.0) pwmLfoPhase_ -= 1.0;
            // Triangle, bipolar, starting at centre and rising: 0 → +1 → -1 → 0.
            const double p = pwmLfoPhase_;
            const double tri = (p < 0.25) ? 4.0 * p
                             : (p < 0.75) ? 2.0 - 4.0 * p
                                          : 4.0 * p - 4.0;
            // Depth 1.0 against the centre PWM of 0.5 sweeps the full duty
            // range. Clamping (not wrapping) flattens at the rails, which is
            // the wanted behaviour — a wrap would snap the pulse inside out.
            if (fLfoDepth > 0.0f) {
                const double m = std::clamp((double)fPWM + (double)fLfoDepth * tri, 0.0, 1.0);
                thrMain = pi + (m - 0.5) * pi * 1.98;
            }
            if (fLfoDupDep > 0.0f && fDupletLvl > 0.0f) {
                const double m = std::clamp((double)fDupletPWM + (double)fLfoDupDep * tri, 0.0, 1.0);
                thrDuplet = pi + (m - 0.5) * pi * 1.98;
            }
        }

        double snd = 0.0;
        if (boxSndType_ == 0) {
            // Saw (PWM has no effect on saw; left unchanged — so the LFO is
            // inaudible here no matter the depth. The dialog says so.)
            snd = (twopi - boxOsc_) * (1.0 / twopi);
        } else {
            // Square with variable duty cycle. PWM slider shifts the
            // comparison threshold; at 0.5 (default) pwmThresh == pi → stock.
            snd = (boxOsc_ > thrMain) ? 0.5 : -0.5;
        }
        // Duplet voice — mixed in with its own level. Off at 0.
        if (fDupletLvl > 0.0f) {
            double d;
            if (dupletSndType == 0) d = (twopi - dupletOsc_) * (1.0 / twopi);
            else                    d = (dupletOsc_ > thrDuplet) ? 0.5 : -0.5;
            snd += d * (double)fDupletLvl;
        }
        // Sine sub-oscillator — thickens the bass an octave below.
        if (fSubLevel > 0.0f) {
            snd += std::sin(subOscPhase_) * (double)fSubLevel * 0.5;
        }
        // White noise, mixed in as just another oscillator: it lands before
        // the rest check and before the filter, so it is shaped by the cutoff
        // sweep and silenced on rests exactly like the tuned sources. Scaled
        // to +/-0.5 to match the square's amplitude. Skipped entirely at
        // level 0 so the generator state never advances and that path stays
        // bit-identical to the pre-noise engine.
        if (fNoiseLvl > 0.0f) {
            noiseState_ ^= noiseState_ << 13;
            noiseState_ ^= noiseState_ >> 17;
            noiseState_ ^= noiseState_ << 5;
            const double n = (double)(int32_t)noiseState_ * (1.0 / 4294967296.0);
            snd += n * (double)fNoiseLvl;
        }
        if (boxOscFrq_ == 0.0) snd = 0.0;

        if (boxSndType_ == 0) {
            snd = (double)IIR_FIRlp(lp3FIR_, 0.7, snd);
        } else {
            if (snd < 0.0) snd = (double)IIR_FIRlp(lp3FIR_, 0.7,   snd);
            else           snd = (double)IIR_FIRlp(lp3FIR_, 0.125, snd);
        }
        snd = (double)IIR_doIIRhigh(hp1_, 0.0068027, snd);

        double knobRezo = pre_knobRezo + (tempAccentEnv_ * 0.04) * knobAccent;
        if (boxSndType_ == 0) knobRezo *= 1.1;

        snd *= boxGate_;
        // Pass the cNoteFreq value raw to the IIR — Frank's internal
        // freq/44100 normalisation is based on the cNoteFreq table values,
        // not the actual output Hz, so no sample-rate scaling is needed
        // once the oscillator itself is SR-scaled correctly.
        // Two different functions rather than one with a flag: the stock
        // path stays byte-for-byte the code it has always been, so turning
        // the boost off cannot possibly change the sound.
        const long double rezoArg =
            (long double)(boxSndType_ == 0 ? knobRezo * 0.9 : knobRezo * 0.992);
        snd = bResoBoost
            ? (double)wwelti_IIR_IIRlp3pole_boost(lp2_, tempcutoff, rezoArg,
                                                  (long double)snd, (long double)boxOscFrq_)
            : (double)wwelti_IIR_IIRlp3pole(lp2_, tempcutoff, rezoArg,
                                            (long double)snd, (long double)boxOscFrq_);

        snd *= ((1.0 - knobRezo) * 1.5 + 1.0);
        snd *= boxVolEnv_ * (tempAccentEnv_ * 5.0 + 1.0) * knobVolume;

        // Type 0 keeps the exact 2004 call, untouched, so the default sound
        // is bit-identical to the engine before the pedal models existed.
        snd = (iDistType == 0) ? (double)dist_dist((long double)snd)
                               : cbDistProcess(distC, distState_, snd);

        // Compressor BEFORE the ring modulator on purpose: its follower would
        // otherwise chase the carrier's amplitude modulation and partly cancel
        // the ring, so the two amount controls would fight each other in a way
        // neither label explains. Both are skipped outright when off - not
        // multiplied by a neutral value - so no state advances and the off path
        // stays bit-identical to the engine before they existed.
        if (bComp) snd = cbCompProcess(compC, compState_, snd);
        if (bRing) snd = cbRingProcess(ringC, ringPhase_, snd);

        if (snd < -1.0) snd = -1.0;
        else if (snd > 1.0) snd = 1.0;

        // Render slide delta.
        boxOscFrq_ *= boxSlide_;

        if (running) ++samplesIntoStep_;

        // --- end-of-step handling: set up boxSlide_ for next step ---
        if (running && samplesIntoStep_ >= samplesPerStep_) {
            bNoteSlide_ = bSlideNext_;
            if (bNoteSlide_) {
                double eNext;
                if (currentStep_ < pat->lastStep) {
                    eNext = noteFreq(pat->steps[currentStep_ + 1].pitch);
                } else {
                    // slide into first step of next pattern in the order
                    int peekIdx = (patIdx + 1) % (int)pats.size();
                    eNext = noteFreq(pats[peekIdx].steps[0].pitch);
                }
                double tuned_curr = std::pow(2.0, std::log2(std::max(1e-6, boxOscFrq_)) + knobTune);
                double tuned_next = std::pow(2.0, std::log2(std::max(1e-6, eNext))     + knobTune);
                if (tuned_curr > 0.0)
                    boxSlide_ = std::exp(std::log(tuned_next / tuned_curr) / samplesPerStep_);
                else
                    boxSlide_ = 1.0;
            } else {
                boxSlide_ = 1.0;
            }
        }

        const float s = static_cast<float>(snd);
        out[i * 2 + 0] = s;
        out[i * 2 + 1] = s;
    }

    (void)fVolume;  // used indirectly through knobVolume already
}
