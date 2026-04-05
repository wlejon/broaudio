#include "test_harness.h"
#include "broaudio/dsp/compressor.h"
#include "broaudio/dsp/params.h"

#include <cmath>
#include <vector>

using namespace broaudio;

TEST(sidechain_compressor_ducks_with_loud_sidechain) {
    Compressor comp;
    comp.init(44100);
    comp.threshold = 0.3f;
    comp.ratio = 8.0f;

    const int frames = 512;
    std::vector<float> input(frames * 2);
    std::vector<float> sidechain(frames * 2);

    // Input: moderate level signal
    for (int i = 0; i < frames; i++) {
        input[i * 2] = 0.4f;
        input[i * 2 + 1] = 0.4f;
    }

    // Sidechain: loud signal that triggers compression
    for (int i = 0; i < frames; i++) {
        sidechain[i * 2] = 0.9f;
        sidechain[i * 2 + 1] = 0.9f;
    }

    // Copy input for comparison
    std::vector<float> original = input;

    comp.processStereoWithSidechain(input.data(), sidechain.data(), frames);

    // The loud sidechain should cause the compressor to reduce the input level
    // Check that the output is quieter than the input on the last frame
    int last = frames - 1;
    ASSERT_LT(std::fabs(input[last * 2]), std::fabs(original[last * 2]));
    PASS();
}

TEST(sidechain_compressor_no_duck_with_quiet_sidechain) {
    Compressor comp;
    comp.init(44100);
    comp.threshold = 0.5f;
    comp.ratio = 4.0f;

    const int frames = 512;
    std::vector<float> input(frames * 2);
    std::vector<float> sidechain(frames * 2);

    // Input: moderate level
    for (int i = 0; i < frames; i++) {
        input[i * 2] = 0.4f;
        input[i * 2 + 1] = 0.4f;
    }

    // Sidechain: quiet signal (below threshold)
    for (int i = 0; i < frames; i++) {
        sidechain[i * 2] = 0.1f;
        sidechain[i * 2 + 1] = 0.1f;
    }

    std::vector<float> original = input;
    comp.processStereoWithSidechain(input.data(), sidechain.data(), frames);

    // With sidechain below threshold, output should be nearly unchanged
    int last = frames - 1;
    ASSERT_NEAR(input[last * 2], original[last * 2], 0.01f);
    PASS();
}

TEST(sidechain_params_default_negative_one) {
    CompressorParams p;
    ASSERT_EQ(p.sidechainBusId.load(), -1);
    PASS();
}

TEST(sidechain_compressor_uses_sidechain_not_input) {
    // Verify the compressor uses the sidechain signal for detection,
    // not the input signal.
    Compressor comp;
    comp.init(44100);
    comp.threshold = 0.3f;
    comp.ratio = 10.0f;

    const int frames = 512;

    // Test 1: loud input, quiet sidechain → no compression
    std::vector<float> input1(frames * 2);
    std::vector<float> sc1(frames * 2);
    for (int i = 0; i < frames; i++) {
        input1[i * 2] = 0.8f;
        input1[i * 2 + 1] = 0.8f;
        sc1[i * 2] = 0.1f;
        sc1[i * 2 + 1] = 0.1f;
    }
    std::vector<float> orig1 = input1;
    comp.processStereoWithSidechain(input1.data(), sc1.data(), frames);
    int last = frames - 1;
    // Should be close to original since sidechain is quiet
    ASSERT_NEAR(input1[last * 2], orig1[last * 2], 0.05f);

    // Test 2: same loud input, loud sidechain → heavy compression
    comp.envelope = 0.0f; // reset
    std::vector<float> input2(frames * 2);
    std::vector<float> sc2(frames * 2);
    for (int i = 0; i < frames; i++) {
        input2[i * 2] = 0.8f;
        input2[i * 2 + 1] = 0.8f;
        sc2[i * 2] = 0.9f;
        sc2[i * 2 + 1] = 0.9f;
    }
    comp.processStereoWithSidechain(input2.data(), sc2.data(), frames);
    // Should be noticeably reduced
    ASSERT_LT(std::fabs(input2[last * 2]), 0.7f);

    PASS();
}

int main() { return runAllTests(); }
