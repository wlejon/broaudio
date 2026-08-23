#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "broaudio/io/audio_file.h"

namespace broaudio {

// Real-time partitioned FFT convolution reverb.
// Performs frequency-domain overlap-save partitioned convolution.
// Zero dynamic allocations on the audio processing path (process / processStereo).
struct ConvolutionReverb {
    bool enabled = false;
    float mix = 0.5f;       // Wet/dry mix (0.0 = 100% dry, 1.0 = 100% wet)
    float gain = 1.0f;      // Output gain scalar
    bool normalize = true;  // Normalize IR energy on load

    ConvolutionReverb();
    ~ConvolutionReverb() = default;

    // Initialize engine for given sample rate and partition block size (must be power of 2, default 256).
    void init(int sampleRate, int partitionSize = 256);

    // Load impulse response from raw PCM buffers.
    // channels: 1 = mono IR (applied to both L and R), 2 = stereo IR (channel 0 -> L, channel 1 -> R).
    // interleaved: if channels == 2, true if samples are [L0, R0, L1, R1...], false if planar [L..., R...].
    bool loadImpulseResponse(const float* samples, int numFrames, int channels, int sampleRate = 0, bool interleaved = true);

    // Load impulse response from AudioFileData (e.g. decoded WAV/FLAC/Vorbis).
    bool loadImpulseResponse(const AudioFileData& fileData);

    // Load impulse response from audio file on disk (.wav, .flac, .ogg, etc.).
    bool loadImpulseResponse(const char* path);
    bool loadImpulseResponse(const std::string& path) { return loadImpulseResponse(path.c_str()); }

    // Real-time processing methods. Zero allocation, RT-safe.
    // Interleaved stereo processing (in-place). Buffer contains 2 * numFrames floats [L0, R0, L1, R1, ...].
    void processStereo(float* buffer, int numFrames);

    // Mono processing (in-place). Buffer contains numFrames floats.
    void process(float* buffer, int numFrames);

    // Planar stereo processing (separate input and output buffers).
    void process(const float* inL, const float* inR, float* outL, float* outR, int numFrames);

    // Process exactly 1 partition block (partitionSize frames) directly without FIFO buffering.
    void processBlock(const float* inL, const float* inR, float* outL, float* outR);
    void processBlockStereo(float* interleavedBuffer);

    // Clear / reset all internal delay lines, FIFO buffers, and spectral histories.
    void clear();
    void reset() { clear(); }

    // Status queries
    bool isLoaded() const { return numPartitions_ > 0; }
    int partitionSize() const { return partitionSize_; }
    int numPartitions() const { return numPartitions_; }
    int irChannels() const { return irChannels_; }
    int irLength() const { return irLength_; }
    int sampleRate() const { return sampleRate_; }

private:
    int sampleRate_ = 44100;
    int partitionSize_ = 256;    // B (power of 2)
    int fftSize_ = 512;          // N = 2 * B
    int numPartitions_ = 0;      // P = ceil(irLength / B)
    int irChannels_ = 0;         // 1 (mono) or 2 (stereo)
    int irLength_ = 0;           // Original IR frame count

    // Frequency domain IR partitions: [channel] -> size: numPartitions_ * fftSize_
    std::vector<float> irPartitionsReal_[2];
    std::vector<float> irPartitionsImag_[2];

    // Input frequency domain ring buffer: [channel] -> size: numPartitions_ * fftSize_
    std::vector<float> inputFftReal_[2];
    std::vector<float> inputFftImag_[2];
    int inputRingHead_ = 0;

    // Previous input block of size partitionSize_ for overlap-save
    std::vector<float> prevInput_[2];

    // Pre-allocated scratch buffers for FFT operations (size = fftSize_)
    std::vector<float> scratchReal_;
    std::vector<float> scratchImag_;
    std::vector<float> scratchAccumReal_;
    std::vector<float> scratchAccumImag_;

    // Temporary storage for single block output (size = partitionSize_)
    std::vector<float> blockOutL_;
    std::vector<float> blockOutR_;
    std::vector<float> blockInL_;
    std::vector<float> blockInR_;

    // FIFO ring buffers for arbitrary block size processing
    std::vector<float> inFifoL_;
    std::vector<float> inFifoR_;
    std::vector<float> outFifoL_;
    std::vector<float> outFifoR_;
    int inFifoWrite_ = 0;
    int inFifoRead_ = 0;
    int inFifoCount_ = 0;
    int outFifoWrite_ = 0;
    int outFifoRead_ = 0;
    int outFifoCount_ = 0;
    int fifoCapacity_ = 0;

    void allocateBuffers();
    void processOneBlock(const float* inL, const float* inR, float* outL, float* outR);
};

} // namespace broaudio
