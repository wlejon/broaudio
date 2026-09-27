// The ear (include/broaudio/ear/ear.h) against synthetic signals with known
// answers: a decaying sine rings long and reads tonal, white noise reads
// flat and atonal, an impulse peaks at 0 with a short tail, a 1 kHz full-
// scale sine reads -3.01 LUFS, a clip compared with itself scores 0 and with
// a shifted, rescaled or resampled copy scores small, and the same input
// gives the same output, bit for bit.

#include "test_harness.h"

#include <broaudio/ear/ear.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>

using namespace broaudio::ear;

namespace {

constexpr int kRate = 48000;

Clip makeClip(std::vector<float> s, int rate = kRate) {
    Clip c;
    c.samples = std::move(s);
    c.sampleRate = rate;
    return c;
}

// Sum of decaying sines: {freq, amplitude, time constant (s)}.
struct Tone {
    double freq, amp, tau;
};
Clip tones(const std::vector<Tone>& parts, double seconds, int rate = kRate) {
    std::vector<float> s(static_cast<size_t>(seconds * rate));
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = 0.0;
        for (const Tone& p : parts) {
            v += p.amp * std::exp(-t / p.tau) * std::sin(2.0 * std::numbers::pi * p.freq * t);
        }
        s[i] = static_cast<float>(v);
    }
    return makeClip(std::move(s), rate);
}

Clip whiteNoise(double seconds, double amp) {
    std::vector<float> s(static_cast<size_t>(seconds * kRate));
    uint32_t state = 0x12345678u;
    for (float& v : s) {
        state = state * 1664525u + 1013904223u;
        v = static_cast<float>(amp * ((state >> 8) / 8388608.0 - 1.0));
    }
    return makeClip(std::move(s));
}

Clip delayed(const Clip& c, double seconds, double gain = 1.0) {
    Clip d = c;
    const size_t pad = static_cast<size_t>(seconds * c.sampleRate);
    d.samples.insert(d.samples.begin(), pad, 0.0f);
    for (float& s : d.samples) s = static_cast<float>(s * gain);
    return d;
}

} // namespace

TEST(decaying_sine_rings_and_reads_tonal) {
    // tau = 0.4 s: 21.7 dB/s, so -60 dB at 2.763 s.
    const Clip c = tones({{440.0, 0.8, 0.4}}, 4.0);
    const Measurement m = measure(c);
    ASSERT_NEAR(m.peakDb, 20.0 * std::log10(0.8), 0.2);
    ASSERT_LT(m.envelopePeakTime, 0.02);
    ASSERT_EQ(m.tailEnd, std::string("floor"));
    ASSERT_NEAR(m.tailTime, 2.763, 0.08);
    ASSERT_NEAR(m.decayRate, 21.71, 1.0);
    ASSERT_NEAR(m.t60, 2.763, 0.15);
    ASSERT_GT(m.tonality, 0.9);
    ASSERT_LT(m.flatness, 0.05);
    ASSERT_NEAR(m.centroidHz, 440.0, 60.0);
    ASSERT_TRUE(!m.partials.empty());
    ASSERT_NEAR(m.partials[0].freqHz, 440.0, 2.0);
    // A ~100 ms analysis window averages over the decay: within 1.5 dB.
    ASSERT_NEAR(m.partials[0].peakDb, 20.0 * std::log10(0.8), 1.5);
    ASSERT_GT(m.partials[0].ringTime, 2.2);
    ASSERT_NEAR(m.partials[0].decayRate, 21.71, 2.0);
    ASSERT_LT(m.partials[0].stabilityCents, 5.0);
    ASSERT_EQ(m.ringing.count, 1);
    ASSERT_NEAR(m.ringing.inharmonicity, 0.0, 1e-9);
    ASSERT_GT(m.ringing.ringScore, 0.8);
    ASSERT_EQ(static_cast<int>(m.timeline.size()), 8);
    ASSERT_GT(m.timeline[0].rmsDb, m.timeline[7].rmsDb + 40.0);
    PASS();
}

TEST(decay_into_noise_floor_and_truncated_decay) {
    // The same decay over a -64.8 dBFS RMS noise floor: the tail ends 6 dB
    // over the floor (-58.8 dBFS), 54 dB under the -4.9 dBFS envelope peak.
    Clip c = tones({{440.0, 0.8, 0.4}}, 4.0);
    const Clip n = whiteNoise(4.0, 0.001);
    for (size_t i = 0; i < c.samples.size(); ++i) c.samples[i] += n.samples[i];
    const Measurement m = measure(c);
    ASSERT_EQ(m.tailEnd, std::string("noise"));
    ASSERT_NEAR(m.noiseFloorDb, -64.8, 1.5);
    ASSERT_NEAR(m.tailTime, 2.49, 0.15);
    ASSERT_NEAR(m.t60, 2.763, 0.2);
    ASSERT_GT(m.tonality, 0.9);

    // A clean decay cut off at -43 dB is not a noise floor.
    const Measurement t = measure(tones({{440.0, 0.8, 0.3}}, 1.5));
    ASSERT_EQ(t.tailEnd, std::string("end"));
    ASSERT_TRUE(std::isnan(t.noiseFloorDb));
    ASSERT_NEAR(t.t60, 2.072, 0.15);
    PASS();
}

TEST(white_noise_reads_flat_and_atonal) {
    const Clip c = whiteNoise(2.0, 0.3);
    const Measurement m = measure(c);
    ASSERT_GT(m.flatness, 0.8);
    ASSERT_LT(m.tonality, 0.02);
    ASSERT_LT(m.ringing.ringScore, 0.02);
    ASSERT_NEAR(m.centroidHz, 12000.0, 800.0);
    ASSERT_EQ(m.tailEnd, std::string("end"));  // stationary: no decay before the end
    ASSERT_TRUE(std::isnan(m.noiseFloorDb));
    for (const Slice& s : m.timeline) ASSERT_GT(s.flatness, 0.7);
    PASS();
}

TEST(impulse_peaks_at_zero_with_short_tail) {
    std::vector<float> s(kRate, 0.0f);
    s[0] = 1.0f;
    const Measurement m = measure(makeClip(std::move(s)));
    ASSERT_EQ(m.peakTime, 0.0);
    ASSERT_NEAR(m.peakDb, 0.0, 1e-6);
    ASSERT_LT(m.envelopePeakTime, 0.006);
    ASSERT_LT(m.tailTime, 0.02);
    ASSERT_LT(m.tonality, 0.05);
    PASS();
}

TEST(lufs_of_full_scale_1k_sine) {
    const Clip c = tones({{1000.0, 1.0, 1e9}}, 3.0);
    const Measurement m = measure(c);
    ASSERT_NEAR(m.lufs, -3.01, 0.05);
    ASSERT_NEAR(m.rmsDb, -3.01, 0.02);
    // A steady sine's partial level is its amplitude.
    ASSERT_NEAR(m.partials[0].peakDb, 0.0, 0.2);
    ASSERT_NEAR(m.partials[0].freqHz, 1000.0, 0.5);
    ASSERT_TRUE(std::isnan(m.partials[0].t60));
    // And at 44.1 kHz: the K-weighting is designed per rate.
    const Measurement m2 = measure(tones({{1000.0, 1.0, 1e9}}, 3.0, 44100));
    ASSERT_NEAR(m2.lufs, -3.01, 0.05);
    // Silence has no loudness.
    const Measurement ms = measure(makeClip(std::vector<float>(kRate, 0.0f)));
    ASSERT_TRUE(std::isnan(ms.lufs));
    PASS();
}

TEST(harmonic_and_bar_like_partials) {
    const Clip harmonic = tones({{200.0, 0.5, 1.0}, {400.0, 0.3, 0.8}, {600.0, 0.2, 0.6}}, 3.0);
    const Measurement h = measure(harmonic);
    ASSERT_EQ(h.ringing.count, 3);
    ASSERT_NEAR(h.ringing.f0Hz, 200.0, 2.0);
    ASSERT_LT(h.ringing.inharmonicity, 0.05);
    ASSERT_NEAR(h.partials[1].ratio, 2.0, 0.03);

    // A free bar's modes, 1 : 2.756 : 5.404, ringing on: the synthetic
    // xylophone signature.
    const Clip bar = tones({{523.0, 0.5, 0.8}, {523.0 * 2.756, 0.3, 0.5}, {523.0 * 5.404, 0.2, 0.3}}, 3.0);
    const Measurement b = measure(bar);
    ASSERT_EQ(b.ringing.count, 3);
    ASSERT_GT(b.ringing.inharmonicity, 0.2);
    ASSERT_NEAR(b.ringing.f0Hz, 523.0, 3.0);
    ASSERT_GT(b.ringing.ringScore, 0.8);
    ASSERT_GT(b.ringing.sparsity, 0.99);
    ASSERT_GT(b.ringing.weightedRingTime, 1.0);
    PASS();
}

TEST(compare_self_shifted_rescaled_resampled) {
    const Clip a = tones({{330.0, 0.6, 0.3}, {990.0, 0.2, 0.2}}, 1.5);
    const Comparison self = compare(a, a);
    ASSERT_NEAR(self.score, 0.0, 1e-9);
    ASSERT_NEAR(self.offsetTime, 0.0, 1e-12);

    const Comparison shifted = compare(delayed(a, 0.030), a);
    ASSERT_NEAR(shifted.offsetTime, 0.030, 0.0051);
    ASSERT_LT(shifted.score, 0.03);

    const Comparison quiet = compare(delayed(a, 0.0, 0.25), a);
    ASSERT_NEAR(quiet.loudnessDiffDb, -12.04, 0.1);
    ASSERT_LT(quiet.score, 0.005);

    Clip r = resampled(a, 44100);
    const Comparison rs = compare(r, a);
    ASSERT_EQ(rs.sampleRate, 44100);
    ASSERT_LT(rs.score, 0.05);

    const Comparison noise = compare(whiteNoise(1.5, 0.3), a);
    ASSERT_GT(noise.score, 0.5);
    ASSERT_GT(noise.spectrum, 0.5);
    ASSERT_GT(noise.tonality, 0.4);

    // A different decay: the envelope component sees it.
    const Comparison longer = compare(tones({{330.0, 0.6, 1.2}, {990.0, 0.2, 0.8}}, 1.5), a);
    ASSERT_GT(longer.envelope, 0.3);
    ASSERT_GT(longer.score, shifted.score * 4);
    PASS();
}

TEST(spectrogram_shape_and_pixels) {
    const Clip a = tones({{1000.0, 0.8, 0.5}}, 1.0);
    const Clip b = whiteNoise(0.5, 0.1);
    SpectrogramOptions o;
    o.width = 400;
    o.height = 200;
    const SpectrogramImage img = spectrogram({&a, &b}, {"sine", "noise"}, o);
    ASSERT_EQ(static_cast<int>(img.panels.size()), 2);
    ASSERT_EQ(static_cast<size_t>(img.width) * img.height * 4, img.rgba.size());
    ASSERT_NEAR(img.duration, 1.0, 1e-9);
    ASSERT_NEAR(img.maxHz, 24000.0, 1e-6);
    ASSERT_NEAR(img.maxDb - img.minDb, 80.0, 1e-9);
    const SpectrogramPanel& p0 = img.panels[0];
    const SpectrogramPanel& p1 = img.panels[1];
    ASSERT_EQ(p0.x, p1.x);  // stacked
    ASSERT_GT(p1.y, p0.y + p0.height);
    auto px = [&](int x, int y) { return img.rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4; };
    auto luma = [&](int x, int y) { const uint8_t* p = px(x, y); return p[0] + p[1] + p[2]; };
    // The 1 kHz row is bright near the start, the top rows (far from 1 kHz) dark.
    const double v = std::log(1000.0 / img.minHz) / std::log(img.maxHz / img.minHz);
    const int y1k = p0.y + p0.height - 1 - static_cast<int>(std::lround(v * (p0.height - 1)));
    ASSERT_GT(luma(p0.x + 10, y1k), 500);
    ASSERT_LT(luma(p0.x + 10, p0.y + 2), 150);
    // The noise clip ends halfway: its panel is hatched grey (r == g) after.
    const uint8_t* after = px(p1.x + 300, p1.y + 50);
    ASSERT_TRUE(after[0] == after[1] && after[0] < 80);
    const std::vector<uint8_t> png = encodePng(img.rgba.data(), img.width, img.height);
    ASSERT_TRUE(png.size() > img.rgba.size());
    ASSERT_TRUE(png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    // Side by side.
    o.stacked = false;
    const SpectrogramImage side = spectrogram({&a, &b}, {}, o);
    ASSERT_EQ(side.panels[0].y, side.panels[1].y);
    ASSERT_GT(side.width, img.width);
    PASS();
}

// A UI click: a Hann-windowed 2 kHz burst `ms` long at `amp`, `lead` seconds
// of silence before it and `trail` after.
Clip click(double ms, double amp, double trail = 0.0, double lead = 0.0) {
    const size_t n = static_cast<size_t>(ms * 0.001 * kRate);
    const size_t pre = static_cast<size_t>(lead * kRate);
    std::vector<float> s(pre + n + static_cast<size_t>(trail * kRate), 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n);
        s[pre + i] = static_cast<float>(amp * w * std::sin(2.0 * std::numbers::pi * 2000.0 * i / kRate));
    }
    return makeClip(std::move(s));
}

TEST(short_clip_loudness) {
    // Silence around a click moves BS.1770's single block, not lufsShort.
    const Measurement bare = measure(click(30, 0.5));
    const Measurement padded = measure(click(30, 0.5, 0.25, 0.05));
    ASSERT_NEAR(padded.lufsShort, bare.lufsShort, 0.01);
    ASSERT_GT(bare.lufs - padded.lufs, 8.0);   // 30 ms vs 330 ms of energy average
    // Shorter than 400 ms: loudness is lufsShort.
    ASSERT_TRUE(bare.loudness == bare.lufsShort && padded.loudness == padded.lufsShort);

    // Level: exactly 20 log10 of the gain, monotone.
    const double ref20 = measure(click(20, 0.5, 0.1)).lufsShort;
    double prev = -1e9;
    for (double amp : {0.01, 0.03, 0.1, 0.2, 0.5, 1.0}) {
        const Measurement m = measure(click(20, amp, 0.1));
        ASSERT_NEAR(m.lufsShort - ref20, 20.0 * std::log10(amp / 0.5), 1e-4);
        ASSERT_GT(m.lufsShort, prev + 1.0);
        prev = m.lufsShort;
    }
    // Duration: longer clicks at one level read louder, by their energy
    // (a doubling ~ +3 dB) up to the 100 ms window.
    prev = -1e9;
    for (double ms : {5.0, 10.0, 20.0, 40.0, 80.0}) {
        const double l = measure(click(ms, 0.5, 0.1)).lufsShort;
        if (prev > -1e8) ASSERT_NEAR(l - prev, 3.01, 0.3);
        prev = l;
    }
    // A steady tone reads its integrated loudness at any length.
    const Measurement tone = measure(tones({{1000.0, 1.0, 1e9}}, 0.3));
    ASSERT_NEAR(tone.lufsShort, -3.01, 0.1);
    const Measurement longTone = measure(tones({{1000.0, 1.0, 1e9}}, 3.0));
    ASSERT_NEAR(longTone.lufsShort, longTone.lufs, 0.05);
    ASSERT_TRUE(longTone.loudness == longTone.lufs);
    // The blend is continuous in duration: a decaying tone cut at 399, 401,
    // 799 and 801 ms.
    const Clip decay = tones({{800.0, 0.8, 0.15}}, 1.0);
    double last = NAN;
    for (double ms : {399.0, 401.0, 600.0, 799.0, 801.0}) {
        Clip c = decay;
        c.samples.resize(static_cast<size_t>(ms * 0.001 * kRate));
        const Measurement m = measure(c);
        ASSERT_TRUE(m.loudness <= std::max(m.lufs, m.lufsShort) + 1e-9);
        ASSERT_TRUE(m.loudness >= std::min(m.lufs, m.lufsShort) - 1e-9);
        if (!std::isnan(last) && (ms == 401.0 || ms == 801.0)) ASSERT_LT(std::fabs(m.loudness - last), 0.2);
        last = m.loudness;
    }
    // Silence: undefined.
    ASSERT_TRUE(std::isnan(measure(makeClip(std::vector<float>(4800, 0.0f))).lufsShort));

    // compare(): a short click and itself with silence after it are equally
    // loud; a quarter-level copy is 12 dB down.
    const Comparison same = compare(click(30, 0.5, 0.25), click(30, 0.5));
    ASSERT_TRUE(same.loudnessScale == "lufsShort");
    ASSERT_NEAR(same.loudnessDiffDb, 0.0, 0.01);
    const Comparison quiet = compare(click(30, 0.125, 0.1), click(30, 0.5, 0.1));
    ASSERT_NEAR(quiet.loudnessDiffDb, -12.04, 0.01);
    ASSERT_LT(quiet.score, 0.005);
    ASSERT_TRUE(compare(tones({{330.0, 0.6, 0.3}}, 1.5), tones({{330.0, 0.6, 0.3}}, 1.0)).loudnessScale == "lufs");
    PASS();
}

TEST(deterministic) {
    const Clip a = tones({{523.0, 0.5, 0.8}, {1441.0, 0.3, 0.5}}, 1.0);
    Clip n = whiteNoise(1.0, 0.05);
    for (size_t i = 0; i < n.samples.size(); ++i) n.samples[i] += a.samples[i];
    const Measurement m1 = measure(n), m2 = measure(n);
    ASSERT_TRUE(std::memcmp(&m1.tonality, &m2.tonality, sizeof(double)) == 0);
    ASSERT_TRUE(std::memcmp(&m1.lufs, &m2.lufs, sizeof(double)) == 0);
    ASSERT_EQ(m1.partials.size(), m2.partials.size());
    for (size_t i = 0; i < m1.partials.size(); ++i) {
        ASSERT_TRUE(std::memcmp(&m1.partials[i].freqHz, &m2.partials[i].freqHz, sizeof(double)) == 0);
        ASSERT_TRUE(std::memcmp(&m1.partials[i].ringTime, &m2.partials[i].ringTime, sizeof(double)) == 0);
    }
    const Comparison c1 = compare(n, a), c2 = compare(n, a);
    ASSERT_TRUE(std::memcmp(&c1.score, &c2.score, sizeof(double)) == 0);
    const SpectrogramImage s1 = spectrogram({&n, &a}, {"x", "y"}), s2 = spectrogram({&n, &a}, {"x", "y"});
    ASSERT_TRUE(s1.rgba == s2.rgba);
    PASS();
}

int main() { return runAllTests(); }
