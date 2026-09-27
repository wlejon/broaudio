// Bus effect processing and bus mixing — audio thread only — plus the bus
// convolution reverb's control-plane calls.

#include "broaudio/engine.h"
#include "broaudio/synth/oscillator.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace broaudio {

void Engine::processBusFilters(Bus& bus, float* buf, int numFrames)
{
    for (int f = 0; f < Bus::MAX_FILTERS; f++) {
        uint32_t ver = bus.filterParams[f].version.load(std::memory_order_acquire);
        if (ver != bus.filterVersions[f]) {
            bus.filterVersions[f] = ver;
            bool enabled = bus.filterParams[f].enabled.load(std::memory_order_relaxed);
            bus.filters[f].enabled = enabled;
            if (enabled) {
                bus.filters[f].type = static_cast<BiquadFilter::Type>(
                    bus.filterParams[f].type.load(std::memory_order_relaxed));
                bus.filters[f].frequency = bus.filterParams[f].frequency.load(std::memory_order_relaxed);
                bus.filters[f].Q = bus.filterParams[f].Q.load(std::memory_order_relaxed);
                bus.filters[f].gainDB = bus.filterParams[f].gainDB.load(std::memory_order_relaxed);
                bus.filters[f].computeCoefficients(sampleRate_);
                bus.filters[f].snapToTarget();
            } else {
                bus.filters[f].reset();
            }
        }
        if (!bus.filters[f].enabled) continue;
        for (int i = 0; i < numFrames; i++) {
            buf[i * 2]     = bus.filters[f].process(buf[i * 2], 0);
            buf[i * 2 + 1] = bus.filters[f].process(buf[i * 2 + 1], 1);
        }
    }
}

void Engine::processBusDelay(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.delayParams.version.load(std::memory_order_acquire);
    if (ver != bus.delayVersion) {
        bus.delayVersion = ver;
        bus.delay.enabled = bus.delayParams.enabled.load(std::memory_order_relaxed);
        float delaySec = bus.delayParams.time.load(std::memory_order_relaxed);
        int maxSamples = static_cast<int>(bus.delay.buffer.size());
        bus.delay.delaySamples = std::clamp(
            static_cast<int>(delaySec * sampleRate_), 1, maxSamples - 1);
        bus.delay.feedback = bus.delayParams.feedback.load(std::memory_order_relaxed);
        bus.delay.mix = bus.delayParams.mix.load(std::memory_order_relaxed);
    }
    if (bus.delay.enabled) {
        bus.delay.processStereo(buf, numFrames);
    }
}

void Engine::processBusCompressor(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.compressorParams.version.load(std::memory_order_acquire);
    if (ver != bus.compressorVersion) {
        bus.compressorVersion = ver;
        bus.compressor.threshold = bus.compressorParams.threshold.load(std::memory_order_relaxed);
        bus.compressor.ratio = bus.compressorParams.ratio.load(std::memory_order_relaxed);
        float attackMs = bus.compressorParams.attackMs.load(std::memory_order_relaxed);
        float releaseMs = bus.compressorParams.releaseMs.load(std::memory_order_relaxed);
        bus.compressor.attackCoeff = 1.0f - std::exp(-1.0f / (attackMs * 0.001f * static_cast<float>(sampleRate_)));
        bus.compressor.releaseCoeff = 1.0f - std::exp(-1.0f / (releaseMs * 0.001f * static_cast<float>(sampleRate_)));
    }
    if (bus.compressorParams.enabled.load(std::memory_order_relaxed)) {
        int scBusId = bus.compressorParams.sidechainBusId.load(std::memory_order_relaxed);
        if (scBusId >= 0) {
            // Sidechain: detect level from another bus's buffer
            auto currentBuses = buses_.load();
            for (auto& scBus : *currentBuses) {
                if (scBus->id == scBusId) {
                    bus.compressor.processStereoWithSidechain(buf, scBus->buffer.data(), numFrames);
                    return;
                }
            }
        }
        bus.compressor.processStereo(buf, numFrames);
    }
}

void Engine::processBusChorus(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.chorusParams.version.load(std::memory_order_acquire);
    if (ver != bus.chorusVersion) {
        bus.chorusVersion = ver;
        bus.chorus.enabled = bus.chorusParams.enabled.load(std::memory_order_relaxed);
        bus.chorus.rate = bus.chorusParams.rate.load(std::memory_order_relaxed);
        bus.chorus.depth = bus.chorusParams.depth.load(std::memory_order_relaxed);
        bus.chorus.mix = bus.chorusParams.mix.load(std::memory_order_relaxed);
        bus.chorus.feedback = bus.chorusParams.feedback.load(std::memory_order_relaxed);
        bus.chorus.baseDelay = bus.chorusParams.baseDelay.load(std::memory_order_relaxed);
    }
    if (bus.chorus.enabled) {
        bus.chorus.processStereo(buf, numFrames);
    }
}

void Engine::processBusReverb(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.reverbParams.version.load(std::memory_order_acquire);
    if (ver != bus.reverbVersion) {
        bus.reverbVersion = ver;
        bus.reverb.enabled = bus.reverbParams.enabled.load(std::memory_order_relaxed);
        bus.reverb.roomSize = bus.reverbParams.roomSize.load(std::memory_order_relaxed);
        bus.reverb.damping = bus.reverbParams.damping.load(std::memory_order_relaxed);
        bus.reverb.mix = bus.reverbParams.mix.load(std::memory_order_relaxed);
    }
    if (bus.reverb.enabled) {
        bus.reverb.processStereo(buf, numFrames);
    }
}

void Engine::processBusEqualizer(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.eqParams.version.load(std::memory_order_acquire);
    if (ver != bus.eqVersion) {
        bus.eqVersion = ver;
        bool enabled = bus.eqParams.enabled.load(std::memory_order_relaxed);
        bus.equalizer.setEnabled(enabled);
        if (enabled) {
            bus.equalizer.setSampleRate(sampleRate_);
            bus.equalizer.setMasterGain(bus.eqParams.masterGain.load(std::memory_order_relaxed));
            for (int b = 0; b < Equalizer::NUM_BANDS; b++) {
                bus.equalizer.setBandGain(b, bus.eqParams.bandGains[b].load(std::memory_order_relaxed));
            }
        }
    }
    if (bus.equalizer.isEnabled()) {
        bus.equalizer.processStereoInterleaved(buf, numFrames);
    }
}

void Engine::processBusDistortion(Bus& bus, float* buf, int numFrames)
{
    uint32_t ver = bus.distortionParams.version.load(std::memory_order_acquire);
    if (ver != bus.distortionVersion) {
        bus.distortionVersion = ver;
        bus.distortion.enabled = bus.distortionParams.enabled.load(std::memory_order_relaxed);
        bus.distortion.mode = static_cast<DistortionMode>(bus.distortionParams.mode.load(std::memory_order_relaxed));
        bus.distortion.drive = bus.distortionParams.drive.load(std::memory_order_relaxed);
        bus.distortion.mix = bus.distortionParams.mix.load(std::memory_order_relaxed);
        bus.distortion.outputGain = bus.distortionParams.outputGain.load(std::memory_order_relaxed);
        bus.distortion.crushBits = bus.distortionParams.crushBits.load(std::memory_order_relaxed);
        bus.distortion.crushRate = bus.distortionParams.crushRate.load(std::memory_order_relaxed);
    }
    if (bus.distortion.enabled) {
        bus.distortion.processStereo(buf, numFrames);
    }
}

void Engine::processBusConvolution(Bus& bus, float* buf, int numFrames)
{
    if (!bus.convolutionEnabled.load(std::memory_order_relaxed)) return;
    auto conv = bus.convolver.load(std::memory_order_acquire);
    if (!conv) return;
    bus.convMix.follow(std::clamp(bus.convolutionMix.load(std::memory_order_relaxed), 0.0f, 1.0f));
    conv->processInterleaved(buf, numFrames, bus.convMix);
}

void Engine::updateBusMeters(Bus& bus, int numFrames)
{
    float* buf = bus.buffer.data();
    float pL = 0.0f, pR = 0.0f;
    float sumSqL = 0.0f, sumSqR = 0.0f;
    for (int i = 0; i < numFrames; i++) {
        float l = std::fabs(buf[i * 2]);
        float r = std::fabs(buf[i * 2 + 1]);
        if (l > pL) pL = l;
        if (r > pR) pR = r;
        sumSqL += buf[i * 2] * buf[i * 2];
        sumSqR += buf[i * 2 + 1] * buf[i * 2 + 1];
    }
    bus.peakL.store(pL, std::memory_order_relaxed);
    bus.peakR.store(pR, std::memory_order_relaxed);
    float invN = 1.0f / static_cast<float>(std::max(numFrames, 1));
    bus.rmsL.store(std::sqrt(sumSqL * invN), std::memory_order_relaxed);
    bus.rmsR.store(std::sqrt(sumSqR * invN), std::memory_order_relaxed);
}

void Engine::processBusEffects(Bus& bus, int numFrames)
{
    float* buf = bus.buffer.data();
    advanceBusGain(bus, numFrames);

    // Fast JIT path: if JIT is enabled and pipeline matches the bus topology,
    // execute the single fused AVX2/FMA kernel across the stereo buffer.
    // matches() is false while the convolution reverb is enabled.
    if (bus.jitEnabled.load(std::memory_order_relaxed)
        && !bus.convolutionEnabled.load(std::memory_order_relaxed)) {
        auto pipeline = bus.jitPipeline.load(std::memory_order_acquire);
        if (pipeline && pipeline->matches(bus)) {
            pipeline->updateParams(bus, numFrames, sampleRate_);
            pipeline->process(buf, numFrames);
            bus.jitActive.store(true, std::memory_order_relaxed);
            updateBusMeters(bus, numFrames);
            return;
        }
    }
    bus.jitActive.store(false, std::memory_order_relaxed);

    uint32_t ver = bus.effectOrderVersion.load(std::memory_order_acquire);
    if (ver != bus.effectOrderVersionSeen) {
        bus.effectOrderVersionSeen = ver;
        for (int i = 0; i < Bus::NUM_EFFECT_SLOTS; i++)
            bus.effectOrderCache[i] = bus.effectOrder[i].load(std::memory_order_relaxed);
    }

    for (int i = 0; i < Bus::NUM_EFFECT_SLOTS; i++) {
        switch (static_cast<EffectSlot>(bus.effectOrderCache[i])) {
            case EffectSlot::Filter:      processBusFilters(bus, buf, numFrames); break;
            case EffectSlot::Delay:       processBusDelay(bus, buf, numFrames); break;
            case EffectSlot::Compressor:  processBusCompressor(bus, buf, numFrames); break;
            case EffectSlot::Chorus:      processBusChorus(bus, buf, numFrames); break;
            case EffectSlot::Reverb:      processBusReverb(bus, buf, numFrames); break;
            case EffectSlot::Equalizer:   processBusEqualizer(bus, buf, numFrames); break;
            case EffectSlot::Distortion:  processBusDistortion(bus, buf, numFrames); break;
            case EffectSlot::Convolution: processBusConvolution(bus, buf, numFrames); break;
            default: break;
        }
    }

    updateBusMeters(bus, numFrames);
}

void Engine::advanceBusGain(Bus& bus, int numFrames)
{
    bus.gainState.update(bus.gain.load(std::memory_order_relaxed), bus.gainRamp, sampleRate_);
    float* g = bus.gainBlock.data();
    const int n = std::min(numFrames, static_cast<int>(bus.gainBlock.size()));
    for (int i = 0; i < n; i++) g[i] = bus.gainState.next();
}

void Engine::mixBusIntoParent(Bus& child, Bus& parent, int numFrames)
{
    if (child.muted.load(std::memory_order_relaxed)) return;

    child.smoothPan.set(child.pan.load(std::memory_order_relaxed));

    const float* src = child.buffer.data();
    const float* gains = child.gainBlock.data();
    float* dst = parent.buffer.data();

    for (int i = 0; i < numFrames; i++) {
        float g = gains[i];
        float panVal = child.smoothPan.next();
        float panL, panR;
        panGains(panVal, panL, panR);

        float L = src[i * 2]     * g;
        float R = src[i * 2 + 1] * g;
        // Apply bus panning (cross-mix stereo signal)
        dst[i * 2]     += L * panL + R * (1.0f - panR);
        dst[i * 2 + 1] += R * panR + L * (1.0f - panL);
    }
}

void Engine::mixBusSend(Bus& bus, Bus& target, float amount, int numFrames)
{
    // Post-effects, post-fader: the send follows the bus's per-sample gain.
    const float* src = bus.buffer.data();
    const float* gains = bus.gainBlock.data();
    float* dst = target.buffer.data();
    for (int i = 0; i < numFrames; i++) {
        const float g = gains[i] * amount;
        dst[i * 2]     += src[i * 2] * g;
        dst[i * 2 + 1] += src[i * 2 + 1] * g;
    }
}

// --- Convolution reverb control (control thread) ---

bool Engine::setBusConvolutionImpulse(int busId, int clipId)
{
    std::shared_ptr<PartitionedConvolver> conv;
    if (clipId >= 0) {
        std::vector<float> ir;
        int channels = 1;
        {
            // Copy the frames under the media lock so a concurrent deleteClip
            // cannot free them mid-read; the FFT work happens outside it.
            std::lock_guard<std::mutex> lock(mediaWriteMutex_);
            AudioClip* clip = findClip(clipId);
            if (!clip || clip->streaming || clip->numFrames() <= 0) return false;
            channels = clip->channels;
            ir = clip->samples;
        }
        if (channels != 1 && channels != 2) return false;
        conv = PartitionedConvolver::create(ir.data(), static_cast<int>(ir.size()) / channels, channels);
        if (!conv) return false;
    }
    std::lock_guard<std::mutex> lock(busWriteMutex_);
    Bus* bus = findBus(busId);
    if (!bus) return false;
    bus->convolver.store(std::move(conv));
    bus->convolutionClipId.store(clipId >= 0 ? clipId : -1, std::memory_order_relaxed);
    return true;
}

int Engine::getBusConvolutionImpulse(int busId) const
{
    auto* b = findBus(busId);
    return b ? b->convolutionClipId.load(std::memory_order_relaxed) : -1;
}

void Engine::setBusConvolutionMix(int busId, float mix)
{
    if (auto* b = findBus(busId))
        b->convolutionMix.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

float Engine::getBusConvolutionMix(int busId) const
{
    auto* b = findBus(busId);
    return b ? b->convolutionMix.load(std::memory_order_relaxed) : 1.0f;
}

void Engine::setBusConvolutionEnabled(int busId, bool enabled)
{
    if (auto* b = findBus(busId))
        b->convolutionEnabled.store(enabled, std::memory_order_relaxed);
}

bool Engine::getBusConvolutionEnabled(int busId) const
{
    auto* b = findBus(busId);
    return b ? b->convolutionEnabled.load(std::memory_order_relaxed) : false;
}

} // namespace broaudio
