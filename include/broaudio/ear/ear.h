#pragma once

// The ear: offline analysis of a finished clip, so a program (or an agent)
// can judge how something sounds from numbers and pictures instead of
// listening. Three entry points:
//
//   measure()      a report on one clip: timing (peak, attack, tail, decay),
//                  loudness (peak, RMS, BS.1770 integrated LUFS and a
//                  short-clip K-weighted window loudness), spectral
//                  centroid and flatness, tonality, and the strongest pure
//                  partials with how long each rings.
//   compare()      how far a clip is from a reference recording, overall and
//                  broken down by envelope, spectrum and tonality. Lower is
//                  closer; a clip compared with itself scores 0.
//   spectrogram()  an RGBA image of one or more clips on a shared time and dB
//                  scale, with labelled time and frequency axes.
//
// Everything here is pure computation on the calling thread: no engine, no
// device, no locks, no globals. It is deterministic: the same input gives the
// same numbers and the same pixels, bit for bit, on a given build. Every clip
// is analysed as mono (monoClip averages the channels). Not for the audio
// thread: all of it allocates.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace broaudio::ear {

// A mono clip. `channels` records how many channels the source had before
// the mixdown (informational only).
struct Clip {
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 1;

    double duration() const {
        return sampleRate > 0 ? static_cast<double>(samples.size()) / sampleRate : 0.0;
    }
};

// Average interleaved `channels`-channel PCM down to one channel.
Clip monoClip(const float* interleaved, size_t frames, int channels, int sampleRate);

// Decode a file with loadAudioFile (wav / flac / mp3 / ogg / opus) and mix it
// to mono. False with `error` set when the file does not decode. The path is
// used as given; resolving it is the caller's business.
bool loadClip(const std::string& path, Clip& out, std::string* error = nullptr);

// The clip at `rate` through broaudio's polyphase resampler (a copy when the
// rate already matches).
Clip resampled(const Clip& clip, int rate);

// "No value" in the result structs below is NaN (a bindings layer maps it to
// null). Levels are dBFS: 0 dB is a full-scale sample (peak) or the RMS of a
// full-scale square wave, so a full-scale sine reads 0 dB peak, -3 dB RMS.
// Silence is floored at -120 dB.

struct MeasureOptions {
    // The tail ends where the envelope stays below the envelope peak plus
    // this (dB, negative), or 6 dB above the noise floor, whichever is higher.
    float tailFloorDb = -60.0f;
    // How many partials the report lists (strongest first).
    int maxPartials = 8;
    // How many equal time slices the coarse timeline has.
    int slices = 8;
};

// One slice of the coarse timeline.
struct Slice {
    double time = 0;        // slice start, seconds
    double duration = 0;
    double rmsDb = -120;
    double centroidHz = 0;  // of the slice's average power spectrum
    double flatness = 0;    // ditto; 0 = pure tone, ~1 = white noise
    double tonality = 0;    // tonal share of the slice's energy
};

// One tracked sinusoidal partial: a spectral peak that stays prominent (15 dB
// over the local median spectrum) and stable in frequency from frame to frame
// for at least 60 ms. A partial that stops and restarts (a repeated note) is
// two entries with different start times.
struct Partial {
    double freqHz = 0;          // energy-weighted mean frequency
    double peakDb = -120;       // its loudest amplitude, dBFS (a sine of amplitude A reads 20log10 A)
    double startTime = 0;       // first frame it is tracked in
    double peakTime = 0;        // when it is loudest
    double endTime = 0;         // last frame it is tracked in
    double ringTime = 0;        // peakTime to the last frame within 60 dB of peakDb
    double decayRate = 0;       // dB per second after its peak (least squares; 0 = not decaying)
    double t60 = NAN;           // 60 / decayRate, NaN when it does not decay
    double stabilityCents = 0;  // energy-weighted frequency deviation, cents
    double energyShare = 0;     // its energy / the clip's total energy
    double ratio = 0;           // freqHz / Ringing::f0Hz (NaN without an f0)
};

// The "few pure tones ringing on" summary. A long-ringing, sparse, tonal set
// of partials (ringScore near 1), especially one whose frequencies are not a
// harmonic series (inharmonicity well above 0), is the signature of a sound
// that reads as synthetic: a sine bell, a xylophone-like bar model.
struct Ringing {
    int count = 0;                 // distinct partial frequencies within 30 dB of the strongest
    double sparsity = 0;           // energy of the 3 strongest partials / all tonal energy
    double strongestRingTime = 0;  // ringTime of the most energetic partial
    double weightedRingTime = 0;   // energy-weighted mean ringTime over the counted partials
    double f0Hz = NAN;             // best-fitting fundamental of the counted partials
    double inharmonicity = 0;      // 0 = exact harmonic series of f0Hz .. 1 = unrelated
    double ringScore = 0;          // tonality * sparsity * min(1, weightedRingTime / 0.5 s)
};

struct Measurement {
    int sampleRate = 0;
    int channels = 1;
    double duration = 0;

    // Timing. The envelope is the RMS level of 10 ms windows every 5 ms.
    double peakTime = 0;          // largest |sample|
    double envelopePeakTime = 0;  // loudest envelope frame
    double onsetTime = 0;         // first envelope frame within 30 dB of the envelope peak
    double attackTime = 0;        // envelopePeakTime - onsetTime
    double tailTime = 0;          // envelopePeakTime to where the envelope stays below the tail threshold
    std::string tailEnd = "end";  // what ended the tail: "floor" (tailFloorDb), "noise" (noise floor) or "end" (the clip ran out first)
    double noiseFloorDb = NAN;    // 10th percentile of the envelope, when at least 20 dB below its peak and a
                                  // plateau (the frames after the peak within 3 dB of it fall slower than 3 dB/s)
    double decayRate = 0;         // dB/s, Schroeder energy-decay slope fitted from -5 to -35 dB (or as far as the tail reaches, if >= 10 dB)
    double t60 = NAN;             // 60 / decayRate

    // Loudness.
    double peakDb = -120;          // sample peak, dBFS
    double envelopePeakDb = -120;  // loudest 10 ms RMS, dBFS
    double rmsDb = -120;           // whole clip RMS, dBFS
    double lufs = NAN;             // BS.1770-4 integrated loudness of the mono clip (K-weighted, 400 ms blocks,
                                   // -70 LUFS absolute and -10 LU relative gates; one block when shorter than 400 ms);
                                   // NaN when nothing passes the absolute gate
    // BS.1770's 400 ms blocks cannot resolve a clip shorter than a block: its
    // one block is the clip's energy over however long the clip happens to
    // run (trailing silence lowers it). lufsShort is meaningful at any
    // length: the loudest 100 ms window of the K-weighted signal (a clip
    // shorter than 100 ms counts as zero-padded to 100 ms), in LUFS units,
    // so it does not depend on silence around the sound, it scales exactly
    // with level (+6.02 dB per doubling), a steady sound reads its lufs, and
    // clicks shorter than 100 ms read by their energy, as the ear integrates
    // them. NaN under the -70 LUFS absolute gate.
    double lufsShort = NAN;
    // The loudness the ear judges by (compare's normalisation, fit's
    // `loudness` target): lufsShort for a clip shorter than 400 ms, lufs from
    // 800 ms, and between the two a blend in dB weighted by the duration, so
    // it is continuous in the clip's length. NaN when both are.
    double loudness = NAN;

    // Spectrum, of the energy-weighted long-term average power spectrum.
    double centroidHz = 0;
    double flatness = 0;  // geometric / arithmetic mean power, 30 Hz .. min(16 kHz, Nyquist)

    // Tonality: the share of the clip's energy that sits in tracked partials.
    double tonality = 0;

    std::vector<Slice> timeline;
    std::vector<Partial> partials;  // strongest (by energy) first, at most maxPartials
    Ringing ringing;
};

Measurement measure(const Clip& clip, const MeasureOptions& opts = MeasureOptions{});

struct CompareOptions {
    bool align = true;       // align onsets (then refine within maxShift)
    double maxShift = 0.05;  // seconds of refinement around the onset alignment
    // Weights of the three components in `score`.
    double envelopeWeight = 0.35;
    double spectrumWeight = 0.45;
    double tonalityWeight = 0.20;
};

// compare(): both clips are brought to the lower of their two sample rates,
// loudness-normalised (each to -23 on one scale for both: the
// Measurement::loudness blend of lufsShort and lufs weighted by the *shorter*
// clip's duration, so when either clip is under 400 ms both are judged by
// lufsShort, and from 800 ms by lufs; or by RMS when that is undefined),
// and onset-aligned; lengths may differ (the missing part of the shorter one
// counts as silence). Components are dimensionless, ~0 for a match and ~1 for
// "very different" (they can exceed 1).
struct Comparison {
    double score = 0;     // envelopeWeight*envelope + spectrumWeight*spectrum + tonalityWeight*tonality
    double envelope = 0;  // envelopeDb / 30
    double spectrum = 0;  // (spectrogramDb + ltasDb) / 2 / 20
    double tonality = 0;  // 0.5*|dTonality| + 0.3*min(1, |log2((ringA+0.05)/(ringB+0.05))| / 2) + 0.2*|dInharmonicity|

    int sampleRate = 0;         // the rate the comparison ran at
    double offsetTime = 0;      // how much later the clip starts than the reference (s)
    double loudnessDiffDb = 0;  // clip loudness - reference loudness (before normalisation; RMS when undefined)
    std::string loudnessScale = "lufs";  // what loudnessDiffDb and the normalisation used: "lufs" (both clips 800 ms
                                         // or longer), "lufsShort" (either under 400 ms), "blend" (between) or "rms"
    double envelopeDb = 0;      // mean |difference| of the peak-relative envelopes, floored at -60 dB, over frames either is above the floor
    double spectrogramDb = 0;   // mean |difference| of 40-band mel spectrograms (dB, floored 80 dB under the louder), over frames either is within 60 dB of its peak
    double ltasDb = 0;          // mean |difference| of the 40-band long-term spectra, each normalised to its total power
    double centroidRatio = 1;   // clip centroid / reference centroid
    double tonalityDiff = 0;    // clip tonality - reference tonality
    double ringTimeRatio = 1;   // (clip weightedRingTime + 0.05) / (reference weightedRingTime + 0.05)
    double inharmonicityDiff = 0;
    double durationDiff = 0;    // clip tail end - reference tail end, after alignment (s)
};

Comparison compare(const Clip& clip, const Clip& reference, const CompareOptions& opts = CompareOptions{});

enum class FrequencyScale : uint8_t { Log, Mel, Linear };

struct SpectrogramOptions {
    int width = 800;   // plot area of each panel, pixels
    int height = 256;
    bool stacked = true;  // clips one above another (false: side by side)
    FrequencyScale scale = FrequencyScale::Log;
    double minHz = 30;    // Linear ignores it and starts at 0
    double maxHz = 0;     // 0 = the lowest Nyquist among the clips
    double dbRange = 80;  // colour span below the top of the scale
    double maxDb = NAN;   // top of the colour scale; NaN = the loudest bin of all clips
    int fftSize = 0;      // 0 = ~43 ms at the clip's rate (2048 at 44.1 / 48 kHz)
    int fontScale = 2;    // the 5x7 bitmap font's pixel size
};

struct SpectrogramPanel {
    std::string label;
    int x = 0, y = 0, width = 0, height = 0;  // the plot area inside the image
    double duration = 0;
    int sampleRate = 0;
};

struct SpectrogramImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width * height * 4, row-major, top row first
    double duration = 0;        // the shared time axis spans 0 .. duration
    double minHz = 0, maxHz = 0;
    double minDb = 0, maxDb = 0;  // the shared colour scale
    std::vector<SpectrogramPanel> panels;
};

// All clips share one time axis (the longest clip's duration; a shorter clip's
// panel is hatched after its end), one frequency axis and one dB scale, so the
// panels compare directly. `labels` (optional, may be shorter than `clips`)
// are drawn over each panel.
SpectrogramImage spectrogram(const std::vector<const Clip*>& clips,
                             const std::vector<std::string>& labels,
                             const SpectrogramOptions& opts = SpectrogramOptions{});

// A PNG (8-bit RGBA, zlib stored blocks: exact and dependency-free, not
// small). False when the file cannot be written.
std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height);
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);

} // namespace broaudio::ear
