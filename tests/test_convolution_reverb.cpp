#include "test_harness.h"
#include "broaudio/dsp/convolution_reverb.h"
#include <cmath>
#include <vector>
#include <algorithm>

using namespace broaudio;

TEST(convolution_reverb_init) {
    ConvolutionReverb conv;
    conv.init(48000, 128);
    ASSERT_EQ(conv.sampleRate(), 48000);
    ASSERT_EQ(conv.partitionSize(), 128);
    ASSERT_FALSE(conv.isLoaded());
    PASS();
}

TEST(convolution_reverb_delta_identity) {
    // A unit impulse IR [1.0, 0.0, ...] should reproduce the input signal exactly
    const int B = 64;
    ConvolutionReverb conv;
    conv.init(44100, B);
    conv.normalize = false;
    conv.mix = 1.0f;
    conv.enabled = true;

    // Mono delta impulse
    std::vector<float> ir = {1.0f, 0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(conv.loadImpulseResponse(ir.data(), static_cast<int>(ir.size()), 1, 44100));
    ASSERT_TRUE(conv.isLoaded());

    std::vector<float> input(B * 4);
    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = std::sin(static_cast<float>(i) * 0.1f);
    }

    std::vector<float> output = input;
    conv.process(output.data(), static_cast<int>(output.size()));

    for (size_t i = 0; i < input.size(); ++i) {
        ASSERT_NEAR(output[i], input[i], 1e-4f);
    }
    PASS();
}

TEST(convolution_reverb_exact_linear_convolution) {
    // Compare partitioned convolution against direct time-domain convolution
    const int B = 64;
    ConvolutionReverb conv;
    conv.init(44100, B);
    conv.normalize = false;
    conv.mix = 1.0f;
    conv.enabled = true;

    // Exponentially decaying noise/impulse response spanning multiple partitions
    const int irLen = 200; // > 3 * B
    std::vector<float> ir(irLen);
    for (int i = 0; i < irLen; ++i) {
        ir[i] = std::exp(-static_cast<float>(i) / 40.0f) * std::cos(static_cast<float>(i) * 0.3f);
    }

    ASSERT_TRUE(conv.loadImpulseResponse(ir.data(), irLen, 1, 44100));
    ASSERT_EQ(conv.numPartitions(), 4); // ceil(200 / 64) = 4

    // Input signal
    const int numFrames = 512;
    std::vector<float> input(numFrames);
    for (int i = 0; i < numFrames; ++i) {
        input[i] = std::sin(static_cast<float>(i) * 0.05f) + 0.5f * std::cos(static_cast<float>(i) * 0.12f);
    }

    // Direct linear convolution reference: y[n] = sum_{k=0}^{irLen-1} x[n - k] * ir[k]
    std::vector<float> expected(numFrames, 0.0f);
    for (int n = 0; n < numFrames; ++n) {
        float sum = 0.0f;
        for (int k = 0; k < irLen; ++k) {
            if (n - k >= 0) {
                sum += input[n - k] * ir[k];
            }
        }
        expected[n] = sum;
    }

    // Run partitioned convolution
    std::vector<float> actual = input;
    conv.process(actual.data(), numFrames);

    // Verify bit-level / numerical match across all frames
    for (int n = 0; n < numFrames; ++n) {
        ASSERT_NEAR(actual[n], expected[n], 2e-4f);
    }
    PASS();
}

TEST(convolution_reverb_stereo_channels) {
    const int B = 128;
    ConvolutionReverb conv;
    conv.init(44100, B);
    conv.normalize = false;
    conv.mix = 1.0f;
    conv.enabled = true;

    // Stereo IR: Left channel has gain 0.5, Right channel has gain 0.25 and delay 2 samples
    const int irLen = 64;
    std::vector<float> irStereo(irLen * 2, 0.0f);
    irStereo[0 * 2 + 0] = 0.5f;   // L[0] = 0.5
    irStereo[2 * 2 + 1] = 0.25f;  // R[2] = 0.25

    ASSERT_TRUE(conv.loadImpulseResponse(irStereo.data(), irLen, 2, 44100, true));
    ASSERT_EQ(conv.irChannels(), 2);

    const int numFrames = 256;
    std::vector<float> stereoBuffer(numFrames * 2);
    for (int i = 0; i < numFrames; ++i) {
        stereoBuffer[2 * i + 0] = 1.0f; // Left DC = 1.0
        stereoBuffer[2 * i + 1] = 2.0f; // Right DC = 2.0
    }

    conv.processStereo(stereoBuffer.data(), numFrames);

    // Left should be 1.0 * 0.5 = 0.5
    // Right should be 2.0 * 0.25 = 0.5 (delayed by 2 samples)
    for (int i = 0; i < numFrames; ++i) {
        ASSERT_NEAR(stereoBuffer[2 * i + 0], 0.5f, 1e-4f);
        if (i >= 2) {
            ASSERT_NEAR(stereoBuffer[2 * i + 1], 0.5f, 1e-4f);
        }
    }
    PASS();
}

TEST(convolution_reverb_chunked_processing) {
    const int B = 64;
    ConvolutionReverb conv;
    conv.init(44100, B);
    conv.normalize = false;
    conv.mix = 1.0f;
    conv.enabled = true;

    std::vector<float> ir = {0.8f, 0.4f, 0.2f, 0.1f};
    ASSERT_TRUE(conv.loadImpulseResponse(ir.data(), static_cast<int>(ir.size()), 1, 44100));

    // Process in various block chunk sizes (multiples of B)
    std::vector<int> chunkSizes = {64, 128, 64, 256, 192, 64};
    int totalFrames = 0;
    for (int sz : chunkSizes) totalFrames += sz;

    std::vector<float> fullInput(totalFrames);
    for (int i = 0; i < totalFrames; ++i) {
        fullInput[i] = std::sin(static_cast<float>(i) * 0.08f);
    }

    // Reference with one contiguous run
    ConvolutionReverb convRef;
    convRef.init(44100, B);
    convRef.normalize = false;
    convRef.mix = 1.0f;
    convRef.enabled = true;
    convRef.loadImpulseResponse(ir.data(), static_cast<int>(ir.size()), 1, 44100);

    std::vector<float> refOutput = fullInput;
    convRef.process(refOutput.data(), totalFrames);

    // Chunked run
    std::vector<float> chunkedOutput = fullInput;
    int offset = 0;
    for (int sz : chunkSizes) {
        conv.process(chunkedOutput.data() + offset, sz);
        offset += sz;
    }

    // Both should produce identical results
    for (int i = 0; i < totalFrames; ++i) {
        ASSERT_NEAR(chunkedOutput[i], refOutput[i], 1e-4f);
    }
    PASS();
}

TEST(convolution_reverb_dry_wet_mix) {
    ConvolutionReverb conv;
    conv.init(44100, 64);
    conv.normalize = false;
    conv.enabled = true;

    std::vector<float> ir = {0.0f}; // zero IR -> 0 wet signal
    conv.loadImpulseResponse(ir.data(), 1, 1, 44100);

    std::vector<float> buf = {1.0f, 1.0f, 1.0f, 1.0f};
    
    // 100% Dry
    conv.mix = 0.0f;
    conv.process(buf.data(), 4);
    for (float v : buf) { ASSERT_NEAR(v, 1.0f, 1e-5f); }

    // 100% Wet (IR is 0 so wet is 0)
    conv.mix = 1.0f;
    conv.process(buf.data(), 4);
    for (float v : buf) { ASSERT_NEAR(v, 0.0f, 1e-5f); }

    // 50% Wet / 50% Dry
    buf = {1.0f, 1.0f, 1.0f, 1.0f};
    conv.mix = 0.5f;
    conv.process(buf.data(), 4);
    for (float v : buf) { ASSERT_NEAR(v, 0.5f, 1e-5f); }

    PASS();
}

TEST(convolution_reverb_clear_reset) {
    ConvolutionReverb conv;
    conv.init(44100, 64);
    conv.normalize = false;
    conv.mix = 1.0f;
    conv.enabled = true;

    std::vector<float> ir = {1.0f, 0.5f, 0.25f};
    conv.loadImpulseResponse(ir.data(), 3, 1, 44100);

    std::vector<float> buf = {1.0f, 1.0f, 1.0f, 1.0f};
    conv.process(buf.data(), 4);

    // Reset should clear state
    conv.clear();

    std::vector<float> zeroes(64, 0.0f);
    conv.process(zeroes.data(), 64);
    for (float v : zeroes) {
        ASSERT_NEAR(v, 0.0f, 1e-6f);
    }
    PASS();
}

int main() { return runAllTests(); }
