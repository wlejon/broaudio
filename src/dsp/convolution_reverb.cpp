#include "broaudio/dsp/convolution_reverb.h"
#include "broaudio/dsp/fft.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace broaudio {

static int nextPowerOf2(int v) {
    if (v <= 1) return 1;
    return 1 << (32 - std::countl_zero(static_cast<uint32_t>(v - 1)));
}

ConvolutionReverb::ConvolutionReverb() {
    init(44100, 256);
}

void ConvolutionReverb::init(int sampleRate, int partitionSize) {
    sampleRate_ = (sampleRate > 0) ? sampleRate : 44100;
    partitionSize_ = nextPowerOf2(std::max(16, partitionSize));
    fftSize_ = 2 * partitionSize_;
    allocateBuffers();
}

void ConvolutionReverb::allocateBuffers() {
    scratchReal_.assign(fftSize_, 0.0f);
    scratchImag_.assign(fftSize_, 0.0f);
    scratchAccumReal_.assign(fftSize_, 0.0f);
    scratchAccumImag_.assign(fftSize_, 0.0f);

    blockInL_.assign(partitionSize_, 0.0f);
    blockInR_.assign(partitionSize_, 0.0f);
    blockOutL_.assign(partitionSize_, 0.0f);
    blockOutR_.assign(partitionSize_, 0.0f);

    prevInput_[0].assign(partitionSize_, 0.0f);
    prevInput_[1].assign(partitionSize_, 0.0f);

    int p = std::max(1, numPartitions_);
    for (int c = 0; c < 2; ++c) {
        inputFftReal_[c].assign(p * fftSize_, 0.0f);
        inputFftImag_[c].assign(p * fftSize_, 0.0f);
    }

    fifoCapacity_ = std::max(8192, 4 * partitionSize_);
    inFifoL_.assign(fifoCapacity_, 0.0f);
    inFifoR_.assign(fifoCapacity_, 0.0f);
    outFifoL_.assign(fifoCapacity_, 0.0f);
    outFifoR_.assign(fifoCapacity_, 0.0f);

    clear();
}

bool ConvolutionReverb::loadImpulseResponse(const float* samples, int numFrames, int channels, int sampleRate, bool interleaved) {
    if (!samples || numFrames <= 0 || channels <= 0) {
        return false;
    }

    if (sampleRate > 0) {
        sampleRate_ = sampleRate;
    }

    irLength_ = numFrames;
    irChannels_ = std::clamp(channels, 1, 2);
    numPartitions_ = (numFrames + partitionSize_ - 1) / partitionSize_;
    if (numPartitions_ <= 0) numPartitions_ = 1;

    allocateBuffers();

    for (int c = 0; c < irChannels_; ++c) {
        irPartitionsReal_[c].assign(numPartitions_ * fftSize_, 0.0f);
        irPartitionsImag_[c].assign(numPartitions_ * fftSize_, 0.0f);
    }

    // Measure energy for normalization
    float energyL = 0.0f;
    float energyR = 0.0f;
    for (int i = 0; i < numFrames; ++i) {
        float sL = interleaved ? samples[i * channels + 0] : samples[i];
        energyL += sL * sL;
        if (irChannels_ > 1) {
            float sR = interleaved ? samples[i * channels + 1] : samples[numFrames + i];
            energyR += sR * sR;
        }
    }

    float maxEnergy = (irChannels_ > 1) ? std::max(energyL, energyR) : energyL;
    float normScale = 1.0f;
    if (normalize && maxEnergy > 1e-12f) {
        normScale = 1.0f / std::sqrt(maxEnergy);
    }

    // Partition and FFT the impulse response
    std::vector<float> timeBuf(fftSize_, 0.0f);
    std::vector<float> imagBuf(fftSize_, 0.0f);

    for (int c = 0; c < irChannels_; ++c) {
        for (int p = 0; p < numPartitions_; ++p) {
            std::fill(timeBuf.begin(), timeBuf.end(), 0.0f);
            std::fill(imagBuf.begin(), imagBuf.end(), 0.0f);

            int startFrame = p * partitionSize_;
            for (int k = 0; k < partitionSize_; ++k) {
                int frameIdx = startFrame + k;
                if (frameIdx < numFrames) {
                    float val = 0.0f;
                    if (interleaved) {
                        val = samples[frameIdx * channels + c];
                    } else {
                        val = samples[c * numFrames + frameIdx];
                    }
                    timeBuf[k] = val * normScale;
                }
            }

            fft(timeBuf.data(), imagBuf.data(), fftSize_);

            float* dstR = &irPartitionsReal_[c][p * fftSize_];
            float* dstI = &irPartitionsImag_[c][p * fftSize_];
            std::memcpy(dstR, timeBuf.data(), fftSize_ * sizeof(float));
            std::memcpy(dstI, imagBuf.data(), fftSize_ * sizeof(float));
        }
    }

    clear();
    return true;
}

bool ConvolutionReverb::loadImpulseResponse(const AudioFileData& fileData) {
    if (!fileData.valid()) {
        return false;
    }
    return loadImpulseResponse(fileData.samples.data(), fileData.numFrames, fileData.channels, fileData.sampleRate, true);
}

bool ConvolutionReverb::loadImpulseResponse(const char* path) {
    AudioFileData data = loadAudioFile(path);
    if (!data.valid()) {
        return false;
    }
    return loadImpulseResponse(data);
}

void ConvolutionReverb::clear() {
    inputRingHead_ = 0;
    for (int c = 0; c < 2; ++c) {
        std::fill(prevInput_[c].begin(), prevInput_[c].end(), 0.0f);
        std::fill(inputFftReal_[c].begin(), inputFftReal_[c].end(), 0.0f);
        std::fill(inputFftImag_[c].begin(), inputFftImag_[c].end(), 0.0f);
    }
    inFifoWrite_ = 0;
    inFifoRead_ = 0;
    inFifoCount_ = 0;
    outFifoWrite_ = 0;
    outFifoRead_ = 0;
    outFifoCount_ = 0;
    std::fill(inFifoL_.begin(), inFifoL_.end(), 0.0f);
    std::fill(inFifoR_.begin(), inFifoR_.end(), 0.0f);
    std::fill(outFifoL_.begin(), outFifoL_.end(), 0.0f);
    std::fill(outFifoR_.begin(), outFifoR_.end(), 0.0f);
}

void ConvolutionReverb::processOneBlock(const float* inL, const float* inR, float* outL, float* outR) {
    if (numPartitions_ <= 0) {
        if (outL && inL) std::memcpy(outL, inL, partitionSize_ * sizeof(float));
        if (outR && inR) std::memcpy(outR, inR, partitionSize_ * sizeof(float));
        return;
    }

    for (int c = 0; c < 2; ++c) {
        const float* in = (c == 0) ? inL : inR;
        float* out = (c == 0) ? outL : outR;
        int irChan = (c < irChannels_) ? c : 0;

        // Overlap-save: [prevInput(B), in(B)]
        std::memcpy(scratchReal_.data(), prevInput_[c].data(), partitionSize_ * sizeof(float));
        if (in) {
            std::memcpy(scratchReal_.data() + partitionSize_, in, partitionSize_ * sizeof(float));
            std::memcpy(prevInput_[c].data(), in, partitionSize_ * sizeof(float));
        } else {
            std::fill(scratchReal_.data() + partitionSize_, scratchReal_.data() + fftSize_, 0.0f);
            std::fill(prevInput_[c].begin(), prevInput_[c].end(), 0.0f);
        }
        std::fill(scratchImag_.begin(), scratchImag_.end(), 0.0f);

        // Forward FFT of input block
        fft(scratchReal_.data(), scratchImag_.data(), fftSize_);

        // Store into input history ring buffer
        float* headR = &inputFftReal_[c][inputRingHead_ * fftSize_];
        float* headI = &inputFftImag_[c][inputRingHead_ * fftSize_];
        std::memcpy(headR, scratchReal_.data(), fftSize_ * sizeof(float));
        std::memcpy(headI, scratchImag_.data(), fftSize_ * sizeof(float));

        // Frequency-domain multiply-accumulate across all partitions
        std::fill(scratchAccumReal_.begin(), scratchAccumReal_.end(), 0.0f);
        std::fill(scratchAccumImag_.begin(), scratchAccumImag_.end(), 0.0f);

        for (int p = 0; p < numPartitions_; ++p) {
            int slot = (inputRingHead_ - p + numPartitions_) % numPartitions_;
            const float* xR = &inputFftReal_[c][slot * fftSize_];
            const float* xI = &inputFftImag_[c][slot * fftSize_];
            const float* hR = &irPartitionsReal_[irChan][p * fftSize_];
            const float* hI = &irPartitionsImag_[irChan][p * fftSize_];

            for (int k = 0; k < fftSize_; ++k) {
                scratchAccumReal_[k] += xR[k] * hR[k] - xI[k] * hI[k];
                scratchAccumImag_[k] += xR[k] * hI[k] + xI[k] * hR[k];
            }
        }

        // IFFT: conj -> FFT -> scale
        for (int k = 0; k < fftSize_; ++k) {
            scratchAccumImag_[k] = -scratchAccumImag_[k];
        }
        fft(scratchAccumReal_.data(), scratchAccumImag_.data(), fftSize_);

        float invN = 1.0f / static_cast<float>(fftSize_);
        if (out) {
            for (int k = 0; k < partitionSize_; ++k) {
                out[k] = scratchAccumReal_[partitionSize_ + k] * invN;
            }
        }
    }

    inputRingHead_ = (inputRingHead_ + 1) % numPartitions_;
}

void ConvolutionReverb::processBlock(const float* inL, const float* inR, float* outL, float* outR) {
    if (!enabled || numPartitions_ <= 0) {
        if (outL && inL && outL != inL) std::memcpy(outL, inL, partitionSize_ * sizeof(float));
        if (outR && inR && outR != inR) std::memcpy(outR, inR, partitionSize_ * sizeof(float));
        return;
    }

    processOneBlock(inL, inR, blockOutL_.data(), blockOutR_.data());

    float dryGain = 1.0f - mix;
    float wetGain = mix;
    for (int i = 0; i < partitionSize_; ++i) {
        float dryL = inL ? inL[i] : 0.0f;
        float dryR = inR ? inR[i] : 0.0f;
        if (outL) outL[i] = (dryGain * dryL + wetGain * blockOutL_[i]) * gain;
        if (outR) outR[i] = (dryGain * dryR + wetGain * blockOutR_[i]) * gain;
    }
}

void ConvolutionReverb::processBlockStereo(float* interleavedBuffer) {
    if (!interleavedBuffer) return;
    if (!enabled || numPartitions_ <= 0) return;

    for (int i = 0; i < partitionSize_; ++i) {
        blockInL_[i] = interleavedBuffer[2 * i + 0];
        blockInR_[i] = interleavedBuffer[2 * i + 1];
    }

    processOneBlock(blockInL_.data(), blockInR_.data(), blockOutL_.data(), blockOutR_.data());

    float dryGain = 1.0f - mix;
    float wetGain = mix;
    for (int i = 0; i < partitionSize_; ++i) {
        interleavedBuffer[2 * i + 0] = (dryGain * blockInL_[i] + wetGain * blockOutL_[i]) * gain;
        interleavedBuffer[2 * i + 1] = (dryGain * blockInR_[i] + wetGain * blockOutR_[i]) * gain;
    }
}

void ConvolutionReverb::processStereo(float* buffer, int numFrames) {
    if (!buffer || numFrames <= 0) return;
    if (!enabled || numPartitions_ <= 0) return;

    float dryGain = 1.0f - mix;
    float wetGain = mix;

    // Push input samples to input FIFO
    for (int i = 0; i < numFrames; ++i) {
        inFifoL_[inFifoWrite_] = buffer[2 * i + 0];
        inFifoR_[inFifoWrite_] = buffer[2 * i + 1];
        inFifoWrite_ = (inFifoWrite_ + 1) % fifoCapacity_;
    }
    inFifoCount_ += numFrames;

    // Process all full partition blocks
    while (inFifoCount_ >= partitionSize_) {
        for (int k = 0; k < partitionSize_; ++k) {
            blockInL_[k] = inFifoL_[inFifoRead_];
            blockInR_[k] = inFifoR_[inFifoRead_];
            inFifoRead_ = (inFifoRead_ + 1) % fifoCapacity_;
        }
        inFifoCount_ -= partitionSize_;

        processOneBlock(blockInL_.data(), blockInR_.data(), blockOutL_.data(), blockOutR_.data());

        for (int k = 0; k < partitionSize_; ++k) {
            outFifoL_[outFifoWrite_] = blockOutL_[k];
            outFifoR_[outFifoWrite_] = blockOutR_[k];
            outFifoWrite_ = (outFifoWrite_ + 1) % fifoCapacity_;
        }
        outFifoCount_ += partitionSize_;
    }

    // Write output samples mixing dry and wet
    for (int i = 0; i < numFrames; ++i) {
        float dryL = buffer[2 * i + 0];
        float dryR = buffer[2 * i + 1];
        float wetL = 0.0f;
        float wetR = 0.0f;

        if (outFifoCount_ > 0) {
            wetL = outFifoL_[outFifoRead_];
            wetR = outFifoR_[outFifoRead_];
            outFifoRead_ = (outFifoRead_ + 1) % fifoCapacity_;
            outFifoCount_--;
        }

        buffer[2 * i + 0] = (dryGain * dryL + wetGain * wetL) * gain;
        buffer[2 * i + 1] = (dryGain * dryR + wetGain * wetR) * gain;
    }
}

void ConvolutionReverb::process(float* buffer, int numFrames) {
    if (!buffer || numFrames <= 0) return;
    if (!enabled || numPartitions_ <= 0) return;

    float dryGain = 1.0f - mix;
    float wetGain = mix;

    for (int i = 0; i < numFrames; ++i) {
        inFifoL_[inFifoWrite_] = buffer[i];
        inFifoR_[inFifoWrite_] = buffer[i];
        inFifoWrite_ = (inFifoWrite_ + 1) % fifoCapacity_;
    }
    inFifoCount_ += numFrames;

    while (inFifoCount_ >= partitionSize_) {
        for (int k = 0; k < partitionSize_; ++k) {
            blockInL_[k] = inFifoL_[inFifoRead_];
            blockInR_[k] = inFifoR_[inFifoRead_];
            inFifoRead_ = (inFifoRead_ + 1) % fifoCapacity_;
        }
        inFifoCount_ -= partitionSize_;

        processOneBlock(blockInL_.data(), blockInR_.data(), blockOutL_.data(), blockOutR_.data());

        for (int k = 0; k < partitionSize_; ++k) {
            outFifoL_[outFifoWrite_] = blockOutL_[k];
            outFifoR_[outFifoWrite_] = blockOutR_[k];
            outFifoWrite_ = (outFifoWrite_ + 1) % fifoCapacity_;
        }
        outFifoCount_ += partitionSize_;
    }

    for (int i = 0; i < numFrames; ++i) {
        float dry = buffer[i];
        float wet = 0.0f;
        if (outFifoCount_ > 0) {
            wet = outFifoL_[outFifoRead_];
            outFifoRead_ = (outFifoRead_ + 1) % fifoCapacity_;
            outFifoCount_--;
        }
        buffer[i] = (dryGain * dry + wetGain * wet) * gain;
    }
}

void ConvolutionReverb::process(const float* inL, const float* inR, float* outL, float* outR, int numFrames) {
    if (numFrames <= 0) return;
    if (!enabled || numPartitions_ <= 0) {
        if (outL && inL && outL != inL) std::memcpy(outL, inL, numFrames * sizeof(float));
        if (outR && inR && outR != inR) std::memcpy(outR, inR, numFrames * sizeof(float));
        return;
    }

    float dryGain = 1.0f - mix;
    float wetGain = mix;

    for (int i = 0; i < numFrames; ++i) {
        inFifoL_[inFifoWrite_] = inL ? inL[i] : 0.0f;
        inFifoR_[inFifoWrite_] = inR ? inR[i] : 0.0f;
        inFifoWrite_ = (inFifoWrite_ + 1) % fifoCapacity_;
    }
    inFifoCount_ += numFrames;

    while (inFifoCount_ >= partitionSize_) {
        for (int k = 0; k < partitionSize_; ++k) {
            blockInL_[k] = inFifoL_[inFifoRead_];
            blockInR_[k] = inFifoR_[inFifoRead_];
            inFifoRead_ = (inFifoRead_ + 1) % fifoCapacity_;
        }
        inFifoCount_ -= partitionSize_;

        processOneBlock(blockInL_.data(), blockInR_.data(), blockOutL_.data(), blockOutR_.data());

        for (int k = 0; k < partitionSize_; ++k) {
            outFifoL_[outFifoWrite_] = blockOutL_[k];
            outFifoR_[outFifoWrite_] = blockOutR_[k];
            outFifoWrite_ = (outFifoWrite_ + 1) % fifoCapacity_;
        }
        outFifoCount_ += partitionSize_;
    }

    for (int i = 0; i < numFrames; ++i) {
        float dryL = inL ? inL[i] : 0.0f;
        float dryR = inR ? inR[i] : 0.0f;
        float wetL = 0.0f;
        float wetR = 0.0f;
        if (outFifoCount_ > 0) {
            wetL = outFifoL_[outFifoRead_];
            wetR = outFifoR_[outFifoRead_];
            outFifoRead_ = (outFifoRead_ + 1) % fifoCapacity_;
            outFifoCount_--;
        }
        if (outL) outL[i] = (dryGain * dryL + wetGain * wetL) * gain;
        if (outR) outR[i] = (dryGain * dryR + wetGain * wetR) * gain;
    }
}

} // namespace broaudio
