/*
 * render.c - the sample-exact tester's render driver. Plain C17, and it
 * talks to the engine only through sb_api.h, so the same driver links
 * against the C++ engine today and against the C port later.
 *
 *   screambox_render cases <outdir> [--keep] [--only name,name,...]
 *   screambox_render file  <outdir> <song.roq|module.cb> [--keep]
 *
 * One line per case on stdout, "key=value" fields; tools/check_sound.py
 * compares them against tests/golden/sound.manifest. For every case:
 *
 *   hash     FNV-1a 64 of the render at 512 frames per block
 *   blocks   the same render at 32, 64 and 1024 frames per block is
 *            identical ("ok"), or the first block size and sample that differs
 *   roq      hash of the .roq the song saves as - pins the writer's bytes,
 *            the version it stamps included
 *   reload   that .roq read back and rendered gives the same hash
 *   resave   that .roq read back and saved again gives the same bytes
 *   hape     the same two checks for the sweeps-only .hape ("-" if no sweeps)
 *   allocs   heap allocations inside process(), invariant 3: must be 0
 *
 * --keep also writes <outdir>/<name>.f32, the raw 512-frame render
 * (interleaved stereo float32), which check_sound.py uses to say where a
 * mismatch starts.
 *
 * Every case is deterministic: the noise source has a fixed seed and the
 * random-pattern generator (time-seeded) is never used here.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "sb_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ util */

static uint64_t fnv1a(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static unsigned char* readFile(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc(len > 0 ? (size_t)len : 1);
    *n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    return buf;
}

static uint64_t hashFile(const char* path) {
    size_t n = 0;
    unsigned char* b = readFile(path, &n);
    if (!b) return 0;
    uint64_t h = fnv1a(b, n);
    free(b);
    return h;
}

static int sameFiles(const char* a, const char* b) {
    size_t na = 0, nb = 0;
    unsigned char* x = readFile(a, &na);
    unsigned char* y = readFile(b, &nb);
    int same = x && y && na == nb && memcmp(x, y, na) == 0;
    free(x); free(y);
    return same;
}

static SbSong* newSong(void) {
    SbSong* s = (SbSong*)malloc(sizeof(SbSong));
    if (!s) { fprintf(stderr, "out of memory\n"); exit(2); }
    sb_song_defaults(s);
    return s;
}

/* ---------------------------------------------------------------- render */

typedef struct Opt {
    double sr;
    int loop;   /* play pattern 0 only */
} Opt;

static long long frameCount(const SbSong* s, const Opt* o) {
    /* One pass and then pattern 0 again, so the wrap back to the start (the
     * per-pattern reset, a slide across the bar line, an oscillating sweep's
     * phase taken afresh from the position) is in every render. Loop mode:
     * pattern 0 three times. */
    SbSong* first = newSong();
    memcpy(first->params, s->params, sizeof s->params);
    first->pattern_count = 1;
    first->patterns[0] = s->patterns[0];
    const long long p0 = sb_song_length(first, o->sr);
    free(first);
    return o->loop ? 3 * p0 : sb_song_length(s, o->sr) + p0;
}

static float* renderSong(const SbSong* s, const Opt* o, long long frames, int block) {
    float* out = (float*)malloc((size_t)frames * 2 * sizeof(float));
    float* buf = (float*)malloc((size_t)block * 2 * sizeof(float));
    SbEngine* e = sb_engine_new(o->sr);
    sb_engine_load_song(e, s);
    sb_engine_start(e, o->loop);
    for (long long done = 0; done < frames; ) {
        const int n = (frames - done < block) ? (int)(frames - done) : block;
        sb_engine_process(e, buf, n);
        memcpy(out + done * 2, buf, (size_t)n * 2 * sizeof(float));
        done += n;
    }
    sb_engine_free(e);
    free(buf);
    return out;
}

static void layout(const SbSong* s, char* buf, size_t len) {
    size_t at = 0;
    buf[0] = '\0';
    for (int i = 0; i < s->pattern_count && at + 8 < len; ++i)
        at += (size_t)snprintf(buf + at, len - at, "%s%d%s", i ? "," : "",
                               s->patterns[i].last_step + 1, s->patterns[i].triplet ? "t" : "");
}

/* Renders, checks and prints one case. Returns 0 if a hard requirement failed. */
/* srcPath is the file the song came from, or NULL for a generated case. */
static int runCase(const char* name, const SbSong* s, const Opt* o, const char* outdir,
                   int keep, const char* srcPath) {
    const int isFile = srcPath != NULL;
    static const int kBlocks[3] = {32, 64, 1024};
    const long long frames = frameCount(s, o);
    const long allocs0 = sb_process_allocations();
    float* ref = renderSong(s, o, frames, 512);
    const size_t bytes = (size_t)frames * 2 * sizeof(float);
    const uint64_t hash = fnv1a(ref, bytes);
    int ok = 1;

    char blocks[64] = "ok";
    for (int b = 0; b < 3; ++b) {
        float* r = renderSong(s, o, frames, kBlocks[b]);
        if (memcmp(r, ref, bytes) != 0) {
            long long i = 0;
            while (i < frames * 2 && memcmp(&r[i], &ref[i], sizeof(float)) == 0) ++i;
            snprintf(blocks, sizeof blocks, "%d@%lld", kBlocks[b], i / 2);
            ok = 0;
            free(r);
            break;
        }
        free(r);
    }
    const long allocs = sb_process_allocations() - allocs0;
    if (allocs != 0) ok = 0;

    if (keep) {
        char p[1024];
        snprintf(p, sizeof p, "%s/%s.f32", outdir, name);
        FILE* f = fopen(p, "wb");
        if (f) { fwrite(ref, 1, bytes, f); fclose(f); }
    }
    free(ref);

    /* File round trips. A song read from a file is not re-hashed as "roq":
     * its own bytes were written by whatever build saved it. */
    char err[256] = "";
    char roqPath[1024], resavePath[1024];
    snprintf(roqPath, sizeof roqPath, "%s/%s.roq", outdir, name);
    snprintf(resavePath, sizeof resavePath, "%s/%s.resave.roq", outdir, name);
    char roq[32] = "-", reload[32] = "-", resave[32] = "-";
    if (!isFile) {
        if (!sb_song_save_roq(roqPath, s, err, sizeof err)) {
            fprintf(stderr, "%s: save .roq failed: %s\n", name, err);
            return 0;
        }
        snprintf(roq, sizeof roq, "%016llx", (unsigned long long)hashFile(roqPath));
    }
    const size_t srcLen = isFile ? strlen(srcPath) : 0;
    if (!isFile || (srcLen > 4 && strcmp(srcPath + srcLen - 4, ".roq") == 0)) {
        const char* src = isFile ? srcPath : roqPath;
        SbSong* back = newSong();
        if (!sb_song_load_roq(src, back, err, sizeof err)) {
            fprintf(stderr, "%s: load .roq failed: %s\n", name, err);
            free(back);
            return 0;
        }
        if (!isFile) {
            float* r = renderSong(back, o, frames, 512);
            const int same = fnv1a(r, bytes) == hash;
            free(r);
            snprintf(reload, sizeof reload, "%s", same ? "ok" : "differs");
            if (!same) ok = 0;
        }
        if (!sb_song_save_roq(resavePath, back, err, sizeof err)) {
            fprintf(stderr, "%s: resave .roq failed: %s\n", name, err);
            free(back);
            return 0;
        }
        snprintf(resave, sizeof resave, "%s", sameFiles(src, resavePath) ? "ok" : "differs");
        /* For a generated song this is a requirement. For a file written by
         * an older build it is recorded, and only a change in it fails. */
        if (!isFile && strcmp(resave, "ok") != 0) ok = 0;
        free(back);
    }

    char hape[32] = "-", haperesave[32] = "-";
    if (!isFile && s->sweep_count > 0) {
        char hp[1024], hp2[1024];
        snprintf(hp, sizeof hp, "%s/%s.hape", outdir, name);
        snprintf(hp2, sizeof hp2, "%s/%s.resave.hape", outdir, name);
        SbSong* back = newSong();
        if (!sb_song_save_hape(hp, s, err, sizeof err) ||
            !sb_song_load_hape(hp, back, err, sizeof err) ||
            !sb_song_save_hape(hp2, back, err, sizeof err)) {
            fprintf(stderr, "%s: .hape round trip failed: %s\n", name, err);
            free(back);
            return 0;
        }
        snprintf(hape, sizeof hape, "%016llx", (unsigned long long)hashFile(hp));
        const int same = sameFiles(hp, hp2) && back->sweep_count == s->sweep_count &&
            memcmp(back->sweeps, s->sweeps, (size_t)s->sweep_count * sizeof(SbSweep)) == 0;
        snprintf(haperesave, sizeof haperesave, "%s", same ? "ok" : "differs");
        if (!same) ok = 0;
        free(back);
    }

    char lay[512];
    layout(s, lay, sizeof lay);
    printf("%s %s sr=%.0f tempo=%.17g loop=%d pats=%s frames=%lld hash=%016llx blocks=%s "
           "roq=%s reload=%s resave=%s hape=%s haperesave=%s allocs=%ld\n",
           isFile ? "file" : "case", name, o->sr, (double)s->params[SB_TEMPO], o->loop, lay, frames,
           (unsigned long long)hash, blocks, roq, reload, resave, hape, haperesave, allocs);
    fflush(stdout);
    return ok;
}

/* ----------------------------------------------------------------- songs */

/* Two 16-step patterns: accent runs (squelch stacking), slides that are not
 * on a last step, rests, one reversed note, one reversed note entered by a
 * tie. Nothing in it changed meaning in the 2026-11-06 round (no pitch 52,
 * no last-step slide), so its renders mean the same thing in every build. */
static void basicSong(SbSong* s) {
    static const int pitches[16] = {13, 13, 25, 0, 20, 13, 18, 30, 13, 0, 22, 13, 27, 25, 13, 15};
    s->pattern_count = 2;
    for (int p = 0; p < 2; ++p) {
        SbPattern* pat = &s->patterns[p];
        memset(pat, 0, sizeof *pat);
        pat->last_step = 15;
        for (int i = 0; i < 16; ++i) {
            SbStep* st = &pat->steps[i];
            st->pitch  = pitches[(i + p * 3) % 16] + (p == 1 ? 7 : 0);
            st->accent = (i % 4 != 3) && (i < 8 || p == 1);
            st->slide  = (i % 5 == 3) && i != 15;
        }
        pat->steps[6].reverse = 1;                       /* plain reversed note */
        pat->steps[9].pitch = 24;                        /* make 9 a note ...   */
        pat->steps[8].slide = 1;                         /* ... tied from 8     */
        pat->steps[8].reverse = 1;                       /* reversed across the tie */
    }
    s->params[SB_DECAY]  = 0.3f;     /* fast enough that the envelope moves within a step */
    s->params[SB_ENVMOD] = 0.6f;
    s->params[SB_CUTOFF] = 0.3f;
    s->params[SB_RESONANCE] = 0.7f;
}

static SbSweep* addSweep(SbSong* s, int target, int sp, int ss, int sv, int ep, int es, int ev,
                         int hold) {
    SbSweep* w = &s->sweeps[s->sweep_count++];
    memset(w, 0, sizeof *w);
    w->target = target;
    w->start_pat = sp; w->start_step = ss; w->start_val = sv;
    w->end_pat = ep;   w->end_step = es;   w->end_val = ev;
    w->hold = hold;
    return w;
}

/* An oscillating sweep (.roq v15): depth sv..ev in the target's stored units. */
static SbSweep* addOsc(SbSong* s, int target, int sp, int ss, int sv, int ep, int es, int ev,
                       int hold, int shape, int hz, float rate, float phase) {
    SbSweep* w = addSweep(s, target, sp, ss, sv, ep, es, ev, hold);
    w->osc_shape = shape; w->osc_hz = hz; w->osc_rate = rate; w->osc_phase = phase;
    return w;
}

/* The PWM LFO as it is now: a triangle in Hz on each PWM target with a depth,
 * from the top of the song, holding - what loading a pre-v15 song makes of
 * the LFO's three values (foldPwmLfo in RoqIO.cpp). */
static void pwmLfo(SbSong* s, float rate, int mainPct, int dupPct) {
    const int lp = s->pattern_count - 1, ls = s->patterns[lp].last_step;
    if (mainPct > 0) addOsc(s, 8,  0, 0, mainPct, lp, ls, mainPct, 1, SB_OSC_TRIANGLE, 1, rate, 0.0f);
    if (dupPct > 0)  addOsc(s, 11, 0, 0, dupPct,  lp, ls, dupPct,  1, SB_OSC_TRIANGLE, 1, rate, 0.0f);
}

/* Every block on, so a sweep on any target is audible. */
static void richPatch(SbSong* s) {
    float* v = s->params;
    v[SB_WAVEFORM] = 1.0f;     v[SB_PWM] = 0.4f;          v[SB_SUB_LEVEL] = 0.3f;
    v[SB_DUPLET_WAVEFORM] = 1.0f; v[SB_DUPLET_PWM] = 0.6f; v[SB_DUPLET_DETUNE] = 0.25f;
    v[SB_DUPLET_LEVEL] = 0.5f; v[SB_NOISE_LEVEL] = 0.05f;
    v[SB_DIST_TYPE] = 1.0f;    v[SB_DIST_GAIN] = 0.4f;    v[SB_DIST_TONE] = 0.5f;
    v[SB_DIST_LEVEL] = 0.8f;   v[SB_COMP_THRESHOLD] = 0.4f; v[SB_COMP_DEPTH] = 0.5f;
    v[SB_RING_HZ] = 150.0f;    v[SB_RING_AMOUNT] = 0.3f;
    pwmLfo(s, 2.0f, 30, 20);   /* was PWM LFO rate 2 Hz, depths 0.3 and 0.2 */
}

/* A sweep range that is audible and legal for each target, in stored units. */
static void sweepRange(int k, int* a, int* b) {
    switch (k) {
        case 2: case 3: case 6: *a = -80;  *b = 80;     break;   /* bipolar main sliders */
        case 7:  *a = 20;    *b = 180;    break;   /* squelch x100 */
        case 12: *a = -700;  *b = 700;    break;   /* cents */
        case 22: *a = 2000;  *b = 200000; break;   /* hundredths of a Hz */
        default: *a = 10;    *b = 90;     break;   /* percent */
    }
}

typedef void (*Setup)(SbSong*, Opt*);

/* Main-window parameters, one at a time on the basic song. */
#define PARAM_CASE(fn, param, value) \
    static void fn(SbSong* s, Opt* o) { (void)o; basicSong(s); s->params[param] = (value); }
static void cStock(SbSong* s, Opt* o) { (void)o; basicSong(s); }
PARAM_CASE(cSaw,         SB_WAVEFORM, 0.0f)
PARAM_CASE(cCutoffLow,   SB_CUTOFF, 0.0f)
PARAM_CASE(cCutoffHigh,  SB_CUTOFF, 1.0f)
PARAM_CASE(cResoNeg,     SB_RESONANCE, -0.8f)
PARAM_CASE(cResoZero,    SB_RESONANCE, 0.0f)
PARAM_CASE(cEnvModNeg,   SB_ENVMOD, -0.7f)
PARAM_CASE(cEnvModMax,   SB_ENVMOD, 1.0f)
PARAM_CASE(cDecayFast,   SB_DECAY, 0.05f)
PARAM_CASE(cDecaySlow,   SB_DECAY, 1.0f)
PARAM_CASE(cAccentZero,  SB_ACCENT, 0.0f)
PARAM_CASE(cTuneDown,    SB_TUNING, -1.0f)
PARAM_CASE(cTuneUp,      SB_TUNING, 0.5f)
PARAM_CASE(cVolumeHalf,  SB_VOLUME, 0.5f)
PARAM_CASE(cSquelch0,    SB_ACCENT_SQUELCH, 0.0f)
PARAM_CASE(cSquelch2,    SB_ACCENT_SQUELCH, 2.0f)
PARAM_CASE(cPwmNarrow,   SB_PWM, 0.05f)
PARAM_CASE(cPwmWide,     SB_PWM, 0.95f)
PARAM_CASE(cSub,         SB_SUB_LEVEL, 0.6f)
PARAM_CASE(cNoise,       SB_NOISE_LEVEL, 0.2f)
static void cTempoFrac(SbSong* s, Opt* o) { basicSong(s); s->params[SB_TEMPO] = 133.7f; o->sr = 48000.0; }
PARAM_CASE(cTempoSlow,   SB_TEMPO, 60.0f)
PARAM_CASE(cTempoFast,   SB_TEMPO, 300.0f)
static void cResoBoost(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_RESO_BOOST] = 1.0f; s->params[SB_RESONANCE] = 1.0f;
}
static void cDupletSquare(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    s->params[SB_DUPLET_LEVEL] = 0.6f; s->params[SB_DUPLET_DETUNE] = 0.5f; s->params[SB_DUPLET_PWM] = 0.3f;
}
static void cDupletSaw(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    s->params[SB_DUPLET_LEVEL] = 0.6f; s->params[SB_DUPLET_DETUNE] = -0.07f; s->params[SB_DUPLET_WAVEFORM] = 0.0f;
}
static void lfo(SbSong* s, float rate, int mainPct, int dupPct) {
    basicSong(s);
    s->params[SB_DUPLET_LEVEL] = 0.5f;
    pwmLfo(s, rate, mainPct, dupPct);
}
static void cLfoSlow(SbSong* s, Opt* o) { (void)o; lfo(s, 0.05f, 100, 0); }
static void cLfoMid(SbSong* s, Opt* o)  { (void)o; lfo(s, 2.0f, 40, 60); }
static void cLfoFast(SbSong* s, Opt* o) { (void)o; lfo(s, 100.0f, 30, 30); }

/* Oscillating sweeps (TODO Y3), beyond the PWM LFO. */
static void mixed(SbSong* s);
/* Steps mode, per step: a triangle on Cutoff, 8 steps a cycle, the whole song. */
static void cOscCutoffSteps(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addOsc(s, 1, 0, 0, 25, 1, 15, 25, 1, SB_OSC_TRIANGLE, 0, 8.0f, 0.0f);
}
/* A straight sweep and an oscillating one on the same target: the wave rides
 * the ramp. And the depth fades in, 0 to 30. */
static void cOscOnRamp(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addSweep(s, 1, 0, 0, 10, 1, 15, 70, 0);
    addOsc(s, 1, 0, 0, 0, 1, 15, 30, 0, SB_OSC_SINE, 0, 4.0f, 0.0f);
}
/* Hz mode on a per-step target: vibrato-ish Tuning, square, with a start phase. */
static void cOscTuningHz(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addOsc(s, 6, 0, 0, 20, 1, 15, 20, 1, SB_OSC_SQUARE, 1, 3.3f, 0.25f);
}
/* Per sample, steps mode: saws on both PWMs across mixed and triplet patterns. */
static void cOscPwmSaws(SbSong* s, Opt* o) {
    mixed(s); o->sr = 48000.0;
    s->params[SB_DUPLET_LEVEL] = 0.5f;
    addOsc(s, 8,  0, 0, 40, 3, 22, 40, 1, SB_OSC_SAW_UP,   0, 3.0f, 0.0f);
    addOsc(s, 11, 1, 2, 10, 3, 10, 45, 0, SB_OSC_SAW_DOWN, 0, 5.5f, 0.5f);
}
/* Starting mid-song, ending without hold, on a dialog target and the ring. */
static void cOscStartsLate(SbSong* s, Opt* o) {
    (void)o; basicSong(s); richPatch(s);
    addOsc(s, 18, 0, 6, 30, 1, 9, 30, 0, SB_OSC_TRIANGLE, 1, 0.7f, 0.0f);
    addOsc(s, 22, 1, 0, 5000, 1, 15, 5000, 0, SB_OSC_SINE, 0, 2.0f, 0.0f);
}
/* Depth 0 adds nothing: check_sound.py's RELATIONS hold these equal to stock. */
static void cOscZeroCutoff(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addOsc(s, 1, 0, 0, 0, 1, 15, 0, 1, SB_OSC_TRIANGLE, 1, 5.0f, 0.1f);
}
static void cOscZeroPwm(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addOsc(s, 8, 0, 0, 0, 1, 15, 0, 1, SB_OSC_SQUARE, 0, 2.0f, 0.0f);
}
/* Loop mode in Hz: the phase is the position's, so every loop is the same. */
static void cOscLoopHz(SbSong* s, Opt* o) {
    basicSong(s); o->loop = 1;
    addOsc(s, 8, 0, 0, 45, 0, 15, 45, 1, SB_OSC_SINE, 1, 1.7f, 0.0f);
    addOsc(s, 0, 0, 0, 30, 0, 15, 30, 1, SB_OSC_TRIANGLE, 1, 2.9f, 0.0f);
}
static void dist(SbSong* s, float type) {
    basicSong(s);
    s->params[SB_DIST_TYPE] = type; s->params[SB_DIST_GAIN] = 0.6f;
    s->params[SB_DIST_TONE] = 0.3f; s->params[SB_DIST_LEVEL] = 0.8f;
}
static void cDistClassic(SbSong* s, Opt* o)  { (void)o; dist(s, 0.0f); }
static void cDistRat(SbSong* s, Opt* o)      { (void)o; dist(s, 1.0f); }
static void cDistScreamer(SbSong* s, Opt* o) { (void)o; dist(s, 2.0f); }
static void cDistPlus(SbSong* s, Opt* o)     { (void)o; dist(s, 3.0f); }
static void cDistFuzz(SbSong* s, Opt* o)     { (void)o; dist(s, 4.0f); }
static void cFuzzRingRests(SbSong* s, Opt* o) {
    /* TODO S: Fuzz used to leave DC at rest, which the ring turned into a tone. */
    (void)o; dist(s, 4.0f);
    s->params[SB_RING_HZ] = 300.0f; s->params[SB_RING_AMOUNT] = 1.0f;
    for (int i = 4; i < 12; ++i) s->patterns[0].steps[i].pitch = 0;
}
static void cCompLight(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_COMP_THRESHOLD] = 0.6f; s->params[SB_COMP_DEPTH] = 0.3f;
}
static void cCompHeavy(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_COMP_THRESHOLD] = 0.1f; s->params[SB_COMP_DEPTH] = 1.0f;
    s->params[SB_VOLUME] = 1.0f;
}
static void cRingLow(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_RING_HZ] = 5.0f; s->params[SB_RING_AMOUNT] = 0.5f;
}
static void cRingHigh(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_RING_HZ] = 2000.0f; s->params[SB_RING_AMOUNT] = 1.0f;
}
/* A straight sweep that does not change its value still runs the per-sample
 * glide (it is "moving": start and end are different steps). It must render
 * identical to the slider at that value - RELATIONS in check_sound.py hold
 * these equal to cutoff_high and ring_high. One main slider, one geometric. */
static void cGlideConstCutoff(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    addSweep(s, 1, 0, 0, 100, 1, 15, 100, 0);
}
static void cGlideConstRing(SbSong* s, Opt* o) {
    (void)o; basicSong(s); s->params[SB_RING_AMOUNT] = 1.0f;
    addSweep(s, 22, 0, 0, 200000, 1, 15, 200000, 0);
}
static void cRich(SbSong* s, Opt* o) { (void)o; basicSong(s); richPatch(s); }
static void cRich48k(SbSong* s, Opt* o) { cRich(s, o); o->sr = 48000.0; }
static void cRich96k(SbSong* s, Opt* o) { cRich(s, o); o->sr = 96000.0; }
static void cRich22k(SbSong* s, Opt* o) { cRich(s, o); o->sr = 22050.0; }
static void cStock48k(SbSong* s, Opt* o) { basicSong(s); o->sr = 48000.0; }
static void cLoop(SbSong* s, Opt* o) { basicSong(s); o->loop = 1; }

/* TODO Q: a slide on a pattern's last step glides into the next pattern. */
static void cLastSlide(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    s->patterns[0].steps[15].slide = 1;
    s->patterns[1].steps[15].slide = 1;
}
static void cLastSlideLoop(SbSong* s, Opt* o) { cLastSlide(s, o); o->loop = 1; }

/* TODO K / B / U: triplet and mixed lengths, at a rate where steps are fractional. */
static void mixed(SbSong* s) {
    basicSong(s);
    s->pattern_count = 4;
    s->patterns[2] = s->patterns[0];
    s->patterns[3] = s->patterns[1];
    s->patterns[0].last_step = 15;
    s->patterns[1].last_step = 11; s->patterns[1].triplet = 1;
    s->patterns[2].last_step = 3;
    s->patterns[3].last_step = 22;
    for (int i = 16; i < 23; ++i) s->patterns[3].steps[i] = s->patterns[3].steps[i - 16];
}
static void cMixed(SbSong* s, Opt* o) { (void)o; mixed(s); }
static void cMixed48k(SbSong* s, Opt* o) { mixed(s); o->sr = 48000.0; }
static void cLong64(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    s->pattern_count = 1;
    s->patterns[0].last_step = 63;
    for (int i = 16; i < 64; ++i) {
        s->patterns[0].steps[i] = s->patterns[1].steps[i % 16];
        s->patterns[0].steps[i].pitch = (s->patterns[0].steps[i].pitch + i) % 61;
    }
}

/* Every pitch, 52 included (TODO T), with rests between. */
static void cPitches(SbSong* s, Opt* o) {
    (void)o; basicSong(s);
    s->pattern_count = 4;
    for (int p = 0; p < 4; ++p) {
        SbPattern* pat = &s->patterns[p];
        memset(pat, 0, sizeof *pat);
        pat->last_step = 15;
        for (int i = 0; i < 16; ++i) {
            const int k = p * 16 + i;
            pat->steps[i].pitch = (k % 5 == 4) ? 0 : 1 + (k * 61 / 64) % 60;
            pat->steps[i].accent = (k % 3 == 0);
        }
    }
}

/* Sweeps. Each target ramps from pattern 0 step 2 to pattern 1 step 13. */
static int g_sweepTarget;
static void cSweep(SbSong* s, Opt* o) {
    (void)o; basicSong(s); richPatch(s);
    int a, b;
    sweepRange(g_sweepTarget, &a, &b);
    addSweep(s, g_sweepTarget, 0, 2, a, 1, 13, b, 0);
}
/* Two sweeps on Cutoff with a gap, the first holding, a third past the song's end. */
static void cSweepHold(SbSong* s, Opt* o) {
    (void)o; basicSong(s); richPatch(s);
    addSweep(s, 1, 0, 0, 10, 0, 7, 80, 1);
    addSweep(s, 1, 1, 4, 20, 1, 12, 60, 0);
    addSweep(s, 1, 7, 0, 0, 9, 15, 100, 0);            /* names patterns that do not exist */
    addSweep(s, 22, 0, 4, 1000, 0, 12, 50000, 1);      /* a geometric target, holding */
}
/* A sweep across patterns of different lengths advances evenly per step (TODO P). */
static void cSweepMixed(SbSong* s, Opt* o) {
    (void)o; mixed(s); richPatch(s);
    addSweep(s, 0, 0, 0, 0, 3, 22, 100, 0);
    addSweep(s, 12, 1, 0, -1200, 2, 3, 1200, 0);
}
/* Sweeps on the main seven and on the dialog targets at once, 48 kHz. */
static void cSweepAll(SbSong* s, Opt* o) {
    basicSong(s); richPatch(s); o->sr = 48000.0;
    for (int k = 0; k < SB_SWEEP_TARGETS; ++k) {
        int a, b;
        if (k >= 14 && k <= 16) continue;   /* retired with the PWM LFO */
        sweepRange(k, &a, &b);
        addSweep(s, k, 0, k % 8, a, 1, 8 + k % 8, b, k % 3 == 0);
    }
}

typedef struct Case { const char* name; Setup setup; int sweepTarget; } Case;

static const Case kCases[] = {
    {"stock", cStock, -1},             {"saw", cSaw, -1},
    {"cutoff_low", cCutoffLow, -1},    {"cutoff_high", cCutoffHigh, -1},
    {"reso_neg", cResoNeg, -1},        {"reso_zero", cResoZero, -1},
    {"envmod_neg", cEnvModNeg, -1},    {"envmod_max", cEnvModMax, -1},
    {"decay_fast", cDecayFast, -1},    {"decay_slow", cDecaySlow, -1},
    {"accent_zero", cAccentZero, -1},  {"tune_down", cTuneDown, -1},
    {"tune_up", cTuneUp, -1},          {"volume_half", cVolumeHalf, -1},
    {"squelch_0", cSquelch0, -1},      {"squelch_2", cSquelch2, -1},
    {"tempo_133_7_48k", cTempoFrac, -1}, {"tempo_60", cTempoSlow, -1},
    {"tempo_300", cTempoFast, -1},     {"stock_48k", cStock48k, -1},
    {"loop", cLoop, -1},               {"last_slide", cLastSlide, -1},
    {"last_slide_loop", cLastSlideLoop, -1},
    {"mixed_lengths", cMixed, -1},     {"mixed_lengths_48k", cMixed48k, -1},
    {"long64", cLong64, -1},           {"pitches", cPitches, -1},
    {"reso_boost", cResoBoost, -1},
    {"pwm_narrow", cPwmNarrow, -1},    {"pwm_wide", cPwmWide, -1},
    {"sub", cSub, -1},                 {"noise", cNoise, -1},
    {"duplet_square", cDupletSquare, -1}, {"duplet_saw", cDupletSaw, -1},
    {"lfo_slow", cLfoSlow, -1},        {"lfo_mid", cLfoMid, -1},
    {"lfo_fast", cLfoFast, -1},
    {"osc_cutoff_steps", cOscCutoffSteps, -1}, {"osc_on_ramp", cOscOnRamp, -1},
    {"osc_tuning_hz", cOscTuningHz, -1}, {"osc_pwm_saws_48k", cOscPwmSaws, -1},
    {"osc_starts_late", cOscStartsLate, -1}, {"osc_loop_hz", cOscLoopHz, -1},
    {"osc_zero_cutoff", cOscZeroCutoff, -1}, {"osc_zero_pwm", cOscZeroPwm, -1},
    {"glide_const_cutoff", cGlideConstCutoff, -1}, {"glide_const_ring", cGlideConstRing, -1},
    {"dist_classic", cDistClassic, -1}, {"dist_rat", cDistRat, -1},
    {"dist_screamer", cDistScreamer, -1}, {"dist_plus", cDistPlus, -1},
    {"dist_fuzz", cDistFuzz, -1},      {"fuzz_ring_rests", cFuzzRingRests, -1},
    {"comp_light", cCompLight, -1},    {"comp_heavy", cCompHeavy, -1},
    {"ring_low", cRingLow, -1},        {"ring_high", cRingHigh, -1},
    {"rich", cRich, -1},               {"rich_48k", cRich48k, -1},
    {"rich_96k", cRich96k, -1},        {"rich_22k", cRich22k, -1},
    {"sweep_hold", cSweepHold, -1},    {"sweep_mixed", cSweepMixed, -1},
    {"sweep_all_48k", cSweepAll, -1},
    /* one ramp per target, 0..23; 14..16 retired with the PWM LFO */
    {"sweep_t00", cSweep, 0},  {"sweep_t01", cSweep, 1},  {"sweep_t02", cSweep, 2},
    {"sweep_t03", cSweep, 3},  {"sweep_t04", cSweep, 4},  {"sweep_t05", cSweep, 5},
    {"sweep_t06", cSweep, 6},  {"sweep_t07", cSweep, 7},  {"sweep_t08", cSweep, 8},
    {"sweep_t09", cSweep, 9},  {"sweep_t10", cSweep, 10}, {"sweep_t11", cSweep, 11},
    {"sweep_t12", cSweep, 12}, {"sweep_t13", cSweep, 13},
    {"sweep_t17", cSweep, 17},
    {"sweep_t18", cSweep, 18}, {"sweep_t19", cSweep, 19}, {"sweep_t20", cSweep, 20},
    {"sweep_t21", cSweep, 21}, {"sweep_t22", cSweep, 22}, {"sweep_t23", cSweep, 23},
};

static int listed(const char* only, const char* name) {
    if (!only) return 1;
    const size_t n = strlen(name);
    for (const char* p = only; (p = strstr(p, name)) != NULL; p += n)
        if ((p == only || p[-1] == ',') && (p[n] == ',' || p[n] == '\0')) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s cases <outdir> [--keep] [--only a,b]\n"
                        "       %s file <outdir> <song.roq|module.cb> [--keep]\n", argv[0], argv[0]);
        return 2;
    }
    int keep = 0;
    const char* only = NULL;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) only = argv[++i];
    }
    int failures = 0;
    if (strcmp(argv[1], "cases") == 0) {
        for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; ++c) {
            if (!listed(only, kCases[c].name)) continue;
            SbSong* s = newSong();
            Opt o = {44100.0, 0};
            g_sweepTarget = kCases[c].sweepTarget;
            kCases[c].setup(s, &o);
            if (!runCase(kCases[c].name, s, &o, argv[2], keep, NULL)) ++failures;
            free(s);
        }
    } else if (strcmp(argv[1], "file") == 0 && argc >= 4) {
        const char* path = argv[3];
        char err[256] = "";
        SbSong* s = newSong();
        const size_t n = strlen(path);
        const int isCb = n > 3 && (strcmp(path + n - 3, ".cb") == 0 || strcmp(path + n - 3, ".CB") == 0);
        if (!(isCb ? sb_song_load_cb(path, s, err, sizeof err)
                   : sb_song_load_roq(path, s, err, sizeof err))) {
            fprintf(stderr, "%s: %s\n", path, err);
            return 2;
        }
        /* The path as given, made safe to use as an output name and as one
         * space-free token on the result line. The whole path, not its last
         * part: modules/x.cb and modules/time for acid/x.cb must not collide. */
        char name[256];
        const char* base = path;
        size_t k = 0;
        for (; base[k] && k + 1 < sizeof name; ++k) {
            const char ch = base[k];
            const int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
            name[k] = safe ? ch : '_';
        }
        name[k] = '\0';
        Opt o = {44100.0, 0};
        if (!runCase(name, s, &o, argv[2], keep, path)) ++failures;
        free(s);
    } else {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        return 2;
    }
    return failures ? 1 : 0;
}
