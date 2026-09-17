#include "broaudio/dsp/jit/jit_bus_pipeline.h"
#include "broaudio/mix/bus.h"
#include <brass/codegen/kernel_jit.hpp>

namespace broaudio {

JitBusPipeline::JitBusPipeline(JitTopology topology,
                               std::shared_ptr<brass::codegen::KernelFunction> kernel,
                               BusJitFn fnPtr) noexcept
    : topology_(topology), kernel_(std::move(kernel)), fnPtr_(fnPtr)
{
    state_.reset();
}

bool JitBusPipeline::matches(const Bus& bus) const noexcept
{
    int enabledCount = 0;
    for (int f = 0; f < Bus::MAX_FILTERS; f++) {
        if (bus.filterParams[f].enabled.load(std::memory_order_relaxed)) {
            enabledCount++;
        }
    }

    bool distEnabled = bus.distortionParams.enabled.load(std::memory_order_relaxed);
    DistortionMode distMode = static_cast<DistortionMode>(
        bus.distortionParams.mode.load(std::memory_order_relaxed));

    // Unsupported distortion modes (e.g. Bitcrush sample-and-hold) stay on OOP fallback
    if (distEnabled && distMode == DistortionMode::Bitcrush) {
        return false;
    }

    // If other bus effects are enabled, fall back to sequential OOP pipeline
    bool delayEnabled = bus.delayParams.enabled.load(std::memory_order_relaxed);
    bool compEnabled  = bus.compressorParams.enabled.load(std::memory_order_relaxed);
    bool chorusEnabled = bus.chorusParams.enabled.load(std::memory_order_relaxed);
    bool reverbEnabled = bus.reverbParams.enabled.load(std::memory_order_relaxed);
    bool eqEnabled     = bus.eqParams.enabled.load(std::memory_order_relaxed);

    if (delayEnabled || compEnabled || chorusEnabled || reverbEnabled || eqEnabled) {
        return false;
    }

    return topology_.filterCount == enabledCount &&
           topology_.hasDistortion == distEnabled &&
           (!distEnabled || topology_.distortionMode == distMode);
}

void JitBusPipeline::updateParams(Bus& bus, int /*numFrames*/, int sampleRate)
{
    params_.activeFilterCount = 0;

    for (int f = 0; f < Bus::MAX_FILTERS; f++) {
        if (!bus.filterParams[f].enabled.load(std::memory_order_relaxed)) continue;
        int idx = params_.activeFilterCount;
        if (idx >= JitTopology::MAX_FILTERS) break;

        uint32_t ver = bus.filterParams[f].version.load(std::memory_order_acquire);
        if (ver != bus.filterVersions[f]) {
            bus.filterVersions[f] = ver;
            bus.filters[f].enabled = true;
            bus.filters[f].type = static_cast<BiquadFilter::Type>(
                bus.filterParams[f].type.load(std::memory_order_relaxed));
            bus.filters[f].frequency = bus.filterParams[f].frequency.load(std::memory_order_relaxed);
            bus.filters[f].Q = bus.filterParams[f].Q.load(std::memory_order_relaxed);
            bus.filters[f].gainDB = bus.filterParams[f].gainDB.load(std::memory_order_relaxed);
            bus.filters[f].computeCoefficients(sampleRate);
            bus.filters[f].snapToTarget();
        }

        params_.filters[idx].b0 = bus.filters[f].b0;
        params_.filters[idx].b1 = bus.filters[f].b1;
        params_.filters[idx].b2 = bus.filters[f].b2;
        params_.filters[idx].a1 = bus.filters[f].a1;
        params_.filters[idx].a2 = bus.filters[f].a2;
        params_.activeFilterCount++;
    }

    if (topology_.hasDistortion) {
        uint32_t ver = bus.distortionParams.version.load(std::memory_order_acquire);
        if (ver != bus.distortionVersion) {
            bus.distortionVersion = ver;
            bus.distortion.enabled = bus.distortionParams.enabled.load(std::memory_order_relaxed);
            bus.distortion.mode = static_cast<DistortionMode>(bus.distortionParams.mode.load(std::memory_order_relaxed));
            bus.distortion.drive = bus.distortionParams.drive.load(std::memory_order_relaxed);
            bus.distortion.mix = bus.distortionParams.mix.load(std::memory_order_relaxed);
            bus.distortion.outputGain = bus.distortionParams.outputGain.load(std::memory_order_relaxed);
        }
        params_.distortionDrive = bus.distortion.drive;
        params_.distortionMix = bus.distortion.mix;
        params_.distortionOutputGain = bus.distortion.outputGain;
    }
}

} // namespace broaudio
