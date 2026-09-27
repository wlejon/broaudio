#pragma once

#include <memory>
#include <vector>

namespace broaudio {

struct ParamRamp;

// Uniformly partitioned overlap-save FFT convolution (UPOLS), sized for
// multi-second outdoor tails on a bus.
//
// Construction (create) does every allocation and the FFT of each IR
// partition, so it runs off the audio thread; the result is published to the
// audio thread whole (AtomicSharedPtr) and never resized there. process*()
// is RT-safe: no allocation, no locks, fixed work per partition block.
//
// Cost per output sample and channel is ~(irFrames / B) complex
// multiply-adds on N/2+1 bins (only the non-redundant half of the real
// spectrum) plus two N-point FFTs per B samples, N = 2B. Latency is exactly B
// frames (the input FIFO): at the default B = 256 that is 5.8 ms at 44.1 kHz,
// inaudible on a reverb send.
//
// IRs: mono (both channels convolved with it) or stereo (left with channel 0,
// right with channel 1), interleaved. The IR is used as given, not normalised:
// its level is the reverb's level.
class PartitionedConvolver {
public:
    static constexpr int kDefaultBlock = 256;

    // Returns nullptr on bad arguments (no samples, channels not 1 or 2) or an
    // allocation that cannot be met.
    static std::shared_ptr<PartitionedConvolver>
    create(const float* ir, int frames, int channels, int blockSize = kDefaultBlock);

    int blockSize() const { return B_; }
    int latencyFrames() const { return B_; }
    int partitions() const { return P_; }
    int irChannels() const { return irCh_; }
    int irFrames() const { return irFrames_; }

    // Wet-only planar processing: outL/outR receive the convolution of inL/inR
    // delayed by latencyFrames(). In and out may alias.
    void processWet(const float* inL, const float* inR, float* outL, float* outR, int n);

    // Bus insert: in-place on interleaved stereo, out = dry*(1-mix) + wet*mix,
    // with `mix` advanced per sample.
    void processInterleaved(float* buf, int n, ParamRamp& mix);

    // Silence the history (FIFO and spectra) without reallocating.
    void clear();

private:
    PartitionedConvolver() = default;
    void fft(float* re, float* im) const;   // in-place, forward, size N
    void processBlock();

    int B_ = 0, N_ = 0, P_ = 0, bins_ = 0, irCh_ = 1, irFrames_ = 0;
    std::vector<float> cosT_, sinT_;
    std::vector<int> bitrev_;
    std::vector<float> irRe_[2], irIm_[2];    // [P][bins]
    std::vector<float> fdlRe_[2], fdlIm_[2];  // frequency-domain delay line, [P][bins]
    int head_ = 0;
    std::vector<float> time_[2];              // [prev B | cur B]
    std::vector<float> outBlk_[2];            // wet output of the last block
    std::vector<float> workRe_, workIm_, accRe_, accIm_;
    int pos_ = 0;                             // FIFO position within the block
};

} // namespace broaudio
