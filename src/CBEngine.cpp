// Screambox DSP engine — lifted from src/SilverBox.cpp (Frank Reitz / Bioroid
// 2004, BSD-style licence in license-bmd.txt) with wwelti's DSP-loop optimizations, stripped of VST
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
    params_[int(CbParam::PwmLfoRate)].store(0.0f);        // retired, read by nothing
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

    // Generate a random pattern on every startup so the synth makes noise
    // even before the user loads a .roq file. Nicely flavour at medium
    // wildness matches the 2002 Delphi "Chaos nicely" defaults.
    // Seed the live buffer directly; no writer needed before any thread exists.
    // The idle buffer is left at its default and is overwritten from live by
    // the first SongWriter, so the two never diverge.
    song_[0].patterns.assign(1, makeRandomPattern(RandomFlavor::Nicely, 0.5f));
    freshIds(song_[0]);
    reindex(song_[0]);

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
    while (e_.readerIdx_.load(std::memory_order_seq_cst) == next_)
        std::this_thread::yield();
    e_.song_[next_] = e_.song_[e_.liveIdx_.load(std::memory_order_relaxed)];
}

void CBEngine::SongWriter::commit() {
    SongData& d = e_.song_[next_];
    if (d.patterns.empty())                      // invariant the reader relies on
        d.patterns.push_back(CbPattern{});
    reindex(d);
    ++d.serial;   // copied from live, so this is always new
    // seq_cst, not release: the reader's publish-then-recheck is a Dekker
    // handshake, and the NEXT writer's load of readerIdx_ must not be ordered
    // before this store. The writer mutex happens to fence on today's
    // platforms; this makes the protocol not depend on that.
    e_.liveIdx_.store(next_, std::memory_order_seq_cst);
}

void CBEngine::freshIds(SongData& d) {
    d.patIds.clear();
    for (size_t i = 0; i < d.patterns.size(); ++i) d.patIds.push_back(d.nextPatId++);
}

void CBEngine::reindex(SongData& d) {
    if (d.patIds.size() != d.patterns.size()) freshIds(d);
    // Allocation here is on the UI thread, inside a writer; the audio thread
    // only ever reads the finished vector.
    d.patStart.resize(d.patterns.size() + 1);
    int at = 0;
    for (size_t i = 0; i < d.patterns.size(); ++i) {
        d.patStart[i] = at;
        at += d.patterns[i].lastStep + 1;
    }
    d.patStart[d.patterns.size()] = at;
}

namespace {
// How a stored sweep value becomes a parameter value, per sweep target. The
// first seven are not in here: they keep their original expression in
// process(), verbatim, which is what keeps their output bit-identical.
struct SweepDest {
    CbParam id;
    float   div;        // stored value / div = parameter value
    float   lo, hi;     // clamp, in parameter units
    float   floorHz;    // geometric targets only: stands in for an endpoint <= 0
    bool    geometric;  // equal ratios per step instead of equal differences
    bool    zeroIsOff;  // an endpoint of exactly 0 means off, at that endpoint
};
constexpr SweepDest kSweepDests[CBEngine::kSweepTargets] = {
    // 0..6 are placeholders; see process().
    {CbParam::Volume,    100.0f, 0, 0, 0, false, false},
    {CbParam::Cutoff,    100.0f, 0, 0, 0, false, false},
    {CbParam::Resonance, 100.0f, 0, 0, 0, false, false},
    {CbParam::EnvMod,    100.0f, 0, 0, 0, false, false},
    {CbParam::Decay,     100.0f, 0, 0, 0, false, false},
    {CbParam::Accent,    100.0f, 0, 0, 0, false, false},
    {CbParam::Tuning,    100.0f, 0, 0, 0, false, false},
    {CbParam::AccentSquelch,     100.0f,  0.0f, 2.0f,     0.0f,  false, false},
    {CbParam::PWM,               100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::SubOscLevel,       100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::NoiseLevel,        100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DupletPWM,         100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DupletDetune,     1200.0f, -1.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DupletLevel,       100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    // 14..16 are RETIRED (the PWM LFO's controls; see sweepTargetRetired).
    // loadRoq() drops sweeps on them, and their parameters are read by nothing,
    // so these rows only keep the table's numbering.
    {CbParam::PwmLfoRate,       1000.0f,  0.0f, 100.0f,   0.05f, true,  true },
    {CbParam::PwmLfoDepth,       100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::PwmLfoDupletDepth, 100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DistGain,          100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DistTone,          100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::DistLevel,         100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::CompThreshold,     100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    {CbParam::CompDepth,         100.0f,  0.0f, 1.0f,     0.0f,  false, false},
    // The bottom of the Frequency slider. The carrier has no "off" - Amount is
    // what switches the ring modulator off.
    {CbParam::RingHz,            100.0f,  0.0f, 20000.0f, 5.0f,  true,  false},
    {CbParam::RingAmount,        100.0f,  0.0f, 1.0f,     0.0f,  false, false},
};

// The value of sweep target k (7 and up) at fraction t of a sweep from a to
// b, both in stored units.
float sweepValueNew(int k, float a, float b, float t) {
    const SweepDest& d = kSweepDests[k];
    double v;
    if (d.geometric) {
        if (d.zeroIsOff && ((a <= 0.0f && t <= 0.0f) || (b <= 0.0f && t >= 1.0f)))
            return 0.0f;
        const double fa = (a > 0.0f) ? a / d.div : d.floorHz;
        const double fb = (b > 0.0f) ? b / d.div : d.floorHz;
        v = fa * std::pow(fb / fa, (double)t);
    } else {
        v = ((double)a + ((double)b - (double)a) * (double)t) / d.div;
    }
    if (v < d.lo) v = d.lo;
    if (v > d.hi) v = d.hi;
    return (float)v;
}

// An oscillating sweep's depth at fraction t, in parameter units. Always
// linear and never clamped - a depth is a distance, may be negative (the wave
// upside down), and for a frequency target is a distance in Hz. Rounded
// through float like every other sweep value, which is also what makes a
// depth of n percent exactly the n / 100.0f the old LFO depth slider stored.
float oscDepthAt(int k, float a, float b, float t) {
    return (float)(((double)a + ((double)b - (double)a) * (double)t) / kSweepDests[k].div);
}

// The range an oscillating sweep may move target k through, in parameter
// units: the slider's own. The first seven have no row of their own above.
void oscRange(int k, double& lo, double& hi) {
    if (k >= CBEngine::kMainSweepTargets) {
        lo = kSweepDests[k].lo; hi = kSweepDests[k].hi;
        return;
    }
    const CbParam id = kSweepDests[k].id;
    const bool bipolar = id == CbParam::Resonance || id == CbParam::EnvMod
                      || id == CbParam::Tuning;
    lo = bipolar ? -1.0 : 0.0;
    hi = 1.0;
}

// One cycle of each shape, p in [0, 1). Every shape starts at the centre
// (or, for the square, at the top) and goes UP first, so a start phase of 0
// means the same thing for all of them. The triangle is the PWM LFO's,
// expression for expression - that is what makes the converted LFO
// bit-identical.
double oscWave(int shape, double p) {
    switch ((CBEngine::OscShape)shape) {
        case CBEngine::OscShape::Triangle:
            return (p < 0.25) ? 4.0 * p
                 : (p < 0.75) ? 2.0 - 4.0 * p
                              : 4.0 * p - 4.0;
        case CBEngine::OscShape::Sine:    return std::sin((double)twopi * p);
        case CBEngine::OscShape::Square:  return (p < 0.5) ? 1.0 : -1.0;
        case CBEngine::OscShape::SawUp:   return (p < 0.5) ? 2.0 * p : 2.0 * p - 2.0;
        case CBEngine::OscShape::SawDown: return (p < 0.5) ? -2.0 * p : 2.0 - 2.0 * p;
        default:                          return 0.0;
    }
}

double frac1(double x) { return x - std::floor(x); }
} // namespace

CbParam CBEngine::sweepTargetParam(int idx) {
    return (idx >= 0 && idx < kSweepTargets) ? kSweepDests[idx].id : CbParam::NumParams;
}

void CBEngine::setParamSweeps(int sliderIdx, const std::vector<ParamSweep>& sweeps) {
    if (sliderIdx < 0 || sliderIdx >= kSweepTargets) return;
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
    freshIds(w.data());
    w.commit();
    patternIdx_.store(0, std::memory_order_relaxed);
}
void CBEngine::setPatterns(const std::vector<CbPattern>& v) {
    SongWriter w(*this);
    if (v.empty()) w.data().patterns.assign(1, CbPattern{});
    else           w.data().patterns = v;
    freshIds(w.data());
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
// Neither of these touches patternIdx_. They used to take "current" to mean the
// playback index and then move it - so adding a pattern while the song played
// threw playback onto the new, empty pattern. Playback now follows the pattern
// it was on by identity; see the patIds note in process().
int CBEngine::insertPatternAfter(int idx) {
    SongWriter w(*this);
    SongData& d = w.data();
    int at = idx + 1;
    if (at < 0) at = 0;
    if (at > (int)d.patterns.size()) at = (int)d.patterns.size();
    d.patterns.insert(d.patterns.begin() + at, CbPattern{});
    d.patIds.insert(d.patIds.begin() + at, d.nextPatId++);
    w.commit();
    return at;
}
bool CBEngine::deletePattern(int idx) {
    SongWriter w(*this);
    SongData& d = w.data();
    if (d.patterns.size() <= 1) return false;                     // no commit
    if (idx < 0 || idx >= (int)d.patterns.size()) return false;   // no commit
    d.patterns.erase(d.patterns.begin() + idx);
    d.patIds.erase(d.patIds.begin() + idx);
    w.commit();
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
    // Sweep overrides belong to a playing song position. Stopped, the sliders
    // are what you hear; started, the first step evaluates them afresh.
    for (int i = 0; i < int(CbParam::NumParams); ++i) sweptOn_[i] = false;
    // Oscillating sweeps likewise; and the next step evaluated is a
    // discontinuity, so an Hz-mode phase is re-derived from the song position.
    for (auto& r : osc_) r.on = false;
    oscHzN_ = 0;
    lastSweepPos_ = -2;
    stepFrac_ = 0.0;
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

// The step that follows (p, s) in what will actually PLAY: the next step of the
// same pattern, or - at the pattern's end - step 0 of the next pattern in the
// song (FullSequence) or of the same pattern again (looping one pattern).
//
// One function, used by everything that looks ahead: the slide decision in
// triggerStep(), the glide target, and the tie walk below. They used to
// disagree at the bar line - the slide decision looked at THIS pattern's step
// 0, the glide target at the NEXT pattern's - which was invisible only because
// resetFilters() then cancelled the slide anyway.
static void nextPlayedStep(const std::vector<CbPattern>& pats, bool fullSequence,
                           int p, int s, int& np, int& ns) {
    const int count = (int)pats.size();
    if (count <= 0) { np = 0; ns = 0; return; }
    if (s < pats[p].lastStep) { np = p; ns = s + 1; return; }
    ns = 0;
    np = fullSequence ? (p + 1) % count : p;
}

// How many samples a note starting at `step` will actually *sound* for: its
// own step, plus every step tied to it by a slide, less the tail of the final
// step after the gate closes. Only "reverse step" needs this - a backwards
// envelope has to know where its peak must land, and the peak has to land when
// the note stops being audible, not when the step ends. Aiming at the step
// boundary instead would put the brightest moment after the gate had already
// shut, which is silent and therefore pointless.
//
// The walk follows nextPlayedStep(), so a tie that crosses the bar line runs on
// into the pattern that actually plays next, at that pattern's step length.
// Where every step is the same whole number of samples, the sum below is
// exactly `tied * firstLen`, which is what this used to compute.
static int audibleNoteLength(const std::vector<CbPattern>& pats, int patIdx, int step,
                             bool fullSequence, int firstLen,
                             double sampleRate, double tempo) {
    const int count = (int)pats.size();
    if (count <= 0) return (firstLen > 1) ? firstLen : 1;
    // Same condition triggerStep() uses for bSlideNext_, walked forward. The
    // guard is not paranoia: a song with a slide on every step would
    // otherwise never terminate.
    int guardMax = 0;
    if (fullSequence) { for (const auto& q : pats) guardMax += q.lastStep + 1; }
    else              { guardMax = pats[patIdx].lastStep + 1; }
    int p = patIdx, i = step;
    double total   = (double)firstLen;
    double lastLen = (double)firstLen;
    for (int guard = 0; guard < guardMax; ++guard) {
        int np, ns;
        nextPlayedStep(pats, fullSequence, p, i, np, ns);
        const CbStep& cur = pats[p].steps[i];
        if (!(cur.slide && cur.pitch != 0 && pats[np].steps[ns].pitch != 0))
            break;
        p = np; i = ns;
        lastLen = CBEngine::exactStepSamples(sampleRate, tempo, pats[p].triplet);
        total  += lastLen;
    }
    // triggerStep() re-latches boxAccent_ on every tied step, so the gate
    // threshold that actually ends the note belongs to the *last* step of the
    // tie, not the first. Accented steps hold the gate open ~25% longer.
    const double thresh = (lastLen * (1.0 / 1.9))
                        * (1.0 - (pats[p].steps[i].accent ? 0.25 : 0.0));
    const double len = total - thresh;
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
        // an oscillating sweep's phase (osc_), which must NOT be reset here:
        // that one is a slow modulator whose whole point is moving across
        // the bar, and it is at a rate where drift is audible AS drift. The ring carrier is at audio rate,
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
        // Swept or not: sweeps are evaluated before triggerStep(), so a
        // squelch sweep's value for this step is already in force here.
        const float sq = effParam(CbParam::AccentSquelch);
        boxAccentFrq_ = (tempAccentFrq_ + 2.0) * sq;
    }

    // Slide flag only takes effect if both current and next pitch are non-zero.
    bSlideNext_ = currStep.slide && currStep.pitch != 0 && nextStep.pitch != 0;
}

// Samples per sequencer step, exactly - a fraction in general. The straight
// branch is the expression it always was, so where it comes out whole the
// sequencer plays exactly the steps it always played.
//
// Nothing else in the engine knows what a step is: the gate threshold,
// audibleNoteLength(), the reversed envelope and the slide rate all derive from
// this number, so they follow a triplet pattern automatically.
double CBEngine::exactStepSamples(double sampleRate, double tempo, bool triplet) {
    return triplet
        ? (sampleRate * 60.0) / (tempo * 3.0)    // eighth triplet
        : (sampleRate * 60.0) / (tempo * 4.0);   // sixteenth
}

namespace {

// A step is the whole part of (exact length + the fraction owed so far); the
// rest is owed to the next step. Truncation is what the sequencer always did,
// so with nothing owed this is the old integer step length exactly.
inline int stepLenFrom(double exact, double owed) {
    const int n = static_cast<int>(exact + owed);
    return (n < 1) ? 1 : n;
}
inline double stepCarry(double exact, double owed, int len) {
    double c = (exact + owed) - (double)len;
    // A tempo change in the middle of a step can leave the step that was
    // actually played longer or shorter than the new exact value says; keep
    // the debt a fraction either way rather than paying it all at once.
    if (c < 0.0)  c = 0.0;
    if (c >= 1.0) c -= std::floor(c);
    return c;
}

// The values derived from the seven sweepable parameters. These used to be
// computed inline at the top of process(), once per block - so a sweep's new
// value, written at a step boundary in the middle of a block, waited for the
// NEXT block, and every note started with the previous step's value for up to
// a whole buffer. How long depended on the audio buffer size, so the same
// song sounded different on different sound cards. They are now recomputed at
// every step boundary as well. The expressions are the original ones, verbatim
// and in the same types, which is what keeps the no-sweep path bit-identical.
struct Knobs {
    double volume, accent, decayMul, decayMulInv, tune, twoPowTune, envMod, cutoff, preRezo;
};

Knobs deriveKnobs(float fVolume, float fTuning, float fCutoff, float fRezo, float fEnvMod,
                  float fDecay, float fAccent, int sndType, bool resoBoost) {
    Knobs k{};
    // Volume scaling. Slider=100 lands at ~0.14 pass-through — chosen so
    // even accented peaks stay below the dist() clipper at default settings,
    // giving the user a clean bed to add outboard distortion later. Saw
    // waveform runs slightly hotter than square, compensated with *1.5.
    double knobVolume = fVolume * 0.14;
    if (sndType == 0) knobVolume *= 1.5;
    k.volume = knobVolume;
    k.accent = fAccent * 0.65;
    double knobDecay = fDecay;
    // The reciprocal is what runs the envelope backwards for a reversed step:
    // 4*r^(N-t) == (4*r^N) * (1/r)^t, so a time-reversed decay costs one
    // different starting value and one different multiplier, nothing else.
    k.decayMul    = knobDecay * 0.0005 + 0.9995;
    k.decayMulInv = (k.decayMul > 0.0) ? (1.0 / k.decayMul) : 1.0;
    // Tuning is bipolar in the rebuild: fTuning already comes in as -1..+1
    // with 0 = neutral (VST used 0..1 with 0.5 as centre; slider widget
    // now provides the bipolar range directly).
    k.tune       = fTuning;
    k.twoPowTune = std::pow(2.0, k.tune);
    k.envMod     = fEnvMod;
    double knobCutoff = (std::pow(2.0, fCutoff * 0.5) - 1.0);
    k.cutoff = (std::pow(2.0, knobCutoff * 0.8) - 0.987);
    double pre_knobRezo = (std::sin(fRezo * halfpi) * 75.0 + 25.0) / 100.0;
    if (resoBoost) {
        // Extend the ceiling rather than lifting the whole range: a quartic
        // term is negligible through the usable part of the travel and only
        // bites at the end, so the knob keeps its familiar feel and only the
        // top ~10 % is new. Full travel lands near 1.06 at the filter, which
        // is past the stability point - hence the _boost filter variant with
        // its bounded resonant integrator.
        pre_knobRezo += 0.07 * (double)fRezo * (double)fRezo * (double)fRezo * (double)fRezo;
    }
    k.preRezo = pre_knobRezo;
    return k;
}

} // namespace

long long CBEngine::songLengthSamples(const std::vector<CbPattern>& pats,
                                      double sampleRate, double tempo) {
    // Mirrors the sequencer step for step: the owed fraction starts at zero on
    // transport start and is carried across pattern boundaries.
    long long total = 0;
    double owed = 0.0;
    for (const auto& p : pats) {
        const double exact = exactStepSamples(sampleRate, tempo, p.triplet);
        for (int s = 0; s <= p.lastStep; ++s) {
            const int len = stepLenFrom(exact, owed);
            owed = stepCarry(exact, owed, len);
            total += len;
        }
    }
    return total;
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

    const float fTempo    = params_[int(CbParam::Tempo)].load(std::memory_order_relaxed);
    const float fWaveform = params_[int(CbParam::Waveform)].load(std::memory_order_relaxed);
    const float fDupletWv   = params_[int(CbParam::DupletWaveform)].load(std::memory_order_relaxed);
    const bool  bResoBoost  = params_[int(CbParam::ResoBoost)].load(std::memory_order_relaxed) > 0.5f;
    const int   iDistType   = (int)(params_[int(CbParam::DistType)].load(std::memory_order_relaxed) + 0.5f);
    const int   dupletSndType = fDupletWv > 0.5f ? 1 : 0;
    boxSndType_ = fWaveform > 0.5f ? 1 : 0;

    // A parameter as the audio thread should use it now - swept or not.
    auto eff = [this](CbParam p) -> float { return effParam(p); };

    // The oscillator-dialog and acid-FX values, and everything derived from
    // them. Sweepable since v14, so - like Knobs below, and for the same
    // reason - computed at the top of the block AND again at every step
    // boundary: a sweep's value written at a step boundary must not wait for
    // the next block. The expressions are the ones that used to sit here as
    // per-block constants, verbatim and in the same types, so with no sweep
    // running every value is exactly what it was.
    struct Voice {
        float  pwm, subLevel, dupletPwm, dupletLvl, noiseLvl;
        double dupletFactor, pwmThresh, dupletPwmThresh;
        CbDistCoeffs distC;
        CbCompCoeffs compC;
        CbRingCoeffs ringC;
        bool   comp, ring;
    };
    auto voiceNow = [&]() {
        Voice v{};
        v.pwm       = eff(CbParam::PWM);
        v.subLevel  = eff(CbParam::SubOscLevel);
        v.dupletPwm = eff(CbParam::DupletPWM);
        const float fDupletDet = eff(CbParam::DupletDetune);
        v.dupletLvl = eff(CbParam::DupletLevel);
        v.noiseLvl  = eff(CbParam::NoiseLevel);
        // Coefficients are derived per block (and per step), never per sample -
        // same tactic as the gate and envelope constants below.
        if (iDistType != 0)
            cbDistPrepare(v.distC, iDistType,
                          eff(CbParam::DistGain), eff(CbParam::DistTone), eff(CbParam::DistLevel),
                          sampleRate_);
        // Only when the effect is actually on, so the off path costs a single
        // comparison.
        v.comp = eff(CbParam::CompDepth)  > 0.0f;
        v.ring = eff(CbParam::RingAmount) > 0.0f;
        if (v.comp)
            cbCompPrepare(v.compC, eff(CbParam::CompThreshold), eff(CbParam::CompDepth),
                          sampleRate_);
        if (v.ring)
            cbRingPrepare(v.ringC, eff(CbParam::RingHz), eff(CbParam::RingAmount),
                          sampleRate_);
        // Detune factor: -1..+1 = ±1 octave → frequency factor 2^detune.
        // 0 = unison, +1 = octave up, -1 = octave down, arbitrary fractions in
        // between. The sub oscillator does NOT follow it: it runs at half the
        // MAIN oscillator's rate (see subOscPhase_ below).
        v.dupletFactor = std::pow(2.0, fDupletDet);
        // PWM thresholds for square, one per oscillator. Widened to ~1..99 %
        // duty at the extremes so the pulse narrows into near-silent spikes.
        //
        // These stay hoisted out of the sample loop and are used verbatim
        // while no oscillating sweep is on the target, which keeps the
        // stock-303 path bit-identical. While one is, the threshold is
        // recomputed per sample instead (see the loop below) - a block is
        // longer than one cycle at the top of the rate range, so per-block
        // modulation would alias.
        v.pwmThresh       = pi + (v.pwm       - 0.5) * pi * 1.98;
        v.dupletPwmThresh = pi + (v.dupletPwm - 0.5) * pi * 1.98;
        return v;
    };
    Voice V = voiceNow();

    // The seven main sliders: re-derived at every step boundary (see Knobs).
    auto knobsNow = [&]() {
        return deriveKnobs(eff(CbParam::Volume), eff(CbParam::Tuning), eff(CbParam::Cutoff),
                           eff(CbParam::Resonance), eff(CbParam::EnvMod), eff(CbParam::Decay),
                           eff(CbParam::Accent), boxSndType_, bResoBoost);
    };
    Knobs K = knobsNow();

    const bool running = running_.load(std::memory_order_relaxed);

    // Acquire the live song WITHOUT locking or copying. Publish the index we
    // intend to read, then re-read liveIdx_ and retry if a writer flipped in
    // between; that ordering is what stops a writer from choosing this very
    // buffer as its next target. See the protocol note in CBEngine.h.
    int songIdx;
    do {
        songIdx = liveIdx_.load(std::memory_order_acquire);
        readerIdx_.store(songIdx, std::memory_order_seq_cst);
    } while (songIdx != liveIdx_.load(std::memory_order_seq_cst));

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
    const std::vector<int>&      patStart = song.patStart;
    const std::vector<uint32_t>& patIds   = song.patIds;
    const int patCount = (int)pats.size();
    const bool fullSeq = playMode_.load() == PlayMode::FullSequence;

    // Writers guarantee patterns is never empty; this is belt-and-braces for
    // the real-time path, and deliberately allocation-free if it ever fires.
    static const CbPattern kEmptyPattern{};
    int patIdx = patternIdx_.load(std::memory_order_relaxed);
    // A writer committed since the last block. If a pattern was inserted or
    // deleted in front of the one playing, its index moved; find it again by
    // identity so the playhead stays on the pattern it was on. A transport
    // request this block has already said where to play, so leave that alone.
    // If the playing pattern itself was deleted, carry on with the one that
    // took its place - the pattern that would have played next anyway.
    if (song.serial != lastSongSerial_) {
        if (running && pendingReset == int(TransportReset::None)
            && playingPatId_ != 0 && patCount > 0) {
            int found = -1;
            for (int p = 0; p < (int)patIds.size(); ++p)
                if (patIds[p] == playingPatId_) { found = p; break; }
            if (found < 0) found = (patIdx >= 0) ? patIdx % patCount : 0;
            if (found != patIdx) {
                patIdx = found;
                patternIdx_.store(patIdx, std::memory_order_relaxed);
            }
        }
        lastSongSerial_ = song.serial;
    }
    if (patIdx < 0 || patIdx >= patCount) patIdx = 0;
    // A pointer into the live buffer, not a per-block copy of the pattern.
    const CbPattern* pat = pats.empty() ? &kEmptyPattern : &pats[patIdx];
    if (patIdx < (int)patIds.size()) playingPatId_ = patIds[patIdx];
    auto patAt = [&](int p) -> const CbPattern& {
        return (p >= 0 && p < patCount) ? pats[p] : kEmptyPattern;
    };

    // Length of the current step. Owed fraction included; see stepFrac_.
    double stepExact = exactStepSamples(sampleRate_, fTempo, pat->triplet);
    samplesPerStep_ = stepLenFrom(stepExact, stepFrac_);

    // Interactive sweeps from the sweep-editor UI.
    //
    // ORDER IS UI SLIDER ORDER, and it deliberately differs from the legacy
    // .cb table further down (search kLegacyIds). Do NOT "reconcile" the two —
    // see the note at that second table for the evidence. This one is indexed
    // by the main window's left-to-right slider order, documented in RoqIO.h,
    // and it is what .roq sweeps and the sweep editor's sliderIdx mean.
    static const CbParam kIds[kMainSweepTargets] = {
        CbParam::Volume, CbParam::Cutoff, CbParam::Resonance, CbParam::EnvMod,
        CbParam::Decay, CbParam::Accent, CbParam::Tuning
    };
    // Where (pattern, step) falls on the song's step timeline. A sweep advances
    // the same amount per STEP wherever it is: positions used to be measured
    // in patterns (pattern + step / its length), so a sweep over a 4-step and a
    // 16-step pattern spent half its travel in the short one. Clamping the step
    // keeps an endpoint inside its own pattern after the user shortens one
    // underneath an existing sweep. An endpoint naming a pattern the song does
    // not have stays past the end, in order, and is never reached.
    auto posOf = [&](int p, int st) -> long {
        if (st < 0) st = 0;
        if (p >= 0 && p < patCount && p < (int)patStart.size()) {
            const int n = pats[p].lastStep + 1;
            if (st >= n) st = n - 1;
            return (long)patStart[p] + st;
        }
        if (st > 63) st = 63;
        const long end = patStart.empty() ? 0L : (long)patStart.back();
        return (p < 0) ? (long)p * 64 + st : end + (long)(p - patCount) * 64 + st;
    };
    // Samples from absolute step a to absolute step b (a <= b), at the
    // pattern's exact step length - fractions included, carry not. Used only
    // to re-derive an Hz-mode phase after a discontinuity; continuous play
    // accumulates instead. Positions outside the song count nothing.
    auto samplesBetween = [&](long a, long b) -> double {
        double total = 0.0;
        if (a < 0) a = 0;
        for (int p = 0; p < patCount && p + 1 < (int)patStart.size(); ++p) {
            const long s0 = std::max(a, (long)patStart[p]);
            const long s1 = std::min(b, (long)patStart[p + 1]);
            if (s1 > s0)
                total += (double)(s1 - s0) * exactStepSamples(sampleRate_, fTempo, pats[p].triplet);
        }
        return total;
    };
    // For each slider: the first sweep whose [start, end] covers `cur`
    // interpolates; failing that, the sweep that ended most recently before
    // `cur` keeps its end value if it holds; failing that, no override and
    // the slider's own setting applies.
    //
    // Straight and oscillating sweeps make that choice separately (SweepOsc):
    // the straight one, or the slider, gives the centre, and the oscillating
    // one moves the value around it. A target with no oscillating sweep runs
    // exactly the code it ran before they existed.
    auto evaluateSweeps = [&](long cur) {
        const bool jump = cur != lastSweepPos_ + 1;
        lastSweepPos_ = cur;
        oscHzN_ = 0;
        for (int k = 0; k < kSweepTargets; ++k) {
            const auto& list = paramSweeps[k];
            const ParamSweep* cover = nullptr;
            const ParamSweep* before = nullptr;
            long beforeEnd = 0;
            float t = 0.0f;
            // The oscillating sweep, chosen by the same rule.
            const ParamSweep* oCover = nullptr;
            const ParamSweep* oBefore = nullptr;
            long oBeforeEnd = 0, oStart = 0;
            int  oIdx = -1;
            float oT = 0.0f;
            for (size_t si = 0; si < list.size(); ++si) {
                const ParamSweep& sw = list[si];
                const long start = posOf(sw.startPat, sw.startStep);
                const long end   = posOf(sw.endPat,   sw.endStep);
                if (sw.osc.on()) {
                    if (oCover) continue;
                    if (cur >= start && cur <= end) {
                        oCover = &sw; oStart = start; oIdx = (int)si;
                        oT = (end > start) ? (float)(cur - start) / (float)(end - start) : 0.0f;
                    } else if (end < cur && (!oBefore || end > oBeforeEnd)) {
                        oBefore = &sw; oBeforeEnd = end; oStart = start; oIdx = (int)si;
                    }
                    continue;
                }
                if (cover) continue;
                if (cur >= start && cur <= end) {
                    cover = &sw;
                    t = (end > start) ? (float)(cur - start) / (float)(end - start) : 0.0f;
                    continue;
                }
                if (end < cur && (!before || end > beforeEnd)) { before = &sw; beforeEnd = end; }
            }
            const int id = int(sweepTargetParam(k));
            // The straight part, unchanged.
            if (!cover && !(before && before->hold)) {
                sweptOn_[id] = false;
            } else if (k >= kMainSweepTargets) {
                // The targets added in v14 decode through their own table, so
                // the original seven below keep their expression untouched.
                sweptVal_[id] = cover
                    ? sweepValueNew(k, (float)cover->startVal, (float)cover->endVal, t)
                    : sweepValueNew(k, (float)before->endVal, (float)before->endVal, 1.0f);
                sweptOn_[id]  = true;
            } else {
                // Raw matches the UI sliders (-100..+100 bipolar, 0..100
                // unipolar). Map to the param's native 0..1 (unipolar) or -1..+1
                // (bipolar) range.
                const float raw = cover
                    ? (float)cover->startVal + ((float)cover->endVal - (float)cover->startVal) * t
                    : (float)before->endVal;
                float v = raw / 100.0f;
                const bool bipolar = kIds[k] == CbParam::Resonance || kIds[k] == CbParam::EnvMod
                                  || kIds[k] == CbParam::Tuning;
                if (!bipolar && v < 0.0f) v = 0.0f;   // Volume/Cutoff/Decay/Accent
                sweptVal_[id] = v;
                sweptOn_[id]  = true;
            }

            // The oscillating part.
            OscRun& r = osc_[k];
            const ParamSweep* o = oCover ? oCover : ((oBefore && oBefore->hold) ? oBefore : nullptr);
            if (!o) { r.on = false; continue; }
            // A different sweep from last step's, or a position that does not
            // follow on from it: the phase is taken afresh from the position.
            const bool fresh = jump || !r.on || r.which != oIdx || r.start != oStart;
            r.on      = true;
            r.which   = oIdx;
            r.start   = oStart;
            r.stepsIn = cur - oStart;
            r.shape   = o->osc.shape;
            r.hz      = o->osc.hz;
            r.phase0  = (double)o->osc.phase;
            r.depth   = (double)(oCover ? oscDepthAt(k, (float)o->startVal, (float)o->endVal, oT)
                                        : oscDepthAt(k, (float)o->endVal, (float)o->endVal, 1.0f));
            const double rate = o->osc.rate > 0.0f ? (double)o->osc.rate : 0.0;
            if (r.hz) {
                // The PWM LFO's own increment, expression for expression. At
                // or above the sample rate, only its fraction: the phase is
                // read modulo 1, so that is the same wave, and it keeps the
                // single wrap per sample in the loop below sufficient.
                r.inc = rate / sampleRate_;
                if (r.inc >= 1.0) r.inc -= std::floor(r.inc);
                if (fresh) r.acc = frac1(r.phase0 + samplesBetween(oStart, cur) * r.inc);
                oscHz_[oscHzN_++] = k;
            } else {
                r.spc = rate;
            }
            if (sweepTargetPerSample(k)) continue;   // moved in the sample loop
            // Everything else moves once per step: the wave where this step's
            // first sample has it, around whatever the straight part left.
            const double ph = r.hz ? frac1(r.acc + r.inc)
                            : (r.spc > 0.0 ? frac1(r.phase0 + (double)r.stepsIn / r.spc)
                                           : frac1(r.phase0));
            const double centre = sweptOn_[id] ? (double)sweptVal_[id]
                                               : (double)params_[id].load(std::memory_order_relaxed);
            double lo, hi;
            oscRange(k, lo, hi);
            sweptVal_[id] = (float)std::clamp(centre + r.depth * oscWave(r.shape, ph), lo, hi);
            sweptOn_[id]  = true;
        }
    };
    // The phase an oscillating sweep has at this sample, per-sample targets
    // only. Hz mode reads the accumulator, already advanced for this sample.
    auto oscPhaseNow = [&](const OscRun& r) -> double {
        if (r.hz) return r.acc;
        if (!(r.spc > 0.0)) return frac1(r.phase0);
        return frac1(r.phase0 + ((double)r.stepsIn
                                 + (double)samplesIntoStep_ / (double)samplesPerStep_) / r.spc);
    };

    // Envelope constants (from SilverBox.cpp wwelti branch)
    double boxMEG_c0, boxMEG_c1;
    if (boxAccent_) { boxMEG_c0 = -1.75;  boxMEG_c1 = 0.00009; }
    else            { boxMEG_c0 = -1.0;   boxMEG_c1 = 0.0000082; }

    double gate_c0, gate_c1;
    if (gateStat_ == 0) { gate_c0 = 1.0; gate_c1 = 0.9; }
    else                { gate_c0 = 0.0; gate_c1 = 0.004; }

    for (int i = 0; i < frames; ++i) {
        // --- sequencer tick ---
        if (running) {
            if (currentStep_ < 0 || samplesIntoStep_ >= samplesPerStep_) {
                // Settle the step that just ended before sizing the next one.
                if (currentStep_ >= 0)
                    stepFrac_ = stepCarry(stepExact, stepFrac_, samplesPerStep_);
                samplesIntoStep_ = 0;
                currentStep_ = (currentStep_ + 1);
                if (currentStep_ > pat->lastStep) {
                    // In FullSequence mode advance through all patterns in
                    // order, looping at the end. In CurrentPatternOnly mode
                    // stay on the initial pattern forever.
                    if (fullSeq && patCount > 0) {
                        patIdx = (patIdx + 1) % patCount;
                        pat = &pats[patIdx];
                        // Plain atomic store — this used to take patternMu_,
                        // the second lock the audio thread held per block.
                        patternIdx_.store(patIdx, std::memory_order_relaxed);
                        if (patIdx < (int)patIds.size()) playingPatId_ = patIds[patIdx];
                        // Step duration is a property of the PATTERN, and
                        // the pattern changes here, inside the sample loop.
                        // Leaving it stale until the next block boundary
                        // would run a triplet pattern following a straight one
                        // at the wrong rate for up to a whole buffer: a flam on
                        // the first note, once per loop, that moves with the
                        // audio buffer size. Recompute it here.
                        stepExact = exactStepSamples(sampleRate_, fTempo, pat->triplet);
                    }
                    currentStep_ = 0;
                }
                samplesPerStep_ = stepLenFrom(stepExact, stepFrac_);
                int nextPat, nextStep;
                nextPlayedStep(pats, fullSeq, patIdx, currentStep_, nextPat, nextStep);
                // Publish for the UI. Once per step, not per sample.
                displayStep_.store(currentStep_, std::memory_order_relaxed);

                // Sweeps BEFORE the note starts, and folded into the knob
                // values right here, so the step's value is in force from its
                // first sample - and so a reversed step below is sized with
                // the swept Decay rather than the previous step's.
                evaluateSweeps((patIdx < (int)patStart.size() ? (long)patStart[patIdx] : 0L)
                               + currentStep_);

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
                // back on; it is NOT dead code to be deleted casually. It writes
                // the same overrides as the interactive sweeps, after them, so
                // it wins where both apply - as it did before.
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
                    static const CbParam kLegacyIds[7] = {
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
                        if (kLegacyIds[k] == CbParam::Volume && v < 0.0f) v = 0.0f;
                        sweptVal_[int(kLegacyIds[k])] = v;
                        sweptOn_[int(kLegacyIds[k])]  = true;
                    }
                }
                K = knobsNow();
                V = voiceNow();

                // The filter reset at every pattern start is the 2004 one
                // ("reset filter every 16 steps"). It is skipped while a slide
                // carries a note across the bar line: zeroing the gate, the
                // filter memories and the oscillator under a note that is
                // still sounding is a click, and it threw the glide away - the
                // note never reached the pitch it was sliding to. The tie is
                // one note; the reset belongs to the next new one. Note that
                // the oscillating sweeps' phases (osc_) are untouched either
                // way (invariant 1).
                if (currentStep_ == 0 && !bNoteSlide_) resetFilters();
                triggerStep(pat->steps[currentStep_], patAt(nextPat).steps[nextStep]);

                // Reverse step: run the filter envelope backwards by starting
                // boxDecay_ where the forward envelope would have *ended* and
                // growing it back to 4.0 over the note. Not a sign flip - an
                // inverted decay is fastest at the note's start, where a
                // genuinely reversed note is slow first and peaks at the end.
                // Seeded here rather than in triggerStep() because the tie
                // walk needs the song, and skipped on a slide continuation
                // because the head of the tie already sized the envelope for
                // the whole note.
                if (stepReverse_ && !bNoteSlide_ && gateStat_ == 0) {
                    const int len = audibleNoteLength(pats, patIdx, currentStep_, fullSeq,
                                                      samplesPerStep_, sampleRate_, fTempo);
                    double start = 4.0 * std::pow(K.decayMul, (double)len);
                    // Degenerate corner - very slow tempo, fastest decay, a long
                    // tie - can underflow r^N to zero, and zero times anything
                    // stays zero, which would strand the envelope at silence.
                    // Flooring makes it peak early and hold instead, which is
                    // the graceful failure rather than the silent one.
                    if (!(start > 1e-30)) start = 1e-30;
                    boxDecay_ = start;
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
            boxDecay_ *= K.decayMulInv;
            // 4.0 is where a forward envelope begins, so it is the ceiling a
            // backwards one may reach. The clamp matters if the tie walk and
            // the actual note length ever disagree - a tempo change during a
            // long reversed note, say - and turns an exponential runaway into
            // a hold.
            if (boxDecay_ > 4.0) boxDecay_ = 4.0;
        } else {
            boxDecay_ *= K.decayMul;
        }

        double boxCutoff = K.cutoff + (boxOscFrq_ / sampleRate_) * 0.33;
        // The two EnvMod terms collapse to knobEnvMod * (boxDecay_ - 2.0).
        // boxDecay_ runs 4.0 -> 0 across a normal note, so that factor sweeps
        // +2 -> -2 and the filter sweeps *through* the base cutoff rather than
        // down to it. "Reverse step" does not appear here at all any more: it
        // reverses boxDecay_'s direction of travel above, which is what makes
        // it compose with sweeps on EnvMod - the parameter is never touched.
        double tempcutoff = boxCutoff * getTab2Interpolated(
            boxMEG_ + (K.envMod * -2.0) + (boxDecay_ * K.envMod) + tempAccentFrq_ * K.accent);

        // Oscillator phase. Frank's VST hardcoded 176400 as the denominator
        // (88200 * 2). At 44.1 kHz runtime, his code produces output at
        // freq/4 of cNoteFreq (so cNoteFreq[1]=130.813 plays at 32.7 Hz,
        // standard C1 bass register). To preserve that behaviour at any
        // sample rate, scale the denominator to sampleRate * 4.
        const double phaseInc = twopi * (1.0 / (sampleRate_ * 4.0)) * boxOscFrq_ * K.twoPowTune;
        boxOsc_ += phaseInc;
        if (boxOsc_ > twopi) boxOsc_ -= twopi;
        // Duplet runs at main frequency × detune factor.
        dupletOsc_ += phaseInc * V.dupletFactor;
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

        // --- oscillating sweeps (what was the PWM LFO) ---
        // Hz-mode phases advance here, every sample, wherever their target is
        // evaluated: a per-step target reads its phase at the step boundary,
        // and that has to be the phase the song position has reached. With
        // none running this is one comparison.
        for (int j = 0; j < oscHzN_; ++j) {
            OscRun& r = osc_[oscHz_[j]];
            r.acc += r.inc;
            if (r.acc >= 1.0) r.acc -= 1.0;
        }
        // The two duty thresholds fall back to the hoisted block constants
        // when no oscillating sweep is on them, which is the stock path.
        // The expressions are the PWM LFO's, verbatim: centre plus depth
        // times the wave, clamped (not wrapped) to the duty range - a wrap
        // would snap the pulse inside out - then the threshold formula.
        double thrMain   = V.pwmThresh;
        double thrDuplet = V.dupletPwmThresh;
        if (osc_[8].on) {
            const OscRun& r = osc_[8];
            const double m = std::clamp((double)V.pwm
                                        + r.depth * oscWave(r.shape, oscPhaseNow(r)), 0.0, 1.0);
            thrMain = pi + (m - 0.5) * pi * 1.98;
        }
        if (osc_[11].on && V.dupletLvl > 0.0f) {
            const OscRun& r = osc_[11];
            const double m = std::clamp((double)V.dupletPwm
                                        + r.depth * oscWave(r.shape, oscPhaseNow(r)), 0.0, 1.0);
            thrDuplet = pi + (m - 0.5) * pi * 1.98;
        }

        double snd = 0.0;
        if (boxSndType_ == 0) {
            // Saw (PWM has no effect on saw; left unchanged — so the LFO is
            // inaudible here no matter the depth. The dialog says so.)
            snd = (twopi - boxOsc_) * (1.0 / twopi);
        } else {
            // Square with variable duty cycle. PWM slider shifts the
            // comparison threshold; at 0.5 (default) V.pwmThresh == pi → stock.
            snd = (boxOsc_ > thrMain) ? 0.5 : -0.5;
        }
        // Duplet voice — mixed in with its own level. Off at 0.
        if (V.dupletLvl > 0.0f) {
            double d;
            if (dupletSndType == 0) d = (twopi - dupletOsc_) * (1.0 / twopi);
            else                    d = (dupletOsc_ > thrDuplet) ? 0.5 : -0.5;
            snd += d * (double)V.dupletLvl;
        }
        // Sine sub-oscillator — thickens the bass an octave below.
        if (V.subLevel > 0.0f) {
            snd += std::sin(subOscPhase_) * (double)V.subLevel * 0.5;
        }
        // White noise, mixed in as just another oscillator: it lands before
        // the rest check and before the filter, so it is shaped by the cutoff
        // sweep and silenced on rests exactly like the tuned sources. Scaled
        // to +/-0.5 to match the square's amplitude. Skipped entirely at
        // level 0 so the generator state never advances and that path stays
        // bit-identical to the pre-noise engine.
        if (V.noiseLvl > 0.0f) {
            noiseState_ ^= noiseState_ << 13;
            noiseState_ ^= noiseState_ >> 17;
            noiseState_ ^= noiseState_ << 5;
            const double n = (double)(int32_t)noiseState_ * (1.0 / 4294967296.0);
            snd += n * (double)V.noiseLvl;
        }
        if (boxOscFrq_ == 0.0) snd = 0.0;

        if (boxSndType_ == 0) {
            snd = (double)IIR_FIRlp(lp3FIR_, 0.7, snd);
        } else {
            if (snd < 0.0) snd = (double)IIR_FIRlp(lp3FIR_, 0.7,   snd);
            else           snd = (double)IIR_FIRlp(lp3FIR_, 0.125, snd);
        }
        snd = (double)IIR_doIIRhigh(hp1_, 0.0068027, snd);

        double knobRezo = K.preRezo + (tempAccentEnv_ * 0.04) * K.accent;
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
        snd *= boxVolEnv_ * (tempAccentEnv_ * 5.0 + 1.0) * K.volume;

        // Type 0 keeps the exact 2004 call, untouched, so the default sound
        // is bit-identical to the engine before the pedal models existed.
        snd = (iDistType == 0) ? (double)dist_dist((long double)snd)
                               : cbDistProcess(V.distC, distState_, snd);

        // Compressor BEFORE the ring modulator on purpose: its follower would
        // otherwise chase the carrier's amplitude modulation and partly cancel
        // the ring, so the two amount controls would fight each other in a way
        // neither label explains. Both are skipped outright when off - not
        // multiplied by a neutral value - so no state advances and the off path
        // stays bit-identical to the engine before they existed.
        if (V.comp) snd = cbCompProcess(V.compC, compState_, snd);
        if (V.ring) snd = cbRingProcess(V.ringC, ringPhase_, snd);

        if (snd < -1.0) snd = -1.0;
        else if (snd > 1.0) snd = 1.0;

        // Render slide delta.
        boxOscFrq_ *= boxSlide_;

        if (running) ++samplesIntoStep_;

        // --- end-of-step handling: set up boxSlide_ for next step ---
        if (running && samplesIntoStep_ >= samplesPerStep_) {
            bNoteSlide_ = bSlideNext_;
            if (bNoteSlide_) {
                // Target and duration both come from the step that will
                // actually play next - across the bar line that is step 0 of
                // the next pattern (or of this one, when looping it), and its
                // length is that pattern's step length, which differs when one
                // of the two is in triplet feel.
                int nextPat, nextStep;
                nextPlayedStep(pats, fullSeq, patIdx, currentStep_, nextPat, nextStep);
                const CbPattern& np = patAt(nextPat);
                const double eNext = noteFreq(np.steps[nextStep].pitch);
                const int nextLen = stepLenFrom(
                    exactStepSamples(sampleRate_, fTempo, np.triplet),
                    stepCarry(stepExact, stepFrac_, samplesPerStep_));
                double tuned_curr = std::pow(2.0, std::log2(std::max(1e-6, boxOscFrq_)) + K.tune);
                double tuned_next = std::pow(2.0, std::log2(std::max(1e-6, eNext))     + K.tune);
                if (tuned_curr > 0.0)
                    boxSlide_ = std::exp(std::log(tuned_next / tuned_curr) / nextLen);
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
}
