#include "RoqIO.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

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
constexpr int kMaxVersion = 13;

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
constexpr int kHapeVersion     = 9;
constexpr int kHapeHoldVersion = 13;

bool anyHold(const std::vector<RoqSweep>& sweeps) {
    for (const auto& s : sweeps) if (s.hold) return true;
    return false;
}

// One sweep line, the seven fields every version has plus the v13 hold token.
void readSweepLine(const std::string& line, RoqSweep& sw) {
    std::istringstream ss(line);
    ss >> sw.sliderIdx >> sw.startPat >> sw.startStep >> sw.startVal
       >> sw.endPat   >> sw.endStep   >> sw.endVal;
    int hold = 0;
    if (ss >> hold) sw.hold = hold != 0;
}

void writeSweepLine(std::ostream& out, const RoqSweep& sw, bool withHold) {
    out << sw.sliderIdx << " "
        << sw.startPat  << " " << sw.startStep << " " << sw.startVal << " "
        << sw.endPat    << " " << sw.endStep   << " " << sw.endVal;
    if (withHold) out << " " << (sw.hold ? 1 : 0);
    out << "\n";
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
    return song;
}

// Lowest version that can represent this song without losing anything. Every
// field up to v9 is appended and defaulted, so any song needs at least v9 to
// carry the distortion block; a pattern of a length other than 16 is the only
// thing that forces v10, because the row count is the only part of the format
// a reader cannot absorb positionally. Keeping this a function of the *content*
// is what lets an ordinary 16-step song stay loadable by pre-v10 builds.
static int neededVersion(const RoqSong& song) {
    // v13: any sweep that holds its end value. Checked first because 13
    // implies everything below it.
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
    for (const auto& sw : song.sweeps) writeSweepLine(out, sw, version >= 13);

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
    return out;
}

bool saveHape(const std::string& path, const std::vector<RoqSweep>& sweeps, std::string& error) {
    std::ofstream out(path);
    if (!out) { error = "cannot create file"; return false; }
    out << kHapeMagic << "\n";
    const bool hold = anyHold(sweeps);
    out << (hold ? kHapeHoldVersion : kHapeVersion) << "\n";
    out << sweeps.size() << "\n";
    for (const auto& sw : sweeps) writeSweepLine(out, sw, hold);
    if (!out) { error = "write failed"; return false; }
    return true;
}
