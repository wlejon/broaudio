#pragma once

#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <cmath>
#include <functional>

namespace broaudio {

class AudioNode;

class AudioParam {
public:
    explicit AudioParam(float defaultValue = 1.0f, float minVal = -3.402823466e+38f, float maxVal = 3.402823466e+38f)
        : value_(defaultValue), defaultValue_(defaultValue), minValue_(minVal), maxValue_(maxVal) {}

    float value() const { return value_; }
    void setValue(float v) {
        value_ = std::clamp(v, minValue_, maxValue_);
        if (onChanged_) onChanged_(value_);
    }
    float defaultValue() const { return defaultValue_; }
    float minValue() const { return minValue_; }
    float maxValue() const { return maxValue_; }

    void setOnChanged(std::function<void(float)> cb) { onChanged_ = std::move(cb); }

private:
    float value_;
    float defaultValue_;
    float minValue_;
    float maxValue_;
    std::function<void(float)> onChanged_;
};

class AudioNode {
public:
    virtual ~AudioNode() = default;

    virtual void connect(AudioNode* destination) {
        if (destination && std::find(outputs_.begin(), outputs_.end(), destination) == outputs_.end()) {
            outputs_.push_back(destination);
            destination->inputs_.push_back(this);
        }
    }

    virtual void disconnect(AudioNode* destination = nullptr) {
        if (!destination) {
            for (auto* in : inputs_) {
                auto it = std::find(in->outputs_.begin(), in->outputs_.end(), this);
                if (it != in->outputs_.end()) in->outputs_.erase(it);
            }
            inputs_.clear();
            for (auto* out : outputs_) {
                auto it = std::find(out->inputs_.begin(), out->inputs_.end(), this);
                if (it != out->inputs_.end()) out->inputs_.erase(it);
            }
            outputs_.clear();
        } else {
            auto it = std::find(outputs_.begin(), outputs_.end(), destination);
            if (it != outputs_.end()) outputs_.erase(it);
            auto it2 = std::find(destination->inputs_.begin(), destination->inputs_.end(), this);
            if (it2 != destination->inputs_.end()) destination->inputs_.erase(it2);
        }
    }

    const std::vector<AudioNode*>& inputs() const { return inputs_; }
    const std::vector<AudioNode*>& outputs() const { return outputs_; }

protected:
    std::vector<AudioNode*> inputs_;
    std::vector<AudioNode*> outputs_;
};

class GainNode : public AudioNode {
public:
    GainNode(float defaultGain = 1.0f) : gain_(defaultGain) {}
    AudioParam& gain() { return gain_; }
    const AudioParam& gain() const { return gain_; }

private:
    AudioParam gain_;
};

enum class OscillatorType { Sine, Square, Sawtooth, Triangle };

class OscillatorNode : public AudioNode {
public:
    OscillatorNode(float defaultFreq = 440.0f) : frequency_(defaultFreq), detune_(0.0f) {}

    AudioParam& frequency() { return frequency_; }
    const AudioParam& frequency() const { return frequency_; }

    AudioParam& detune() { return detune_; }
    const AudioParam& detune() const { return detune_; }

    OscillatorType type() const { return type_; }
    void setType(OscillatorType t) { type_ = t; }

    GainNode* connectedGainNode() const {
        for (auto* out : outputs_) {
            if (auto* g = dynamic_cast<GainNode*>(out)) return g;
        }
        return nullptr;
    }

private:
    AudioParam frequency_;
    AudioParam detune_;
    OscillatorType type_ = OscillatorType::Sine;
};

class Analyser : public AudioNode {
public:
    Analyser(int fftSz = 2048) : fftSize_(fftSz) {}

    int fftSize() const { return fftSize_; }
    void setFftSize(int sz) { fftSize_ = sz; }

    float smoothingTimeConstant() const { return smoothingTimeConstant_; }
    void setSmoothingTimeConstant(float s) { smoothingTimeConstant_ = std::clamp(s, 0.0f, 1.0f); }

    float minDecibels() const { return minDecibels_; }
    void setMinDecibels(float db) { minDecibels_ = db; }

    float maxDecibels() const { return maxDecibels_; }
    void setMaxDecibels(float db) { maxDecibels_ = db; }

private:
    int fftSize_ = 2048;
    float smoothingTimeConstant_ = 0.8f;
    float minDecibels_ = -100.0f;
    float maxDecibels_ = -30.0f;
};

} // namespace broaudio
