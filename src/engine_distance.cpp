// Physical-distance controls: world scale, air absorption, propagation delay.
// Control thread only; the audio-thread half is in engine_playback_mix.cpp
// and spatial/spatial_chain.cpp.

#include "broaudio/engine.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace broaudio {

void Engine::initDistanceState(int maxFrames)
{
    chainScratch_.assign(static_cast<size_t>(maxFrames) * 4, 0.0f);
}

void Engine::ensureAirTable()
{
    // Fitted lazily (tens of ms): engines that never use air absorption never
    // pay for it at init.
    std::lock_guard<std::mutex> lock(airWriteMutex_);
    if (airTable_.load()) return;
    airTable_.store(buildAirFilterTable(sampleRate_,
                                        airTemperature_.load(std::memory_order_relaxed),
                                        airHumidity_.load(std::memory_order_relaxed)));
}

void Engine::setSpatialMetresPerUnit(float metres)
{
    if (!(metres > 0.0f) || !std::isfinite(metres)) return;
    metresPerUnit_.store(metres, std::memory_order_relaxed);
}

void Engine::setSpatialAirConditions(float temperatureC, float relativeHumidityPct)
{
    if (!std::isfinite(temperatureC) || !std::isfinite(relativeHumidityPct)) return;
    const float t = std::clamp(temperatureC, -20.0f, 50.0f);
    const float h = std::clamp(relativeHumidityPct, 1.0f, 100.0f);
    std::lock_guard<std::mutex> lock(airWriteMutex_);
    airTemperature_.store(t, std::memory_order_relaxed);
    airHumidity_.store(h, std::memory_order_relaxed);
    airTable_.store(buildAirFilterTable(sampleRate_, t, h));
}

void Engine::setSpatialAirAbsorptionStrength(float strength)
{
    if (!std::isfinite(strength)) return;
    airStrength_.store(std::max(0.0f, strength), std::memory_order_relaxed);
}

void Engine::setPlaybackSpatialAirAbsorption(int instanceId, bool enabled)
{
    auto* pb = findPlayback(instanceId);
    if (!pb) return;
    if (enabled) ensureAirTable();
    pb->airAbsorption.store(enabled, std::memory_order_relaxed);
}

void Engine::setSpatialSpeedOfSound(float metresPerSecond)
{
    if (!std::isfinite(metresPerSecond)) return;
    speedOfSound_.store(std::max(1.0f, metresPerSecond), std::memory_order_relaxed);
}

void Engine::ensureDelayBuffer(ClipPlayback& pb, int channels)
{
    const float maxSec = maxPropagationDelay_.load(std::memory_order_relaxed);
    const int needed = static_cast<int>(std::ceil(maxSec * static_cast<float>(sampleRate_))) + 8;
    const int capacity = static_cast<int>(std::bit_ceil(static_cast<unsigned>(std::max(needed, 64))));
    channels = channels == 2 ? 2 : 1;
    PropagationDelayBuffer* cur = pb.delayBuffer.load(std::memory_order_relaxed);
    if (cur && cur->capacity >= capacity && cur->channels == channels) return;

    auto* fresh = new PropagationDelayBuffer();
    fresh->channels = channels;
    fresh->capacity = capacity;
    fresh->mask = capacity - 1;
    fresh->data.assign(static_cast<size_t>(capacity) * channels, 0.0f);
    PropagationDelayBuffer* old = pb.delayBuffer.exchange(fresh, std::memory_order_acq_rel);
    if (old) {
        rcu_.retire(old, [](void* p) noexcept { delete static_cast<PropagationDelayBuffer*>(p); });
    }
}

void Engine::setSpatialMaxPropagationDelay(float seconds)
{
    if (!std::isfinite(seconds)) return;
    maxPropagationDelay_.store(std::clamp(seconds, 0.0f, 10.0f), std::memory_order_relaxed);
    // Grow the lines that are in use (serialised with the other playback writers).
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    auto clips = clips_.load();
    for (auto& pb : *playbacks_.load()) {
        if (!pb->propagationDelay.load(std::memory_order_relaxed)) continue;
        int channels = 1;
        for (auto& c : *clips) {
            if (c->id == pb->clipId) { channels = c->channels; break; }
        }
        ensureDelayBuffer(*pb, channels);
    }
}

void Engine::setPlaybackSpatialPropagationDelay(int instanceId, bool enabled)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return;
    if (enabled) {
        AudioClip* clip = findClip(pb->clipId);
        ensureDelayBuffer(*pb, clip ? clip->channels : 1);
    }
    pb->propagationDelay.store(enabled, std::memory_order_release);
}

} // namespace broaudio
