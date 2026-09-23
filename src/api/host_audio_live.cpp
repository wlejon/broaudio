// Live params: recompute engine state from AudioParams whenever one changes
// and on every tick, so value sets and scheduled automation reach sources
// that are already playing (host_audio_internal.h describes the model).

#include "host_audio_internal.h"

#include <algorithm>
#include <cmath>

namespace broaudio::api {

namespace {

struct PlayingHold {
    std::shared_ptr<LiveSource> source;
    ev::Persistent node;  // the AudioBufferSourceNode, for onended
};

struct LiveRegistry {
    std::vector<std::weak_ptr<LiveSource>> sources;
    std::vector<std::weak_ptr<LiveFilter>> filters;
    std::vector<std::weak_ptr<HostAudioParam>> automated;
    std::vector<PlayingHold> playing;
};

// Per thread, like every JS value the holds root. Deliberately leaked: a
// thread_local destructor would release Persistents after the runtime's own
// thread-local slot registry may already be gone.
LiveRegistry& registry() {
    static thread_local LiveRegistry* r = new LiveRegistry();
    return *r;
}

template <typename T>
void pruneExpired(std::vector<std::weak_ptr<T>>& v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const std::weak_ptr<T>& w) { return w.expired(); }),
            v.end());
}

float at(const ParamRef& p, double t, float fallback) {
    return p ? p->evaluate(t) : fallback;
}

float centsToRatio(float cents) {
    return cents == 0.0f ? 1.0f : std::pow(2.0f, cents / 1200.0f);
}

bool differs(float a, float b) {
    return std::fabs(a - b) > 1e-7f * std::max(1.0f, std::fabs(b));
}

void refreshSource(broaudio::Engine& e, LiveSource& s, double t) {
    if (s.ended || s.id < 0) return;

    float gain = at(s.ownGain, t, 1.0f);
    for (const auto& g : s.pathGains) gain *= at(g, t, 1.0f);
    const float pitch = at(s.pitch, t, 1.0f) * centsToRatio(at(s.detune, t, 0.0f));

    if (s.kind == LiveSource::Kind::Voice) {
        if (differs(gain, s.lastGain)) { e.setGain(s.id, gain); s.lastGain = gain; }
        if (differs(pitch, s.lastPitch)) { e.setFrequency(s.id, pitch); s.lastPitch = pitch; }
        const float pan = std::clamp(at(s.ownPan, t, 0.0f) + at(s.pathPan, t, 0.0f), -1.0f, 1.0f);
        if (differs(pan, s.lastPan)) { e.setVoicePan(s.id, pan); s.lastPan = pan; }
    } else {
        if (differs(gain, s.lastGain)) { e.setPlaybackGain(s.id, gain); s.lastGain = gain; }
        const float rate = pitch * s.rateScale;
        if (differs(rate, s.lastPitch)) { e.setPlaybackRate(s.id, rate); s.lastPitch = rate; }
        if (s.pathPan) {
            const float pan = std::clamp(at(s.pathPan, t, 0.0f), -1.0f, 1.0f);
            if (differs(pan, s.lastPan)) { e.setPlaybackPan(s.id, pan); s.lastPan = pan; }
        }
    }

    if (s.position[0]) {
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = at(s.position[i], t, 0.0f);
        if (!s.posWritten || differs(p[0], s.lastPos[0]) || differs(p[1], s.lastPos[1]) ||
            differs(p[2], s.lastPos[2])) {
            if (s.kind == LiveSource::Kind::Voice) e.setVoiceSpatialPosition(s.id, p[0], p[1], p[2]);
            else e.setPlaybackSpatialPosition(s.id, p[0], p[1], p[2]);
            std::copy(p, p + 3, s.lastPos);
            s.posWritten = true;
        }
    }
}

void refreshFilter(broaudio::Engine& e, LiveFilter& f, double t) {
    if (f.slot < 0) return;
    const float freq = at(f.frequency, t, 350.0f) * centsToRatio(at(f.detune, t, 0.0f));
    if (differs(freq, f.lastFrequency)) {
        e.setFilterFrequency(f.slot, freq);
        f.lastFrequency = freq;
    }
}

// The targets a param drives on its own (one param, one engine setter).
// Everything composite -- voice pitch/gain/pan, playback rate/gain/pan, the
// filter cutoff, panner position -- is a LiveSource / LiveFilter's job.
void applyDirectTarget(broaudio::Engine& e, const HostAudioParam& p, float v) {
    if (p.targetId < 0) return;
    switch (p.target) {
        case AudioParamTarget::FilterQ:             e.setFilterQ(p.targetId, v); break;
        case AudioParamTarget::FilterGain:          e.setFilterGain(p.targetId, v); break;
        case AudioParamTarget::DelayTime:           e.setDelayTime(v); break;
        case AudioParamTarget::VoiceAttack:         e.setAttackTime(p.targetId, v); break;
        case AudioParamTarget::VoiceDecay:          e.setDecayTime(p.targetId, v); break;
        case AudioParamTarget::VoiceSustain:        e.setSustainLevel(p.targetId, v); break;
        case AudioParamTarget::VoiceRelease:        e.setReleaseTime(p.targetId, v); break;
        case AudioParamTarget::VoicePitchBend:      e.setVoicePitchBend(p.targetId, v); break;
        case AudioParamTarget::CompressorThreshold:
            e.setBusCompressorThreshold(p.targetId, compressorThresholdLinear(v));
            break;
        case AudioParamTarget::CompressorRatio:     e.setBusCompressorRatio(p.targetId, v); break;
        case AudioParamTarget::CompressorAttack:    e.setBusCompressorAttack(p.targetId, v * 1000.0f); break;
        case AudioParamTarget::CompressorRelease:   e.setBusCompressorRelease(p.targetId, v * 1000.0f); break;
        default: break;
    }
}

} // namespace

void syncAudioParamValue(HostAudioParam* p, float val) {
    p->value = std::clamp(val, p->minValue, p->maxValue);
    auto* e = getAudioEngine();
    if (!e) return;
    applyDirectTarget(*e, *p, p->value);
    refreshLiveParams(e->currentTime());
}

void paramTimelineChanged(const ParamRef& p) {
    if (!p) return;
    if (!p->timeline.empty()) noteAutomatedParam(p);
    auto* e = getAudioEngine();
    if (!e) return;
    const double t = e->currentTime();
    applyDirectTarget(*e, *p, p->evaluate(t));
    refreshLiveParams(t);
}

void registerLiveSource(const std::shared_ptr<LiveSource>& src) {
    auto& r = registry();
    pruneExpired(r.sources);
    r.sources.push_back(src);
}

void registerLiveFilter(const std::shared_ptr<LiveFilter>& filter) {
    auto& r = registry();
    pruneExpired(r.filters);
    r.filters.push_back(filter);
}

void noteAutomatedParam(const ParamRef& param) {
    if (!param) return;
    auto& r = registry();
    for (const auto& w : r.automated) {
        if (w.lock() == param) return;
    }
    pruneExpired(r.automated);
    r.automated.push_back(param);
}

void holdPlayingSource(const std::shared_ptr<LiveSource>& src, Value node) {
    registry().playing.push_back(PlayingHold{src, ev::Persistent(node)});
}

void refreshLiveParams(double t) {
    auto& r = registry();
    if (r.sources.empty() && r.filters.empty() && r.automated.empty()) return;
    auto* e = existingAudioEngine();
    if (!e) return;

    for (size_t i = 0; i < r.automated.size();) {
        ParamRef p = r.automated[i].lock();
        if (!p || p->timeline.empty()) {
            r.automated.erase(r.automated.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        applyDirectTarget(*e, *p, p->evaluate(t));
        ++i;
    }
    for (size_t i = 0; i < r.filters.size();) {
        auto f = r.filters[i].lock();
        if (!f) {
            r.filters.erase(r.filters.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        refreshFilter(*e, *f, t);
        ++i;
    }
    for (size_t i = 0; i < r.sources.size();) {
        auto s = r.sources[i].lock();
        if (!s || s->ended) {
            r.sources.erase(r.sources.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        refreshSource(*e, *s, t);
        ++i;
    }
}

void tickLiveParams() {
    auto& r = registry();
    auto* e = existingAudioEngine();
    if (!e) return;
    refreshLiveParams(e->currentTime());
    if (r.playing.empty()) return;

    // Take the finished holds out first: an onended handler runs JS that
    // may start another source onto this list.
    std::vector<ev::Persistent> ended;
    for (size_t i = 0; i < r.playing.size();) {
        LiveSource& s = *r.playing[i].source;
        const auto state = e->getPlaybackState(s.id);
        if (state == broaudio::Engine::PlaybackState::Invalid ||
            state == broaudio::Engine::PlaybackState::Finished) {
            s.ended = true;
            ended.push_back(std::move(r.playing[i].node));
            r.playing.erase(r.playing.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }

    // `onended`, then any addEventListener('ended', ...) listeners.
    for (auto& node : ended) dispatchNodeEvent(node.get(), "ended");
}

void shutdownLiveParams() {
    auto& r = registry();
    r.playing.clear();
    r.sources.clear();
    r.filters.clear();
    r.automated.clear();
}

} // namespace broaudio::api
