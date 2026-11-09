#include "RoqIO.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// The program was called Creakbox when these were chosen. They stay: a reader
// checks them before anything else, so renaming them would make every existing
// .roq and .hape "not a .roq file".
constexpr const char* kRoqMagic  = "CreakboxRoq";
constexpr const char* kHapeMagic = "CreakboxHape";
// v1: patterns + 9 params + sweeps.
// v2: adds `impulsive` flag after tempo (long deprecated; still written as a
//     constant 0 because the format is positional - see RoqIO.h).
// v3: adds `accentSquelch` (0..2) after impulsive — the buildup-amount knob.
// v4: adds oscillator-dialog fields: pwm, subOscLevel, dupletWaveform,
//     dupletDetune, dupletLevel (in that order, after accentSquelch).
// v5: dupletDetune range changed from 0..1 (0..100 cents up) to -1..+1
//     (bipolar ±1200 cents). On read, v4 values are rescaled.
// v6: adds dupletPWM (independent PWM for the duplet oscillator) after
//     dupletWaveform. v5 and earlier default it to 0.5 on read.
// v7: adds the shared PWM LFO — pwmLfoRate (Hz), pwmLfoDepth,
//     pwmLfoDupletDepth — after dupletLevel. v6 and earlier default all
//     three to 0, i.e. LFO off, so older songs load sounding identical.
// v8: adds noiseLevel and resoBoost after the PWM LFO fields, and a fourth
//     per-step column (reverse step) after accent. v7 and earlier default
//     all three to 0. The step line gains a token rather than changing shape,
//     so the format stays readable in both directions: older builds stop
//     extracting after three tokens and ignore the fourth.
// v9: adds the distortion block — distType, distGain, distTone, distLevel —
//     after resoBoost. v8 and earlier default to type 0 (Classic, the stock
//     2004 clipper) with gain 0, tone 0.5 and level 1.0, which is exactly what
//     the engine did before the block existed, so older songs load unchanged.
//     Note this bump is distortion ONLY. Variable-length patterns (TODO item B)
//     are a *later* bump, not a second definition of v9 — the two were
//     deliberately kept apart so neither has to wait for the other.
// v10: variable-length patterns. A pattern block writes `lastStep + 1` step
//     rows instead of a fixed 16, and the reader takes its row count from the
//     `lastStep` it has just read. This is the one change in the format's
//     history that is NOT a pure append: the row count is positional, so a v9
//     reader handed a long pattern would run off the end of it and
//     desynchronise every field after it.
//
//     Which is why the writer stamps the *lowest* version that can represent
//     the song (`neededVersion` below) rather than always stamping the newest.
//     A song whose patterns are all 16 steps writes exactly the bytes v9 wrote
//     and still loads in an older build; only a song that actually uses another
//     length declares v10 and gives that up. The header number therefore means
//     "what this file needs", not "what wrote it".
// v11: the rest of the acid FX block - compThreshold, compDepth, ringHz,
//     ringAmount - after distLevel. A plain append, defaulting to a
//     compressor and ring modulator that are switched off, so a v10 or
//     earlier song loads unchanged. Written only when the song has something
//     to put in them; see neededVersion.
// v12: per-pattern triplet mode, as a SECOND TOKEN on the pattern's lastStep
//     line rather than a new line. `readInt` parses with std::stoi, which reads
//     the leading integer and ignores the rest, so an older build reads
//     "11 1" as lastStep 11 and never sees the flag. That makes v12 a pure
//     append in the same sense the per-step reverse column was: tolerant in
//     both directions, and costing no compatibility at all - unlike v10, whose
//     row count is positional. Stamped only when some pattern uses it.
// v13: per-sweep "hold end value", as an EIGHTH TOKEN on the sweep line. The
//     sweep line is read with `istringstream >>` for seven fields and the rest
//     ignored, so an older build reads a v13 sweep correctly and simply does not
//     hold - the step-line trick again. The reader takes the token whenever it
//     is there, whatever the header says. Stamped only when some sweep holds;
//     the writer emits the token only at v13, so lower stamps keep their bytes.
// v14: sweeps on the accent squelch and on the oscillator and acid FX sliders -
//     sweep targets 7..23 (the table is in CBEngine.h). Not one byte of the
//     layout changed: the sweep line is the same line with a bigger number in
//     its first token. It is a version anyway because of what an OLDER build
//     does with that number: it drops any sweep whose target is above 6 while
//     loading, without a word, and plays the song without them. A song that
//     quietly sounds different is the one outcome the header exists to
//     prevent, so such a song is stamped 14 and an older build refuses it out
//     loud instead. Stamped only when some sweep has such a target.
// v15: oscillating sweeps (TODO Y3) - four more tokens on the sweep line, after
//     the hold token: shape, rate unit (0 steps per cycle, 1 Hz), rate, start
//     phase in cycles. Written only on a sweep that oscillates. Tolerant on the
//     page exactly as v13 was, and stamped anyway for v14's reason: an older
//     build would play an oscillating sweep as a straight one between its two
//     DEPTHS, without a word. Stamped only when some sweep oscillates.
//     A FIFTH oscillation token, 1, marks a "between" LFO (it swings between its
//     two values instead of around the slider); it is written only then, so
//     every other LFO line keeps its four. Added before v15 was ever released,
//     which is why it is not a version of its own.
//     The same version retires the PWM LFO: its three header lines stay
//     (positional) and are written as 0, and a song that used it loads with
//     the LFO folded into oscillating sweeps - foldPwmLfo() below.
constexpr int kMaxVersion = 15;

// .hape shares the version counter with .roq for historical reasons, but its
// schema — a sweep count and one line per sweep — has never changed. Pin it
// rather than letting it ride the .roq bumps: stamping a sweep export with a
// number no older build accepts would break compatibility to describe a change
// that did not happen. Bump this only if a .hape line ever gains a field.
//
// It did, in v13 (the hold token) - and the same "what the file needs" rule as
// .roq applies: a .hape with no held sweep is still stamped 9 and still has the
// seven-token lines every older build reads; only one carrying a held sweep is
// stamped 13 and gets the eighth token.
//
// v14's new sweep targets do NOT raise it. A .hape is loaded from inside the
// sweep editor, which keeps only the sweeps for the slider being edited and
// says how many it skipped - so an older build handed a squelch sweep reports
// "skipped 1 for other sliders" rather than misplaying anything. Refusing the
// whole file would lose the sweeps it can use, to protect nothing.
constexpr int kHapeVersion     = 9;
constexpr int kHapeHoldVersion = 13;
// An oscillating sweep in a .hape IS a reason to refuse it in an older build,
// unlike v14's targets: the older build would keep it - it is on the slider
// being edited - and play it as a straight sweep between its depths.
constexpr int kHapeOscVersion  = 15;

bool anyHold(const std::vector<RoqSweep>& sweeps) {
    for (const auto& s : sweeps) if (s.hold) return true;
    return false;
}

bool anyOsc(const std::vector<RoqSweep>& sweeps) {
    for (const auto& s : sweeps) if (s.osc.on()) return true;
    return false;
}

// The shortest text that reads back as exactly this float. Done by trying
// precisions rather than with std::to_chars so that the C port can produce
// the same bytes with nothing but snprintf and strtof.
std::string floatToken(float v) {
    char buf[32];
    for (int prec = 1; prec <= 9; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, (double)v);
        if (std::strtof(buf, nullptr) == v) break;
    }
    return buf;
}

// One sweep line, the seven fields every version has, the v13 hold token and
// the v15 oscillation tokens. Missing tokens leave the defaults: no hold, no
// oscillation.
void readSweepLine(const std::string& line, RoqSweep& sw) {
    std::istringstream ss(line);
    ss >> sw.sliderIdx >> sw.startPat >> sw.startStep >> sw.startVal
       >> sw.endPat   >> sw.endStep   >> sw.endVal;
    int hold = 0;
    if (ss >> hold) sw.hold = hold != 0;
    int shape = 0, hz = 0;
    std::string rate, phase;
    if (ss >> shape >> hz >> rate >> phase) {
        sw.osc.shape = shape;
        sw.osc.hz    = hz != 0;
        sw.osc.rate  = std::strtof(rate.c_str(), nullptr);
        sw.osc.phase = std::strtof(phase.c_str(), nullptr);
        // strtof reads "nan" and "inf", and a NaN phase would reach the audio
        // as NaN samples. Nothing this program writes is either.
        if (!std::isfinite(sw.osc.rate))  sw.osc.rate  = 0.0f;
        if (!std::isfinite(sw.osc.phase)) sw.osc.phase = 0.0f;
        int mode = 0;
        if (ss >> mode) sw.osc.between = mode == 1;
        if (!sw.osc.on()) sw.osc = CBEngine::SweepOsc{};   // unknown shape: straight
    }
}

void writeSweepLine(std::ostream& out, const RoqSweep& sw, bool withHold, bool withOsc) {
    out << sw.sliderIdx << " "
        << sw.startPat  << " " << sw.startStep << " " << sw.startVal << " "
        << sw.endPat    << " " << sw.endStep   << " " << sw.endVal;
    if (withHold) out << " " << (sw.hold ? 1 : 0);
    if (withOsc && sw.osc.on())
        out << " " << sw.osc.shape << " " << (sw.osc.hz ? 1 : 0)
            << " " << floatToken(sw.osc.rate) << " " << floatToken(sw.osc.phase)
            << (sw.osc.between ? " 1" : "");
    out << "\n";
}

// Sweeps on the PWM LFO's own controls (targets 14..16) went with it. They
// were never in a release, so nothing is lost but a test song or two.
void dropRetired(std::vector<RoqSweep>& sweeps) {
    std::vector<RoqSweep> kept;
    kept.reserve(sweeps.size());
    for (const auto& s : sweeps)
        if (!CBEngine::sweepTargetRetired(s.sliderIdx)) kept.push_back(s);
    sweeps.swap(kept);
}

// The PWM LFO of v7..v14, as the oscillating sweeps that replace it (TODO Y3):
// for each destination with a depth, a triangle at the same rate in Hz, the
// same depth, starting with the song and holding to its end and beyond.
// With nothing else changed, the engine renders that exactly as the LFO did
// in a fresh engine - the WAV export's case - and the tester proves it. In
// the app it differs on purpose: the LFO was free-running, so every play of
// a song caught it somewhere else; the sweep's phase is the song position's.
//
// Run on every load, not only below v15: a hand-edited newer file with the
// LFO lines set should not get a modulator that nothing can show or edit.
//
// A rate of 0 was the LFO switched off, and then nothing is touched - not even
// depths left set behind it, which were inert and stay inert. One of the
// testers' songs has exactly that, and a song that never used the LFO should
// re-save to the bytes it was loaded from.
void foldPwmLfo(RoqSong& song) {
    if (!(song.pwmLfoRate > 0.0f)) return;
    {
        const int lastPat  = song.patterns.empty() ? 0 : (int)song.patterns.size() - 1;
        const int lastStep = song.patterns.empty() ? 15 : song.patterns.back().lastStep;
        auto add = [&](int target, float depth) {
            if (!(depth > 0.0f)) return;   // the engine skipped a zero depth too
            RoqSweep sw;
            sw.sliderIdx = target;
            sw.startPat = 0;       sw.startStep = 0;
            sw.endPat   = lastPat; sw.endStep   = lastStep;
            // The depth slider stored whole percents, and n / 100.0f is what
            // the engine gets back from n here (oscDepthAt in CBEngine.cpp).
            sw.startVal = sw.endVal = (int)std::lround(depth * 100.0f);
            sw.hold = true;
            sw.osc.shape = (int)CBEngine::OscShape::Triangle;
            sw.osc.hz    = true;
            sw.osc.rate  = song.pwmLfoRate;
            sw.osc.phase = 0.0f;
            song.sweeps.push_back(sw);
        };
        add(8,  song.pwmLfoDepth);         // Main PWM
        add(11, song.pwmLfoDupletDepth);   // Duplet PWM
    }
    song.pwmLfoRate = song.pwmLfoDepth = song.pwmLfoDupletDepth = 0.0f;
}

bool readLine(std::ifstream& in, std::string& out) {
    if (!std::getline(in, out)) return false;
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

bool readInt(std::ifstream& in, int& v, std::string& err) {
    std::string s;
    if (!readLine(in, s)) { err = "unexpected end of file"; return false; }
    try { v = std::stoi(s); } catch (...) { err = "expected integer, got '" + s + "'"; return false; }
    return true;
}

bool readFloat(std::ifstream& in, float& v, std::string& err) {
    std::string s;
    if (!readLine(in, s)) { err = "unexpected end of file"; return false; }
    try { v = std::stof(s); } catch (...) { err = "expected number, got '" + s + "'"; return false; }
    return true;
}

} // namespace

std::optional<RoqSong> loadRoq(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "cannot open file"; return std::nullopt; }

    std::string magic;
    if (!readLine(in, magic) || magic != kRoqMagic) {
        error = "not a .roq file (magic mismatch)";
        return std::nullopt;
    }
    int version = 0;
    if (!readInt(in, version, error)) return std::nullopt;
    if (version > kMaxVersion) { error = "unsupported .roq version"; return std::nullopt; }

    RoqSong song;
    if (!readFloat(in, song.volume,    error)) return std::nullopt;
    if (!readFloat(in, song.waveform,  error)) return std::nullopt;
    if (!readFloat(in, song.tuning,    error)) return std::nullopt;
    if (!readFloat(in, song.cutoff,    error)) return std::nullopt;
    if (!readFloat(in, song.resonance, error)) return std::nullopt;
    if (!readFloat(in, song.envmod,    error)) return std::nullopt;
    if (!readFloat(in, song.decay,     error)) return std::nullopt;
    if (!readFloat(in, song.accent,    error)) return std::nullopt;
    if (!readFloat(in, song.tempo,     error)) return std::nullopt;
    if (version >= 2) {
        if (!readFloat(in, song.impulsive, error)) return std::nullopt;
    }
    if (version >= 3) {
        if (!readFloat(in, song.accentSquelch, error)) return std::nullopt;
    }
    if (version >= 4) {
        if (!readFloat(in, song.pwm,            error)) return std::nullopt;
        if (!readFloat(in, song.subOscLevel,    error)) return std::nullopt;
        if (!readFloat(in, song.dupletWaveform, error)) return std::nullopt;
        if (version >= 6) {
            if (!readFloat(in, song.dupletPWM,  error)) return std::nullopt;
        }
        if (!readFloat(in, song.dupletDetune,   error)) return std::nullopt;
        if (!readFloat(in, song.dupletLevel,    error)) return std::nullopt;
        if (version == 4) {
            // v4 detune was 0..1 = 0..100 cents. Convert to v5's ±1 = ±1200 c.
            song.dupletDetune = song.dupletDetune * (100.0f / 1200.0f);
        }
    }
    if (version >= 7) {
        if (!readFloat(in, song.pwmLfoRate,        error)) return std::nullopt;
        if (!readFloat(in, song.pwmLfoDepth,       error)) return std::nullopt;
        if (!readFloat(in, song.pwmLfoDupletDepth, error)) return std::nullopt;
    }
    if (version >= 8) {
        if (!readFloat(in, song.noiseLevel, error)) return std::nullopt;
        if (!readFloat(in, song.resoBoost,  error)) return std::nullopt;
    }
    if (version >= 9) {
        if (!readFloat(in, song.distType,  error)) return std::nullopt;
        if (!readFloat(in, song.distGain,  error)) return std::nullopt;
        if (!readFloat(in, song.distTone,  error)) return std::nullopt;
        if (!readFloat(in, song.distLevel, error)) return std::nullopt;
    }

    if (version >= 11) {
        if (!readFloat(in, song.compThreshold, error)) return std::nullopt;
        if (!readFloat(in, song.compDepth,     error)) return std::nullopt;
        if (!readFloat(in, song.ringHz,        error)) return std::nullopt;
        if (!readFloat(in, song.ringAmount,    error)) return std::nullopt;
    }

    int patternCount = 0;
    if (!readInt(in, patternCount, error)) return std::nullopt;
    if (patternCount < 0 || patternCount > 4096) { error = "pattern count out of range"; return std::nullopt; }

    song.patterns.reserve(patternCount);
    for (int p = 0; p < patternCount; ++p) {
        CbPattern pat;
        {
            // Read as a LINE, not with readInt, because from v12 this line can
            // carry a second token. Extraction of the second simply fails on an
            // older file and leaves `trip` at 0 - the same tolerance the
            // per-step reverse column relies on.
            std::string line;
            if (!readLine(in, line)) { error = "unexpected end of file"; return std::nullopt; }
            std::istringstream ls(line);
            int last = 15, trip = 0;
            if (!(ls >> last)) {
                error = "expected integer, got '" + line + "'";
                return std::nullopt;
            }
            ls >> trip;
            pat.lastStep = last;
            pat.triplet  = trip != 0;
        }
        if (pat.lastStep < 0 || pat.lastStep > 63) pat.lastStep = 15;
        // Up to v9 the writer emitted a fixed 16 rows whatever `lastStep` said,
        // so 16 is what is actually on disk there — trusting `lastStep` on an
        // old file would read the wrong number of lines. From v10 the two agree
        // by construction. Steps past the last row keep CbStep's defaults, i.e.
        // rests.
        const int stepRows = (version >= 10) ? (pat.lastStep + 1) : 16;
        for (int s = 0; s < stepRows; ++s) {
            std::string line;
            if (!readLine(in, line)) { error = "truncated pattern data"; return std::nullopt; }
            std::istringstream ss(line);
            int pitch = 0, slide = 0, accent = 0, reverse = 0;
            ss >> pitch >> slide >> accent;
            // Fourth column arrived in v8. Pre-v8 lines simply have nothing
            // left to extract, the read fails, and `reverse` keeps its 0.
            ss >> reverse;
            pat.steps[s].pitch     = pitch;
            pat.steps[s].slide     = slide != 0;
            pat.steps[s].accent    = accent != 0;
            pat.steps[s].reverseStep = reverse != 0;
        }
        song.patterns.push_back(pat);
    }

    int sweepCount = 0;
    if (!readInt(in, sweepCount, error)) return std::nullopt;
    if (sweepCount < 0 || sweepCount > 100000) { error = "sweep count out of range"; return std::nullopt; }
    song.sweeps.reserve(sweepCount);
    for (int i = 0; i < sweepCount; ++i) {
        std::string line;
        if (!readLine(in, line)) { error = "truncated sweep data"; return std::nullopt; }
        RoqSweep sw;
        readSweepLine(line, sw);
        song.sweeps.push_back(sw);
    }
    dropRetired(song.sweeps);
    foldPwmLfo(song);
    return song;
}

// Lowest version that can represent this song without losing anything. Every
// field up to v9 is appended and defaulted, so any song needs at least v9 to
// carry the distortion block; a pattern of a length other than 16 is the only
// thing that forces v10, because the row count is the only part of the format
// a reader cannot absorb positionally. Keeping this a function of the *content*
// is what lets an ordinary 16-step song stay loadable by pre-v10 builds.
static int neededVersion(const RoqSong& song) {
    // v15: any oscillating sweep. Checked first because 15 implies
    // everything below it.
    if (anyOsc(song.sweeps)) return 15;
    // v14: any sweep on a target beyond the original seven.
    for (const auto& s : song.sweeps) {
        if (s.sliderIdx >= CBEngine::kMainSweepTargets) return 14;
    }
    // v13: any sweep that holds its end value.
    if (anyHold(song.sweeps)) return 13;
    // v12: any pattern in triplet mode.
    for (const auto& p : song.patterns) {
        if (p.triplet) return 12;
    }
    // v11: anything to store in the acid FX block. The test is "differs from
    // the default", not "would be audible" - a carrier frequency the user
    // dialled in with the amount at zero is still something they set, and
    // silently dropping it to keep a version number down would be the wrong
    // trade. A song that never opened the dialog writes v9 or v10 as before.
    const RoqSong d{};
    if (song.compThreshold != d.compThreshold || song.compDepth  != d.compDepth
     || song.ringHz        != d.ringHz        || song.ringAmount != d.ringAmount)
        return 11;
    // v10: any pattern that is not 16 steps. This is the one that actually
    // costs compatibility, because the step-row count is positional.
    for (const auto& p : song.patterns) {
        if (p.lastStep != 15) return 10;
    }
    // v9 is the floor: the distortion block is always written.
    return 9;
}

bool saveRoq(const std::string& path, const RoqSong& song, std::string& error) {
    std::ofstream out(path);
    if (!out) { error = "cannot create file"; return false; }

    const int version = neededVersion(song);

    out << kRoqMagic << "\n";
    out << version << "\n";
    out << song.volume    << "\n"
        << song.waveform  << "\n"
        << song.tuning    << "\n"
        << song.cutoff    << "\n"
        << song.resonance << "\n"
        << song.envmod    << "\n"
        << song.decay     << "\n"
        << song.accent    << "\n"
        << song.tempo          << "\n"
        // Deprecated, and deliberately still here: the line is positional
        // padding, not a value. Removing it shifts every field below it.
        << song.impulsive      << "\n"
        << song.accentSquelch  << "\n"
        << song.pwm            << "\n"
        << song.subOscLevel    << "\n"
        << song.dupletWaveform << "\n"
        << song.dupletPWM      << "\n"
        << song.dupletDetune   << "\n"
        << song.dupletLevel    << "\n"
        << song.pwmLfoRate        << "\n"
        << song.pwmLfoDepth       << "\n"
        << song.pwmLfoDupletDepth << "\n"
        << song.noiseLevel        << "\n"
        << song.resoBoost         << "\n"
        << song.distType  << "\n"
        << song.distGain  << "\n"
        << song.distTone  << "\n"
        << song.distLevel << "\n";

    // Guarded to mirror the reader exactly. A file stamped 9 or 10 must not
    // carry these lines: the header is positional, so four extra values would
    // be read as the pattern count and everything after it would shift.
    if (version >= 11) {
        out << song.compThreshold << "\n"
            << song.compDepth     << "\n"
            << song.ringHz        << "\n"
            << song.ringAmount    << "\n";
    }

    out << song.patterns.size() << "\n";
    for (const auto& p : song.patterns) {
        // The triplet flag is appended to this line rather than given one of
        // its own, so a pre-v12 reader still parses lastStep correctly off it.
        // Written only at v12 so a file stamped lower carries exactly the bytes
        // that version defined.
        if (version >= 12) out << p.lastStep << " " << (p.triplet ? 1 : 0) << "\n";
        else               out << p.lastStep << "\n";
        // Mirrors the reader. Under v9 this is 16 by construction — neededVersion()
        // returned 9 only because every lastStep is 15 — but derive it from the
        // version rather than from lastStep, so a future caller that got those two
        // out of step still produces a readable file rather than a truncated one.
        const int stepRows = (version >= 10) ? (p.lastStep + 1) : 16;
        for (int s = 0; s < stepRows; ++s) {
            out << p.steps[s].pitch << " "
                << (p.steps[s].slide  ? 1 : 0) << " "
                << (p.steps[s].accent ? 1 : 0) << " "
                << (p.steps[s].reverseStep ? 1 : 0) << "\n";
        }
    }

    out << song.sweeps.size() << "\n";
    // The hold token only at v13, mirroring neededVersion(): a file stamped
    // lower keeps exactly the seven-token lines it always had.
    for (const auto& sw : song.sweeps) writeSweepLine(out, sw, version >= 13, version >= 15);

    if (!out) { error = "write failed"; return false; }
    return true;
}

std::optional<std::vector<RoqSweep>> loadHape(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) { error = "cannot open file"; return std::nullopt; }

    std::string magic;
    if (!readLine(in, magic) || magic != kHapeMagic) {
        error = "not a .hape file (magic mismatch)";
        return std::nullopt;
    }
    int version = 0;
    if (!readInt(in, version, error)) return std::nullopt;
    if (version > kMaxVersion) { error = "unsupported .hape version"; return std::nullopt; }

    int sweepCount = 0;
    if (!readInt(in, sweepCount, error)) return std::nullopt;
    if (sweepCount < 0 || sweepCount > 100000) { error = "sweep count out of range"; return std::nullopt; }

    std::vector<RoqSweep> out;
    out.reserve(sweepCount);
    for (int i = 0; i < sweepCount; ++i) {
        std::string line;
        if (!readLine(in, line)) { error = "truncated sweep data"; return std::nullopt; }
        RoqSweep sw;
        readSweepLine(line, sw);
        out.push_back(sw);
    }
    dropRetired(out);
    return out;
}

bool saveHape(const std::string& path, const std::vector<RoqSweep>& sweeps, std::string& error) {
    std::ofstream out(path);
    if (!out) { error = "cannot create file"; return false; }
    out << kHapeMagic << "\n";
    // The same content rule as .roq: the lowest version that carries it all.
    // An oscillating sweep needs the hold token in front of its own, so 15
    // writes both.
    const bool osc  = anyOsc(sweeps);
    const bool hold = osc || anyHold(sweeps);
    out << (osc ? kHapeOscVersion : hold ? kHapeHoldVersion : kHapeVersion) << "\n";
    out << sweeps.size() << "\n";
    for (const auto& sw : sweeps) writeSweepLine(out, sw, hold, osc);
    if (!out) { error = "write failed"; return false; }
    return true;
}
