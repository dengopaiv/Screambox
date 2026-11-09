// sb_api_cpp.cpp - sb_api.h implemented over the C++ engine and file code.
//
// Deliberately thin: every function hands straight to the code the program
// itself runs. The one piece that is not shared is sb_engine_load_song(),
// which repeats what MainWindow::loadRoqFile() does to the engine (every
// parameter, the patterns, the sweeps through setParamSweeps). If Open ever
// starts doing more to the engine, this has to follow.
#include "sb_api.h"

#include "CBEngine.h"
#include "CbPresetIO.h"
#include "RoqIO.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// The C header's numbering must be CbParam's, value for value.
#define SB_SAME(sb, cb) static_assert((int)(sb) == (int)CbParam::cb, #sb " != CbParam::" #cb)
SB_SAME(SB_VOLUME, Volume);                 SB_SAME(SB_WAVEFORM, Waveform);
SB_SAME(SB_TUNING, Tuning);                 SB_SAME(SB_CUTOFF, Cutoff);
SB_SAME(SB_RESONANCE, Resonance);           SB_SAME(SB_ENVMOD, EnvMod);
SB_SAME(SB_DECAY, Decay);                   SB_SAME(SB_ACCENT, Accent);
SB_SAME(SB_TEMPO, Tempo);                   SB_SAME(SB_RUNSTOP, RunStop);
SB_SAME(SB_ACCENT_SQUELCH, AccentSquelch);  SB_SAME(SB_PWM, PWM);
SB_SAME(SB_SUB_LEVEL, SubOscLevel);         SB_SAME(SB_DUPLET_WAVEFORM, DupletWaveform);
SB_SAME(SB_DUPLET_PWM, DupletPWM);          SB_SAME(SB_DUPLET_DETUNE, DupletDetune);
SB_SAME(SB_DUPLET_LEVEL, DupletLevel);      SB_SAME(SB_PWM_LFO_RATE, PwmLfoRate);
SB_SAME(SB_PWM_LFO_DEPTH, PwmLfoDepth);     SB_SAME(SB_PWM_LFO_DUPLET_DEPTH, PwmLfoDupletDepth);
SB_SAME(SB_NOISE_LEVEL, NoiseLevel);        SB_SAME(SB_RESO_BOOST, ResoBoost);
SB_SAME(SB_DIST_TYPE, DistType);            SB_SAME(SB_DIST_GAIN, DistGain);
SB_SAME(SB_DIST_TONE, DistTone);            SB_SAME(SB_DIST_LEVEL, DistLevel);
SB_SAME(SB_COMP_THRESHOLD, CompThreshold);  SB_SAME(SB_COMP_DEPTH, CompDepth);
SB_SAME(SB_RING_HZ, RingHz);                SB_SAME(SB_RING_AMOUNT, RingAmount);
SB_SAME(SB_NUM_PARAMS, NumParams);
static_assert(SB_SWEEP_TARGETS == CBEngine::kSweepTargets, "sweep target count");

// ---- allocation counter, armed only inside sb_engine_process() ----
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocs{0};
void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

void setErr(char* err, int errlen, const std::string& msg) {
    if (err && errlen > 0) std::snprintf(err, (size_t)errlen, "%s", msg.c_str());
}

CbPattern toCb(const SbPattern& p) {
    CbPattern c;
    c.lastStep = p.last_step;
    c.triplet  = p.triplet != 0;
    for (int i = 0; i < SB_MAX_STEPS; ++i) {
        c.steps[i].pitch       = p.steps[i].pitch;
        c.steps[i].slide       = p.steps[i].slide != 0;
        c.steps[i].accent      = p.steps[i].accent != 0;
        c.steps[i].reverseStep = p.steps[i].reverse != 0;
    }
    return c;
}

SbPattern fromCb(const CbPattern& c) {
    SbPattern p{};
    p.last_step = c.lastStep;
    p.triplet   = c.triplet ? 1 : 0;
    for (int i = 0; i < SB_MAX_STEPS; ++i) {
        p.steps[i].pitch   = c.steps[i].pitch;
        p.steps[i].slide   = c.steps[i].slide ? 1 : 0;
        p.steps[i].accent  = c.steps[i].accent ? 1 : 0;
        p.steps[i].reverse = c.steps[i].reverseStep ? 1 : 0;
    }
    return p;
}

std::vector<CbPattern> patternsOf(const SbSong* s) {
    std::vector<CbPattern> v;
    for (int i = 0; i < s->pattern_count; ++i) v.push_back(toCb(s->patterns[i]));
    return v;
}

RoqSweep toRoq(const SbSweep& w) {
    RoqSweep r;
    r.sliderIdx = w.target;
    r.startPat = w.start_pat; r.startStep = w.start_step; r.startVal = w.start_val;
    r.endPat   = w.end_pat;   r.endStep   = w.end_step;   r.endVal   = w.end_val;
    r.hold     = w.hold != 0;
    return r;
}

SbSweep fromRoq(const RoqSweep& r) {
    SbSweep w{};
    w.target = r.sliderIdx;
    w.start_pat = r.startPat; w.start_step = r.startStep; w.start_val = r.startVal;
    w.end_pat   = r.endPat;   w.end_step   = r.endStep;   w.end_val   = r.endVal;
    w.hold      = r.hold ? 1 : 0;
    return w;
}

bool setSweeps(SbSong* s, const std::vector<RoqSweep>& v, char* err, int errlen) {
    if (v.size() > (size_t)SB_MAX_SWEEPS) { setErr(err, errlen, "too many sweeps"); return false; }
    s->sweep_count = (int)v.size();
    for (size_t i = 0; i < v.size(); ++i) s->sweeps[i] = fromRoq(v[i]);
    return true;
}

bool setPatterns(SbSong* s, const std::vector<CbPattern>& v, char* err, int errlen) {
    if (v.size() > (size_t)SB_MAX_PATTERNS) { setErr(err, errlen, "too many patterns"); return false; }
    s->pattern_count = (int)v.size();
    for (size_t i = 0; i < v.size(); ++i) s->patterns[i] = fromCb(v[i]);
    return true;
}

// RoqSong field <-> parameter. One table, used both ways.
struct FieldMap { float RoqSong::*field; int param; };
const FieldMap kFields[] = {
    {&RoqSong::volume, SB_VOLUME},               {&RoqSong::waveform, SB_WAVEFORM},
    {&RoqSong::tuning, SB_TUNING},               {&RoqSong::cutoff, SB_CUTOFF},
    {&RoqSong::resonance, SB_RESONANCE},         {&RoqSong::envmod, SB_ENVMOD},
    {&RoqSong::decay, SB_DECAY},                 {&RoqSong::accent, SB_ACCENT},
    {&RoqSong::tempo, SB_TEMPO},                 {&RoqSong::accentSquelch, SB_ACCENT_SQUELCH},
    {&RoqSong::pwm, SB_PWM},                     {&RoqSong::subOscLevel, SB_SUB_LEVEL},
    {&RoqSong::dupletWaveform, SB_DUPLET_WAVEFORM}, {&RoqSong::dupletPWM, SB_DUPLET_PWM},
    {&RoqSong::dupletDetune, SB_DUPLET_DETUNE},  {&RoqSong::dupletLevel, SB_DUPLET_LEVEL},
    {&RoqSong::pwmLfoRate, SB_PWM_LFO_RATE},     {&RoqSong::pwmLfoDepth, SB_PWM_LFO_DEPTH},
    {&RoqSong::pwmLfoDupletDepth, SB_PWM_LFO_DUPLET_DEPTH},
    {&RoqSong::noiseLevel, SB_NOISE_LEVEL},      {&RoqSong::resoBoost, SB_RESO_BOOST},
    {&RoqSong::distType, SB_DIST_TYPE},          {&RoqSong::distGain, SB_DIST_GAIN},
    {&RoqSong::distTone, SB_DIST_TONE},          {&RoqSong::distLevel, SB_DIST_LEVEL},
    {&RoqSong::compThreshold, SB_COMP_THRESHOLD}, {&RoqSong::compDepth, SB_COMP_DEPTH},
    {&RoqSong::ringHz, SB_RING_HZ},              {&RoqSong::ringAmount, SB_RING_AMOUNT},
};

} // namespace

struct SbEngine {
    CBEngine eng;
};

extern "C" {

void sb_song_defaults(SbSong* s) {
    std::memset(s, 0, sizeof *s);
    CBEngine fresh;
    for (int i = 0; i < SB_NUM_PARAMS; ++i) s->params[i] = fresh.getParam(CbParam(i));
}

int sb_song_load_roq(const char* path, SbSong* s, char* err, int errlen) {
    std::string e;
    auto song = loadRoq(path, e);
    if (!song) { setErr(err, errlen, e); return 0; }
    sb_song_defaults(s);
    for (const auto& f : kFields) s->params[f.param] = (*song).*(f.field);
    if (!setPatterns(s, song->patterns, err, errlen)) return 0;
    return setSweeps(s, song->sweeps, err, errlen) ? 1 : 0;
}

int sb_song_save_roq(const char* path, const SbSong* s, char* err, int errlen) {
    RoqSong song;
    for (const auto& f : kFields) song.*(f.field) = s->params[f.param];
    song.patterns = patternsOf(s);
    for (int i = 0; i < s->sweep_count; ++i) song.sweeps.push_back(toRoq(s->sweeps[i]));
    std::string e;
    if (!saveRoq(path, song, e)) { setErr(err, errlen, e); return 0; }
    return 1;
}

int sb_song_load_cb(const char* path, SbSong* s, char* err, int errlen) {
    std::string e;
    auto preset = loadCb(path, e);
    if (!preset) { setErr(err, errlen, e); return 0; }
    sb_song_defaults(s);
    s->params[SB_TEMPO] = preset->tempo;
    return setPatterns(s, preset->patterns, err, errlen) ? 1 : 0;
}

int sb_song_load_hape(const char* path, SbSong* s, char* err, int errlen) {
    std::string e;
    auto sweeps = loadHape(path, e);
    if (!sweeps) { setErr(err, errlen, e); return 0; }
    return setSweeps(s, *sweeps, err, errlen) ? 1 : 0;
}

int sb_song_save_hape(const char* path, const SbSong* s, char* err, int errlen) {
    std::vector<RoqSweep> v;
    for (int i = 0; i < s->sweep_count; ++i) v.push_back(toRoq(s->sweeps[i]));
    std::string e;
    if (!saveHape(path, v, e)) { setErr(err, errlen, e); return 0; }
    return 1;
}

long long sb_song_length(const SbSong* s, double sample_rate) {
    return CBEngine::songLengthSamples(patternsOf(s), sample_rate, s->params[SB_TEMPO]);
}

SbEngine* sb_engine_new(double sample_rate) {
    SbEngine* e = new SbEngine;
    e->eng.setSampleRate(sample_rate);
    return e;
}

void sb_engine_free(SbEngine* e) { delete e; }

void sb_engine_load_song(SbEngine* e, const SbSong* s) {
    // As MainWindow::loadRoqFile(): patterns first, then every parameter but
    // RunStop, then the sweeps bucketed per target.
    if (s->pattern_count > 0) e->eng.setPatterns(patternsOf(s));
    for (int i = 0; i < SB_NUM_PARAMS; ++i)
        if (i != SB_RUNSTOP) e->eng.setParam(CbParam(i), s->params[i]);
    std::vector<CBEngine::ParamSweep> lists[CBEngine::kSweepTargets];
    for (int i = 0; i < s->sweep_count; ++i) {
        const SbSweep& w = s->sweeps[i];
        if (w.target < 0 || w.target >= CBEngine::kSweepTargets) continue;
        CBEngine::ParamSweep p;
        p.startPat = w.start_pat; p.startStep = w.start_step; p.startVal = w.start_val;
        p.endPat   = w.end_pat;   p.endStep   = w.end_step;   p.endVal   = w.end_val;
        p.hold     = w.hold != 0;
        lists[w.target].push_back(p);
    }
    for (int k = 0; k < CBEngine::kSweepTargets; ++k) e->eng.setParamSweeps(k, lists[k]);
}

void sb_engine_start(SbEngine* e, int loop_pattern) {
    e->eng.setPlayMode(loop_pattern ? CBEngine::PlayMode::CurrentPatternOnly
                                    : CBEngine::PlayMode::FullSequence);
    e->eng.setRunning(true, true);
}

void sb_engine_process(SbEngine* e, float* out, int frames) {
    g_armed.store(true, std::memory_order_relaxed);
    e->eng.process(out, frames);
    g_armed.store(false, std::memory_order_relaxed);
}

long sb_process_allocations(void) { return g_allocs.load(); }

} // extern "C"
