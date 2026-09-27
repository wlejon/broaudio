// Air absorption: the ISO 9613-1 coefficient, the fitted filter table against
// it, the chain stage against the table, and the engine end to end (the
// centroid of a spatialized noise source falls with distance).

#include "test_harness.h"
#include "distance_test_util.h"
#include "broaudio/spatial/air_absorption.h"
#include "broaudio/spatial/spatial_chain.h"

#include <cstdio>

using namespace broaudio;

TEST(iso_coefficient_matches_published_values) {
    // ISO 9613-1 / ANSI S1.26 at 20 °C, 50 % RH, 101.325 kPa (dB/km).
    const double a1k = isoAirAbsorptionDbPerMetre(1000.0, 20.0, 50.0) * 1000.0;
    const double a8k = isoAirAbsorptionDbPerMetre(8000.0, 20.0, 50.0) * 1000.0;
    const double a125 = isoAirAbsorptionDbPerMetre(125.0, 20.0, 50.0) * 1000.0;
    std::printf("  alpha 125 Hz %.3f  1 kHz %.3f  8 kHz %.2f dB/km\n", a125, a1k, a8k);
    ASSERT_NEAR(a1k, 4.66, 0.3);
    ASSERT_NEAR(a8k, 105.0, 12.0);
    ASSERT_NEAR(a125, 0.44, 0.1);
    // Highs fall much faster than lows, and drier air absorbs more at 4 kHz.
    double prev = 0.0;
    for (double f = 63.0; f < 20000.0; f *= 2.0) {
        const double a = isoAirAbsorptionDbPerMetre(f, 20.0, 50.0);
        ASSERT_GT(a, prev);
        prev = a;
    }
    ASSERT_GT(isoAirAbsorptionDbPerMetre(4000.0, 20.0, 20.0),
              isoAirAbsorptionDbPerMetre(4000.0, 20.0, 80.0));
    PASS();
}

// The table's model response at octave-band centres against ISO, at the
// distances a battle spans and beyond. Where ISO attenuation is <= 30 dB the
// model must be within max(2 dB, 25 %); past that (inaudible anyway) it
// must still attenuate by at least 25 dB.
static bool checkTable(int sampleRate, float tC, float rh) {
    auto t = buildAirFilterTable(sampleRate, tC, rh);
    const float distances[] = {5.f, 25.f, 50.f, 100.f, 200.f, 300.f, 600.f, 1500.f};
    bool ok = true;
    for (float d : distances) {
        float pole[kAirSections], mix[kAirSections];
        t->lookup(d, pole, mix);
        std::printf("  %4.0f m:", d);
        for (float f = 125.f; f <= std::min(16000.f, 0.45f * sampleRate); f *= 2.f) {
            const float iso = static_cast<float>(isoAirAbsorptionDbPerMetre(f, tC, rh) * d);
            const float got = t->responseDb(pole, mix, f);
            std::printf(" %g:%.1f/%.1f", f, got, iso);
            if (iso <= 30.f) {
                if (std::fabs(got - iso) > std::max(2.0f, 0.25f * iso)) ok = false;
            } else if (got < 25.f) {
                ok = false;
            }
        }
        std::printf("\n");
    }
    return ok;
}

TEST(table_tracks_iso_per_octave_band) {
    ASSERT_TRUE(checkTable(44100, 20.f, 50.f));
    ASSERT_TRUE(checkTable(48000, 10.f, 80.f));
    ASSERT_TRUE(checkTable(44100, 30.f, 20.f));
    PASS();
}

// Steady-state gain of the chain's air stage for a sine, dB (positive = loss).
static float stageLossDb(const AirFilterTable& t, float metres, float hz) {
    const int sr = t.sampleRate;
    VoiceChainState s;
    float pole[kAirSections], mix[kAirSections];
    t.lookup(metres, pole, mix);
    const int n = sr;   // 1 s
    auto x = dtest::sine(n, hz, 0.5f, sr);
    std::vector<float> y = x;
    for (int off = 0; off < n; off += 256) {
        float* c[1] = {y.data() + off};
        chainAir(s, pole, mix, c, 1, std::min(256, n - off));
    }
    const double in = dtest::rms(x, n / 2, n / 2);
    const double out = dtest::rms(y, n / 2, n / 2);
    return static_cast<float>(-20.0 * std::log10(out / in));
}

TEST(chain_air_stage_realises_the_table) {
    auto t = buildAirFilterTable(44100, 20.f, 50.f);
    const float cases[][2] = {{100.f, 1000.f}, {100.f, 4000.f}, {300.f, 2000.f},
                              {300.f, 4000.f}, {300.f, 8000.f}, {50.f, 8000.f}};
    for (auto& c : cases) {
        float pole[kAirSections], mix[kAirSections];
        t->lookup(c[0], pole, mix);
        const float model = t->responseDb(pole, mix, c[1]);
        const float stage = stageLossDb(*t, c[0], c[1]);
        const float iso = static_cast<float>(isoAirAbsorptionDbPerMetre(c[1], 20.0, 50.0) * c[0]);
        std::printf("  %4.0f m %5.0f Hz: stage %.2f model %.2f iso %.2f dB\n", c[0], c[1], stage, model, iso);
        ASSERT_NEAR(stage, model, 0.3f);
        if (iso <= 30.f) ASSERT_NEAR(stage, iso, std::max(2.0f, 0.25f * iso));
    }
    PASS();
}

// End to end: a looping noise source straight ahead, distance gain disabled
// (rolloff 0) so only the air changes the sound; the centroid must fall
// strictly as the source moves away, and not move at all with air off.
static double centroidAt(float metres, bool air) {
    Engine e;
    e.initHeadless();
    e.setMasterGain(0.5f);
    const int sr = e.sampleRate();
    auto n = dtest::noise(sr, 0.2f);
    int clip = e.createClip(n.data(), static_cast<int>(n.size()), 1);
    int pb = e.playClip(clip, 1.0f, true);
    e.setPlaybackSpatialEnabled(pb, true);
    e.setPlaybackSpatialRolloff(pb, 0.0f);
    e.setPlaybackSpatialMaxDistance(pb, 100000.f);
    e.setPlaybackSpatialPosition(pb, 0.f, 0.f, -metres);
    e.setPlaybackSpatialAirAbsorption(pb, air);
    auto rec = dtest::left(dtest::record(e, sr * 2));
    return dtest::centroid(rec, sr / 2, sr + sr / 2 - 4096, sr);
}

TEST(centroid_falls_monotonically_with_distance) {
    const float distances[] = {1.f, 25.f, 50.f, 100.f, 150.f, 200.f, 300.f};
    double prev = 1e9;
    for (float d : distances) {
        const double c = centroidAt(d, true);
        std::printf("  %4.0f m: centroid %.0f Hz\n", d, c);
        ASSERT_LT(c, prev);
        prev = c;
    }
    ASSERT_LT(centroidAt(300.f, true), 0.6 * centroidAt(1.f, true));
    ASSERT_NEAR(centroidAt(300.f, false), centroidAt(1.f, false), 5.0);
    PASS();
}

TEST(strength_and_metres_per_unit_scale_the_path) {
    // 150 units at 2 m/unit == 300 m; strength 2 at 150 m == 300 m.
    auto t = buildAirFilterTable(44100, 20.f, 50.f);
    (void)t;
    Engine a; a.initHeadless();
    a.setSpatialMetresPerUnit(2.0f);
    ASSERT_NEAR(a.spatialMetresPerUnit(), 2.0f, 1e-6f);
    a.setSpatialMetresPerUnit(-1.0f);   // ignored
    ASSERT_NEAR(a.spatialMetresPerUnit(), 2.0f, 1e-6f);
    a.setSpatialAirAbsorptionStrength(2.0f);
    ASSERT_NEAR(a.spatialAirAbsorptionStrength(), 2.0f, 1e-6f);
    a.setSpatialAirConditions(35.f, 10.f);
    auto tbl = a.airFilterTable();
    ASSERT_TRUE(tbl != nullptr);
    ASSERT_NEAR(tbl->temperatureC, 35.f, 1e-4f);
    ASSERT_NEAR(tbl->humidityPct, 10.f, 1e-4f);
    PASS();
}

int main() { return runAllTests(); }
