#include "test_harness.h"
#include "broaudio/mix/bus.h"

#include <cmath>

using namespace broaudio;

TEST(metering_defaults_zero) {
    Bus bus;
    ASSERT_NEAR(bus.peakL.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.peakR.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.rmsL.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.rmsR.load(), 0.0f, 0.0001f);
    PASS();
}

TEST(metering_peak_tracks_maximum) {
    Bus bus;
    bus.initAudioState(44100, 256);

    const int frames = 128;
    for (int i = 0; i < frames; i++) {
        bus.buffer[i * 2]     = (i == 50) ? 0.8f : 0.1f;
        bus.buffer[i * 2 + 1] = (i == 70) ? 0.6f : 0.05f;
    }

    // Simulate what updateBusMeters does
    float pL = 0.0f, pR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < frames; i++) {
        float l = std::fabs(bus.buffer[i * 2]);
        float r = std::fabs(bus.buffer[i * 2 + 1]);
        if (l > pL) pL = l;
        if (r > pR) pR = r;
        sumSqL += bus.buffer[i * 2] * bus.buffer[i * 2];
        sumSqR += bus.buffer[i * 2 + 1] * bus.buffer[i * 2 + 1];
    }
    bus.peakL.store(pL);
    bus.peakR.store(pR);
    float invN = 1.0f / static_cast<float>(frames);
    bus.rmsL.store(std::sqrt(sumSqL * invN));
    bus.rmsR.store(std::sqrt(sumSqR * invN));

    ASSERT_NEAR(bus.peakL.load(), 0.8f, 0.0001f);
    ASSERT_NEAR(bus.peakR.load(), 0.6f, 0.0001f);
    ASSERT_GT(bus.rmsL.load(), 0.0f);
    ASSERT_GT(bus.rmsR.load(), 0.0f);
    ASSERT_LT(bus.rmsL.load(), bus.peakL.load());
    ASSERT_LT(bus.rmsR.load(), bus.peakR.load());
    PASS();
}

TEST(metering_negative_samples_peak_is_absolute) {
    Bus bus;
    bus.initAudioState(44100, 64);

    const int frames = 32;
    for (int i = 0; i < frames; i++) {
        bus.buffer[i * 2]     = -0.9f;
        bus.buffer[i * 2 + 1] = -0.5f;
    }

    float pL = 0.0f, pR = 0.0f;
    for (int i = 0; i < frames; i++) {
        float l = std::fabs(bus.buffer[i * 2]);
        float r = std::fabs(bus.buffer[i * 2 + 1]);
        if (l > pL) pL = l;
        if (r > pR) pR = r;
    }
    bus.peakL.store(pL);
    bus.peakR.store(pR);

    ASSERT_NEAR(bus.peakL.load(), 0.9f, 0.0001f);
    ASSERT_NEAR(bus.peakR.load(), 0.5f, 0.0001f);
    PASS();
}

TEST(metering_silent_buffer_is_zero) {
    Bus bus;
    bus.initAudioState(44100, 64);
    bus.clearBuffer(32);

    float pL = 0.0f, pR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    const int frames = 32;
    for (int i = 0; i < frames; i++) {
        float l = std::fabs(bus.buffer[i * 2]);
        float r = std::fabs(bus.buffer[i * 2 + 1]);
        if (l > pL) pL = l;
        if (r > pR) pR = r;
        sumSqL += bus.buffer[i * 2] * bus.buffer[i * 2];
        sumSqR += bus.buffer[i * 2 + 1] * bus.buffer[i * 2 + 1];
    }
    bus.peakL.store(pL);
    bus.peakR.store(pR);
    bus.rmsL.store(std::sqrt(sumSqL / frames));
    bus.rmsR.store(std::sqrt(sumSqR / frames));

    ASSERT_NEAR(bus.peakL.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.peakR.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.rmsL.load(), 0.0f, 0.0001f);
    ASSERT_NEAR(bus.rmsR.load(), 0.0f, 0.0001f);
    PASS();
}

int main() { return runAllTests(); }
