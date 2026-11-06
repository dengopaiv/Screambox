#pragma once

#include "CBEngine.h"
#include <optional>
#include <string>
#include <vector>

// Creakbox native file formats.
//
// .roq — "song" file (Estonian *röök* = roar). Line-delimited text. Bundles
//        patterns, slider values, and every sweep the user has drawn.
// .hape — sweeps-only export for re-using a sweep collection across songs.
//
// Both formats are plain ASCII so they can be hand-inspected and diffed.
// Version is bumped only on breaking schema changes; readers must tolerate
// unknown trailing lines so older Creakbox builds don't choke on new fields.

struct RoqSweep {
    int sliderIdx = 0;      // 0=Volume 1=Cutoff 2=Resonance 3=EnvMod 4=Decay 5=Accent 6=Tuning
    int startPat = 0, startStep = 0, startVal = 0;
    int endPat   = 0, endStep   = 15, endVal  = 0;
    // Keep the end value after the sweep ends, until the next sweep on the
    // same slider starts. v13; an eighth token on the sweep line.
    bool hold = false;
};

struct RoqSong {
    // Slider values in UI order: Volume, Waveform, Tuning, Cutoff, Resonance,
    // EnvMod, Decay, Accent, Tempo. Stored as raw param values (0..1 or -1..+1).
    float volume    = 1.0f;
    float waveform  = 1.0f;   // 0 = saw, 1 = square
    float tuning    = 0.0f;
    float cutoff    = 0.15f;
    float resonance = 1.0f;
    float envmod    = 0.10f;
    float decay     = 0.90f;
    float accent    = 1.0f;
    float tempo     = 140.0f;
    float accentSquelch = 1.0f; // Residual-stacking amount. 1.0 = Frank's original; 0 = no buildup; >1 = squelchier consecutive accents.
    float pwm = 0.5f;           // Square duty-cycle (0.5 = 50 %, stock 303).
    float subOscLevel = 0.0f;   // Sine sub-oscillator level (octave below).
    float dupletWaveform = 1.0f;// 0 = saw, 1 = square for the duplet voice.
    float dupletPWM = 0.5f;     // Duplet square duty-cycle (independent of main PWM).
    float dupletDetune = 0.0f;  // -1..+1 = ±1200 cents detune.
    float dupletLevel = 0.0f;   // Duplet mix level.
    // Shared triangle PWM LFO: one rate, one depth per destination.
    // Rate is stored in Hz, never as a slider position, so the UI's
    // exponential taper can change later without repitching saved songs.
    float pwmLfoRate = 0.0f;        // Hz, 0..100. 0 = off.
    float pwmLfoDepth = 0.0f;       // 0..1 into the main PWM.
    float pwmLfoDupletDepth = 0.0f; // 0..1 into the duplet PWM.
    float noiseLevel = 0.0f;        // 0..1 white noise into the oscillator sum.
    float resoBoost  = 0.0f;        // 0/1 Devilfish-style resonance boost.
    // Distortion block. Type 0 is Classic — the stock 2004 dist_dist() — and
    // the other three values are inert while it is selected, so a default
    // song loads and sounds exactly as it did before the block existed.
    float distType  = 0.0f;         // 0 Classic, 1 Rat, 2 Screamer, 3 Dist+, 4 Fuzz.
    float distGain  = 0.0f;         // 0..1 drive into the clipper.
    float distTone  = 0.5f;         // 0..1 post-clip lowpass, up = brighter.
    float distLevel = 1.0f;         // 0..1 output level after the clipper.
    // The rest of the acid FX block (v11). Defaults are "off", so a v10 or
    // earlier song loads sounding exactly as it did.
    float compThreshold = 0.5f;     // 0..1 where compression starts.
    float compDepth     = 0.0f;     // 0..1 amount; 0 = compressor off.
    float ringHz        = 100.0f;   // carrier frequency in Hz, stored directly.
    float ringAmount    = 0.0f;     // 0..1 dry/ring blend; 0 = ring off.

    // --- legacy ------------------------------------------------------------
    // Not a parameter. `impulsive` selected a "clean" accent mode in the 2002
    // build that caused clicks; the engine has always used Frank's TB-303
    // buildup since, and AccentSquelch replaced it as the control.
    //
    // It is still READ (v2 and later files have the line) and still WRITTEN as
    // a constant 0, and the write is not optional. `.roq` is a positional
    // format - one value per line, in a fixed order - so dropping the line
    // would shift every field after it by one and an older reader would parse
    // `accentSquelch` as this and mis-read the whole rest of the header. The
    // zero is padding that has to stay, not a value anyone should consult.
    //
    // Surveyed 2026-09-04 across all 25 shipped modules: two are v1 and predate
    // the field, the other 23 all store 0. It has never carried anything else.
    float impulsive = 0.0f;     // DEPRECATED - see above. Never read by anything.

    std::vector<CbPattern> patterns;
    std::vector<RoqSweep>  sweeps;
};

std::optional<RoqSong> loadRoq(const std::string& path, std::string& error);
bool                   saveRoq(const std::string& path, const RoqSong& song, std::string& error);

std::optional<std::vector<RoqSweep>> loadHape(const std::string& path, std::string& error);
bool                                  saveHape(const std::string& path,
                                               const std::vector<RoqSweep>& sweeps,
                                               std::string& error);
