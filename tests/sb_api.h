/*
 * sb_api.h - the boundary the sample-exact tester talks to.
 *
 * Plain C17 on purpose. Today it is implemented by a thin C++ shim over
 * CBEngine, RoqIO and CbPresetIO (sb_api_cpp.cpp). The planned C port will
 * implement this same header natively. The render driver (render.c) is
 * written in C against nothing but this file, so the one driver binary can be
 * linked against either engine, and "the sound did not change" means the two
 * builds print the same hashes.
 *
 * A song is plain data (SbSong) rather than a sequence of calls on an engine,
 * so the driver can also save it as .roq / .hape, read it back and check that
 * the file round trip changes nothing either.
 */
#ifndef SB_API_H
#define SB_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors enum class CbParam, in the same order. The shim static_asserts each
 * value, so a reordered CbParam fails to compile instead of renumbering every
 * test case without anyone noticing. */
enum {
    SB_VOLUME, SB_WAVEFORM, SB_TUNING, SB_CUTOFF, SB_RESONANCE, SB_ENVMOD,
    SB_DECAY, SB_ACCENT, SB_TEMPO, SB_RUNSTOP, SB_ACCENT_SQUELCH,
    SB_PWM, SB_SUB_LEVEL, SB_DUPLET_WAVEFORM, SB_DUPLET_PWM, SB_DUPLET_DETUNE,
    SB_DUPLET_LEVEL, SB_PWM_LFO_RATE, SB_PWM_LFO_DEPTH, SB_PWM_LFO_DUPLET_DEPTH,
    SB_NOISE_LEVEL, SB_RESO_BOOST, SB_DIST_TYPE, SB_DIST_GAIN, SB_DIST_TONE,
    SB_DIST_LEVEL, SB_COMP_THRESHOLD, SB_COMP_DEPTH, SB_RING_HZ, SB_RING_AMOUNT,
    SB_NUM_PARAMS
};

enum { SB_MAX_STEPS = 64, SB_MAX_PATTERNS = 256, SB_MAX_SWEEPS = 1024, SB_SWEEP_TARGETS = 24 };

typedef struct SbStep {
    int pitch;      /* 0 = rest, 1..60 */
    int slide, accent, reverse;
} SbStep;

typedef struct SbPattern {
    int last_step;  /* 0..63 */
    int triplet;
    SbStep steps[SB_MAX_STEPS];
} SbPattern;

/* Oscillating-sweep shapes (CBEngine::OscShape). */
enum { SB_OSC_NONE, SB_OSC_TRIANGLE, SB_OSC_SINE, SB_OSC_SQUARE, SB_OSC_SAW_UP, SB_OSC_SAW_DOWN };

/* A sweep as .roq stores it: target 0..23, values in the target's stored units.
 * With osc_shape set it oscillates (.roq v15) and the two values are its depth;
 * osc_hz says whether osc_rate is in Hz or in steps per cycle, and osc_phase
 * is the start phase in cycles. */
typedef struct SbSweep {
    int target;
    int start_pat, start_step, start_val;
    int end_pat, end_step, end_val;
    int hold;
    int osc_shape, osc_hz;
    float osc_rate, osc_phase;
    int osc_between;   /* 1: swing between start_val and end_val, not around */
} SbSweep;

typedef struct SbSong {
    float params[SB_NUM_PARAMS];
    int pattern_count;
    SbPattern patterns[SB_MAX_PATTERNS];
    int sweep_count;
    SbSweep sweeps[SB_MAX_SWEEPS];
} SbSong;   /* large: allocate it, do not put it on the stack */

/* A fresh engine's parameters, no patterns, no sweeps. */
void sb_song_defaults(SbSong* s);

/* Files. Each returns 1 on success, 0 on failure with a message in err. A .cb
 * import brings patterns and tempo only; the 2002 sweeps are inert, as in the
 * program. A .hape load replaces s->sweeps and leaves the rest alone. A .roq
 * load folds a pre-v15 PWM LFO into oscillating sweeps, as the program does;
 * SB_PWM_LFO_* are read by nothing else. */
int sb_song_load_roq(const char* path, SbSong* s, char* err, int errlen);
int sb_song_save_roq(const char* path, const SbSong* s, char* err, int errlen);
int sb_song_load_cb(const char* path, SbSong* s, char* err, int errlen);
int sb_song_load_hape(const char* path, SbSong* s, char* err, int errlen);
int sb_song_save_hape(const char* path, const SbSong* s, char* err, int errlen);

/* Samples one pass of the song takes, exactly as the sequencer counts them. */
long long sb_song_length(const SbSong* s, double sample_rate);

typedef struct SbEngine SbEngine;
SbEngine* sb_engine_new(double sample_rate);
void      sb_engine_free(SbEngine* e);
/* Everything the program does on Open: parameters, patterns, sweeps. */
void      sb_engine_load_song(SbEngine* e, const SbSong* s);
/* Starts from the first pattern; loop_pattern = 1 plays only that pattern. */
void      sb_engine_start(SbEngine* e, int loop_pattern);
/* Interleaved stereo float32, frames frames. */
void      sb_engine_process(SbEngine* e, float* out, int frames);

/* Heap allocations made inside sb_engine_process() since the program started.
 * Invariant 3 says this stays 0. */
long sb_process_allocations(void);

#ifdef __cplusplus
}
#endif
#endif
