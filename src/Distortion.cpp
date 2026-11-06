#include "Distortion.h"

#include <cmath>

namespace {

// One-pole coefficient for a cutoff in Hz. Called per block, never per sample.
double onePole(double fc, double sampleRate) {
    if (fc <= 0.0)                 return 0.0;   // no filtering
    if (fc >= sampleRate * 0.45)   return 0.0;
    const double a = std::exp(-6.28318530717958647692 * fc / sampleRate);
    return (a < 0.0) ? 0.0 : (a > 0.9999 ? 0.9999 : a);
}

// Exponential map so a linear slider feels right across a wide gain range.
double expMap(double t, double lo, double hi) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return lo * std::pow(hi / lo, t);
}

inline double hardClip(double x, double lim) {
    return (x > lim) ? lim : (x < -lim ? -lim : x);
}

// Bounded soft saturator. Rational rather than tanh: same shape in the region
// that matters, no transcendental per sample, and it cannot overflow.
inline double softClip(double x) {
    return x / (1.0 + std::fabs(x));
}

} // namespace

void cbDistPrepare(CbDistCoeffs& c, int type, double gain, double tone,
                   double level, double sampleRate) {
    if (type < 0 || type >= int(CbDistType::Count)) type = 0;
    c.type = type;
    if (sampleRate < 1000.0) sampleRate = 44100.0;

    // Tone runs the conventional way round — up is brighter. A real RAT's
    // Filter knob is wired backwards (clockwise cuts treble), which is a famous
    // piece of trivia and a bad idea in a control that announces itself to a
    // screen reader, so the mapping is inverted here instead.
    switch (CbDistType(type)) {
        case CbDistType::Rat:
            // Slew-limited op-amp ahead of the diodes, then the passive
            // low-pass Filter sweeping roughly 475 Hz upward.
            c.drive   = expMap(gain, 1.0, 60.0);
            c.preLpA  = onePole(3000.0, sampleRate);
            c.postLpA = onePole(expMap(tone, 475.0, 12000.0), sampleRate);
            c.hpA     = 0.0;
            c.bias    = 0.0;
            c.level   = level;
            break;

        case CbDistType::Screamer:
            // Bass is kept out of the clipper, which is what stops a Tube
            // Screamer turning a bassline to mud, and it is why the mids push.
            c.drive   = expMap(gain, 1.0, 30.0);
            c.preLpA  = 0.0;
            c.postLpA = onePole(expMap(tone, 900.0, 16000.0), sampleRate);
            c.hpA     = onePole(720.0, sampleRate);
            c.bias    = 0.0;
            c.level   = level;
            break;

        case CbDistType::DistPlus:
            // Hard clipping with no slew limiting: brighter and rawer than the
            // RAT, which is the whole point of having both.
            c.drive   = expMap(gain, 1.0, 40.0);
            c.preLpA  = 0.0;
            c.postLpA = onePole(expMap(tone, 1200.0, 18000.0), sampleRate);
            c.hpA     = 0.0;
            c.bias    = 0.0;
            c.level   = level;
            break;

        case CbDistType::Fuzz:
            // Very high gain into an asymmetric clip. The asymmetry is what
            // produces even harmonics and the germanium-ish snarl; the stock
            // 2004 clipper is asymmetric too (+0.8 / -0.78), for the same
            // reason, so this is in keeping with the instrument.
            c.drive   = expMap(gain, 2.0, 250.0);
            c.preLpA  = 0.0;
            c.postLpA = onePole(expMap(tone, 700.0, 9000.0), sampleRate);
            c.hpA     = onePole(180.0, sampleRate);
            c.bias    = 0.18;
            c.level   = level;
            break;

        case CbDistType::Classic:
        default:
            c.drive = 1.0; c.preLpA = 0.0; c.postLpA = 0.0;
            c.hpA = 0.0; c.bias = 0.0; c.level = 1.0;
            break;
    }
}

double cbDistProcess(const CbDistCoeffs& c, CbDistState& s, double x) {
    double v = x;

    // Bass out of the clipper (Screamer, Fuzz): one-pole high-pass as
    // input-minus-lowpass.
    if (c.hpA > 0.0) {
        s.hpZ = x + c.hpA * (s.hpZ - x);
        v = x - s.hpZ;
    }

    // Slew limiting ahead of the diodes (RAT).
    if (c.preLpA > 0.0) {
        s.preLp = v + c.preLpA * (s.preLp - v);
        v = s.preLp;
    }

    v *= c.drive;

    switch (CbDistType(c.type)) {
        case CbDistType::Rat:      v = hardClip(v, 0.6); break;
        case CbDistType::DistPlus: v = hardClip(v, 0.7); break;
        case CbDistType::Screamer: v = softClip(v) * 0.9; break;
        case CbDistType::Fuzz:
            // Subtract what the clipper outputs for SILENCE, hardClip(bias),
            // not half the bias. The old `- bias * 0.5` left +0.09 of DC on
            // every rest - harmless alone, but the ring modulator after this
            // multiplies it into a carrier tone that played through the rests,
            // louder than the notes. Same waveform shape, same asymmetry
            // (clips at +0.37 / -0.73); only the constant offset is gone, and
            // with no filter state there is nothing for the per-pattern reset
            // to turn into a thump.
            v = hardClip(v + c.bias, 0.55) - hardClip(c.bias, 0.55);
            break;
        default: break;
    }

    // Tone: post-clip low-pass, the RAT's Filter with the sense corrected.
    if (c.postLpA > 0.0) {
        s.postLp = v + c.postLpA * (s.postLp - v);
        v = s.postLp;
    }

    v *= c.level;

    // Clipping stages can only produce bounded output, but the filters carry
    // state, and a denormal or a bad sample rate must not be able to poison it
    // permanently — the same reasoning as the resonant filter's NaN guard.
    if (!(v == v)) { s.reset(); return 0.0; }
    return (v > 1.0) ? 1.0 : (v < -1.0 ? -1.0 : v);
}
