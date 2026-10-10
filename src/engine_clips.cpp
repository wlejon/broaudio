// Audio clips and clip playback, streaming PCM sources, disk-streamed file
// playback, and clip loading from / recording export to audio files.
// Control thread only; the audio-thread mixer is in engine_playback_mix.cpp.

#include "broaudio/engine.h"
#include "broaudio/clip/file_stream.h"
#include "broaudio/io/audio_stream.h"
#include "broaudio/log.h"
#include "broaudio/dsp/resampler.h"

#include <new>
#include <algorithm>
#include <cmath>

namespace broaudio {

// ---------------------------------------------------------------------------
// Audio Clips — RCU for clip and playback lists
// ---------------------------------------------------------------------------

AudioClip* Engine::findClip(int clipId) const
{
    auto currentClips = clips_.load();
    for (auto& c : *currentClips) {
        if (c->id == clipId) return c.get();
    }
    return nullptr;
}

ClipPlayback* Engine::findPlayback(int instanceId) const
{
    auto currentPlaybacks = playbacks_.load();
    for (auto& pb : *currentPlaybacks) {
        if (pb->id == instanceId && pb->active.load(std::memory_order_relaxed))
            return pb.get();
    }
    return nullptr;
}

int Engine::createClip(const float* samples, int numSamples, int channels)
{
    if (numSamples <= 0 || !samples || channels < 1 || channels > 2) return -1;

    auto clip = std::make_shared<AudioClip>();
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    clip->id = nextClipId_++;
    clip->channels = channels;
    clip->samples.assign(samples, samples + numSamples);

    int id = clip->id;
    auto newList = std::make_shared<ClipList>(*clips_.load());
    newList->push_back(std::move(clip));
    clips_.store(std::move(newList));

    return id;
}

void Engine::deleteClip(int clipId)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);

    auto currentPB = playbacks_.load();
    auto newPB = std::make_shared<PlaybackList>();
    for (auto& pb : *currentPB) {
        if (pb->clipId == clipId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newPB->push_back(pb);
        }
    }
    playbacks_.store(std::move(newPB));

    auto currentClips = clips_.load();
    auto newClips = std::make_shared<ClipList>();
    for (auto& c : *currentClips) {
        if (c->id != clipId) newClips->push_back(c);
    }
    clips_.store(std::move(newClips));
}

int Engine::getClipSampleCount(int clipId) const
{
    if (auto* c = findClip(clipId)) return c->numFrames();
    return 0;
}

int Engine::getClipChannels(int clipId) const
{
    if (auto* c = findClip(clipId)) return c->channels;
    return 0;
}

std::vector<float> Engine::getClipWaveform(int clipId, int numBins) const
{
    std::vector<float> outMinMax(numBins * 2, 0.0f);
    auto* clip = findClip(clipId);
    if (!clip || clip->samples.empty() || numBins <= 0) {
        return outMinMax;
    }

    int totalFrames = clip->numFrames();
    int ch = clip->channels;
    float framesPerBin = static_cast<float>(totalFrames) / static_cast<float>(numBins);

    for (int b = 0; b < numBins; b++) {
        int startFrame = static_cast<int>(b * framesPerBin);
        int endFrame = static_cast<int>((b + 1) * framesPerBin);
        endFrame = std::min(endFrame, totalFrames);

        float minVal = 1.0f, maxVal = -1.0f;
        for (int f = startFrame; f < endFrame; f++) {
            float s;
            if (ch == 2) {
                s = (clip->samples[f * 2] + clip->samples[f * 2 + 1]) * 0.5f;
            } else {
                s = clip->samples[f];
            }
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }
        outMinMax[b * 2] = minVal;
        outMinMax[b * 2 + 1] = maxVal;
    }
    return outMinMax;
}

// ---------------------------------------------------------------------------
// Clip Playback
// ---------------------------------------------------------------------------

int Engine::playClip(int clipId, float gain, bool loop)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (!findClip(clipId)) return -1;

    auto pb = std::make_shared<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed);
    pb->regionStart.store(0, std::memory_order_relaxed);
    pb->regionEnd.store(0, std::memory_order_relaxed);

    int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

int Engine::playClipAt(int clipId, double when, float gain, bool loop)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (!findClip(clipId)) return -1;

    // Resolve the start to an absolute engine sample. A time at/before now maps
    // to 0 (immediate); the mixer treats 0 / past samples as "already started".
    uint64_t startSample = 0;
    double s = when * static_cast<double>(sampleRate_);
    if (s > 0.0) startSample = static_cast<uint64_t>(s + 0.5);

    auto pb = std::make_shared<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed);
    pb->regionStart.store(0, std::memory_order_relaxed);
    pb->regionEnd.store(0, std::memory_order_relaxed);
    pb->startSample.store(startSample, std::memory_order_relaxed);

    int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

int Engine::playClip(int clipId, const ClipPlayOptions& opts)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    AudioClip* clip = findClip(clipId);
    if (!clip || clip->streaming) return -1;

    uint64_t startSample = 0;
    double s = opts.when * static_cast<double>(sampleRate_);
    if (s > 0.0) startSample = static_cast<uint64_t>(s + 0.5);

    const double frames = static_cast<double>(clip->numFrames());
    const double offset = std::clamp(opts.offsetFrames, 0.0, frames);

    auto pb = std::make_shared<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(opts.gain, std::memory_order_relaxed);
    pb->looping.store(opts.loop, std::memory_order_relaxed);
    pb->rate.store(std::clamp(opts.rate, 0.01f, 16.0f), std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(static_cast<uint64_t>(offset * 65536.0 + 0.5), std::memory_order_relaxed);
    pb->regionStart.store(0, std::memory_order_relaxed);
    pb->regionEnd.store(0, std::memory_order_relaxed);
    pb->loopStart.store(opts.loopStartFrame, std::memory_order_relaxed);
    pb->loopEnd.store(opts.loopEndFrame, std::memory_order_relaxed);
    pb->durationFixed.store(opts.durationFrames < 0.0
                                ? UINT64_MAX
                                : static_cast<uint64_t>(opts.durationFrames * 65536.0 + 0.5),
                            std::memory_order_relaxed);
    pb->startSample.store(startSample, std::memory_order_relaxed);

    int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

void Engine::setPlaybackLoopPoints(int instanceId, int loopStartFrame, int loopEndFrame)
{
    if (auto* pb = findPlayback(instanceId)) {
        pb->loopStart.store(loopStartFrame, std::memory_order_relaxed);
        pb->loopEnd.store(loopEndFrame, std::memory_order_relaxed);
    }
}

Engine::PlaybackState Engine::getPlaybackState(int instanceId) const
{
    auto current = playbacks_.load();
    for (auto& pb : *current) {
        if (pb->id != instanceId) continue;
        if (!pb->active.load(std::memory_order_relaxed)) return PlaybackState::Finished;
        if (!pb->playing.load(std::memory_order_relaxed)) return PlaybackState::Paused;
        // A disk stream that reached EOF (not looping) and whose ring has
        // drained has nothing left to play: it stays open (closeStream) but
        // is finished, like a one-shot parked at its end.
        if (pb->clipId > 0) {
            if (AudioClip* clip = findClip(pb->clipId); clip && clip->streaming &&
                clip->streamEnded.load(std::memory_order_acquire) &&
                pb->playPos.load(std::memory_order_relaxed) >=
                    clip->writeFrames.load(std::memory_order_acquire))
                return PlaybackState::Finished;
        }
        // The mixer begins a scheduled playback inside the block that spans
        // startSample; samplesGenerated_ is the end of the last mixed block.
        uint64_t startS = pb->startSample.load(std::memory_order_relaxed);
        if (startS > 0 && startS >= samplesGenerated_.load(std::memory_order_relaxed))
            return PlaybackState::Scheduled;
        return PlaybackState::Playing;
    }
    return PlaybackState::Invalid;
}

// ---------------------------------------------------------------------------
// Streaming PCM source — a ring-backed clip + a persistent playback. Reuses the
// full ClipPlayback machinery (gain/pan/bus/sends/spatial); the mixer takes a
// ring-read branch when clip->streaming is set.
// ---------------------------------------------------------------------------
int Engine::createStreamPlayback(int channels, int ringFrames, bool startPlaying,
                                 float gain, bool loop,
                                 std::shared_ptr<AudioClip>& outClip,
                                 std::shared_ptr<ClipPlayback>& outPb)
{
    auto clip = std::make_shared<AudioClip>();
    auto pb   = std::make_shared<ClipPlayback>();

    // The ring is sized by the caller (a script's ringFrames option): an
    // allocation that cannot be met fails the stream, before any state
    // changes, rather than throwing out of the engine.
    try {
        clip->samples.assign(static_cast<size_t>(ringFrames) * channels, 0.0f);
    } catch (const std::bad_alloc&) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    clip->id = nextClipId_++;
    clip->channels = channels;
    clip->streaming = true;
    clip->ringFrames = ringFrames;
    clip->writeFrames.store(0, std::memory_order_relaxed);
    int clipId = clip->id;

    pb->id = nextPlaybackId_++;
    pb->clipId = clipId;
    pb->gain.store(gain, std::memory_order_relaxed);
    pb->looping.store(loop, std::memory_order_relaxed);
    pb->playing.store(startPlaying, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->playPos.store(0, std::memory_order_relaxed); // absolute read-frame cursor
    int id = pb->id;

    outClip = clip;
    outPb   = pb;

    auto newClips = std::make_shared<ClipList>(*clips_.load());
    newClips->push_back(std::move(clip));
    clips_.store(std::move(newClips));

    auto newPB = std::make_shared<PlaybackList>(*playbacks_.load());
    newPB->push_back(std::move(pb));
    playbacks_.store(std::move(newPB));
    return id;
}

int Engine::createStream(int channels, int ringFrames)
{
    if (channels < 1 || channels > 2) return -1;
    if (ringFrames <= 0) ringFrames = sampleRate_ * 2; // ~2 s default

    std::shared_ptr<AudioClip> clip;
    std::shared_ptr<ClipPlayback> pb;
    return createStreamPlayback(channels, ringFrames, /*startPlaying=*/true,
                                1.0f, false, clip, pb);
}

int Engine::pushStreamSamples(int instanceId, const float* samples, int numSamples)
{
    if (!samples || numSamples <= 0) return 0;
    // No write lock: this is the single producer. We only read the (stable)
    // clip pointer + write ring slots, then release-publish writeFrames.
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return 0;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip || !clip->streaming || clip->ringFrames <= 0) return 0;

    int numFrames = numSamples / clip->channels;
    if (numFrames <= 0) return 0;
    return pushStreamFrames(*clip, samples, numFrames);
}

void Engine::closeStream(int instanceId)
{
    // Disk streams: stop + join the decode worker before dropping the ring.
    stopFileStream(instanceId);

    ClipPlayback* pb = findPlayback(instanceId);
    int clipId = pb ? pb->clipId : -1;
    stopPlayback(instanceId);
    if (clipId >= 0) deleteClip(clipId);
}

// ---------------------------------------------------------------------------
// Disk-streamed file playback — decode on a cold worker thread into the same
// SPSC ring a live PCM stream uses; the audio thread only consumes.
// ---------------------------------------------------------------------------

int Engine::createStreamFromFile(const char* path, std::string* outError)
{
    return createStreamFromFile(path, FileStreamOptions{}, outError);
}

int Engine::createStreamFromFile(const char* path, const FileStreamOptions& opts,
                                 std::string* outError)
{
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        log(LogLevel::Error, "createStreamFromFile: %s: %s",
            path ? path : "(null)", msg.c_str());
        return -1;
    };

    auto decoder = std::make_unique<AudioFileStream>();
    if (!decoder->open(path)) {
        return fail(decoder->error().empty() ? "cannot open or decode file"
                                             : decoder->error());
    }
    if (decoder->channels() < 1 || decoder->channels() > 2) {
        return fail("only mono and stereo files can be streamed (file has "
                    + std::to_string(decoder->channels()) + " channels)");
    }
    if (decoder->sampleRate() <= 0) {
        return fail("file reports an invalid sample rate");
    }

    int ringFrames = opts.ringFrames > 0 ? opts.ringFrames : sampleRate_ * 2;   // ~2 s
    int prebuffer  = opts.prebufferFrames > 0 ? opts.prebufferFrames : sampleRate_ / 2; // ~500 ms
    prebuffer = std::min(prebuffer, ringFrames / 2);

    std::shared_ptr<AudioClip> clip;
    std::shared_ptr<ClipPlayback> pb;
    // startPlaying=false: the worker releases playback once the prebuffer is
    // decoded, so the mixer never counts pre-start silence as underrun.
    int id = createStreamPlayback(decoder->channels(), ringFrames,
                                  /*startPlaying=*/false, opts.gain, opts.loop,
                                  clip, pb);
    if (id < 0) {
        return fail("cannot allocate a ring of " + std::to_string(ringFrames) + " frames");
    }

    if (decoder->totalFrames() > 0)
        clip->streamDurationSeconds.store(
            static_cast<double>(decoder->totalFrames()) / decoder->sampleRate(),
            std::memory_order_relaxed);

    auto runner = std::make_unique<FileStreamRunner>(
        std::move(clip), std::move(pb), std::move(decoder), sampleRate_, prebuffer);
    runner->start();

    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        fileStreams_.push_back(std::move(runner));
    }
    return id;
}

StreamStats Engine::getStreamStats(int instanceId) const
{
    StreamStats s;
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return s;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip || !clip->streaming) return s;

    s.valid = true;
    s.decodedFrames = clip->writeFrames.load(std::memory_order_acquire);
    s.playedFrames = pb->playPos.load(std::memory_order_relaxed);
    s.bufferedFrames = s.decodedFrames > s.playedFrames
                           ? s.decodedFrames - s.playedFrames : 0;
    s.underrunFrames = clip->streamUnderrunFrames.load(std::memory_order_relaxed);
    s.finished = clip->streamEnded.load(std::memory_order_acquire)
                 && s.bufferedFrames == 0;
    s.durationSeconds = clip->streamDurationSeconds.load(std::memory_order_relaxed);
    s.positionSeconds = getPlaybackPositionSeconds(instanceId);
    return s;
}

double Engine::getStreamDuration(int instanceId) const
{
    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return 0.0;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip || !clip->streaming) return 0.0;
    return clip->streamDurationSeconds.load(std::memory_order_relaxed);
}

void Engine::stopFileStream(int instanceId)
{
    std::unique_ptr<FileStreamRunner> runner;
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        for (auto it = fileStreams_.begin(); it != fileStreams_.end(); ++it) {
            if ((*it)->playbackId() == instanceId) {
                runner = std::move(*it);
                fileStreams_.erase(it);
                break;
            }
        }
    }
    // Join outside the lock — the worker never touches fileStreams_, but a
    // slow decode step shouldn't stall unrelated stream creation.
    if (runner) {
        runner->requestStop();
        runner->join();
    }
}

void Engine::stopAllFileStreams()
{
    std::vector<std::unique_ptr<FileStreamRunner>> runners;
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        runners.swap(fileStreams_);
    }
    for (auto& r : runners) r->requestStop();  // signal all first
    for (auto& r : runners) r->join();
}

void Engine::stopPlayback(int instanceId)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    auto current = playbacks_.load();
    auto newList = std::make_shared<PlaybackList>();
    for (auto& pb : *current) {
        if (pb->id == instanceId) {
            pb->playing.store(false, std::memory_order_relaxed);
            pb->active.store(false, std::memory_order_relaxed);
        } else {
            newList->push_back(pb);
        }
    }
    playbacks_.store(std::move(newList));
}

void Engine::stopPlaybackAt(int instanceId, double when)
{
    const double s = when * static_cast<double>(sampleRate_);
    const uint64_t now = samplesGenerated_.load(std::memory_order_relaxed);
    if (!(s > static_cast<double>(now))) {
        stopPlayback(instanceId);
        return;
    }
    if (auto* pb = findPlayback(instanceId))
        pb->stopSample.store(static_cast<uint64_t>(s + 0.5), std::memory_order_relaxed);
}

void Engine::setPlaybackGain(int instanceId, float gain, float rampSeconds)
{
    if (auto* pb = findPlayback(instanceId))
        pb->gainRamp.set(pb->gain, gain, rampSeconds);
}

void Engine::setPlaybackLoop(int instanceId, bool loop)
{
    if (auto* pb = findPlayback(instanceId))
        pb->looping.store(loop, std::memory_order_relaxed);
}

void Engine::setPlaybackPlaying(int instanceId, bool playing)
{
    if (auto* pb = findPlayback(instanceId)) {
        if (playing && !pb->playing.load(std::memory_order_relaxed))
            pb->playPos.store(0, std::memory_order_relaxed);
        pb->playing.store(playing, std::memory_order_relaxed);
    }
}

void Engine::setPlaybackRate(int instanceId, float rate)
{
    if (auto* pb = findPlayback(instanceId))
        pb->rate.store(std::clamp(rate, 0.01f, 16.0f), std::memory_order_relaxed);
}

void Engine::setPlaybackPan(int instanceId, float pan)
{
    if (auto* pb = findPlayback(instanceId))
        pb->pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Engine::setPlaybackRegion(int instanceId, int start, int end)
{
    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    if (auto* pb = findPlayback(instanceId)) {
        auto* clip = findClip(pb->clipId);
        if (!clip) return;
        int maxLen = clip->numFrames();
        int rs = std::clamp(start, 0, maxLen);
        int re = std::clamp(end, rs, maxLen);
        pb->regionStart.store(rs, std::memory_order_relaxed);
        pb->regionEnd.store(re, std::memory_order_relaxed);
        pb->playPos.store(0, std::memory_order_relaxed);
    }
}

void Engine::seekPlayback(int instanceId, double seconds)
{
    if (!(seconds > 0.0)) seconds = 0.0;  // negative, and NaN

    // Disk streams: hand the seek to the decode worker (codec seek + ring
    // flush fence). Control-plane mutex only — the audio thread never touches
    // fileStreams_.
    {
        std::lock_guard<std::mutex> lock(fileStreamsMutex_);
        for (auto& r : fileStreams_) {
            if (r->playbackId() == instanceId) {
                r->requestSeek(seconds);
                return;
            }
        }
    }

    ClipPlayback* pb = findPlayback(instanceId);
    if (!pb) return;
    AudioClip* clip = findClip(pb->clipId);
    if (!clip) return;
    if (clip->streaming) return;  // live PCM stream — nothing to seek into

    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return;

    // Compared as a double first: a huge or infinite time converted straight
    // to int64 is undefined behaviour.
    const double f = seconds * sampleRate_ + 0.5;
    const int64_t frame = f >= static_cast<double>(len) ? len - 1 : static_cast<int64_t>(f);
    // playPos is a 16.16 fixed-point cursor relative to the region start.
    // Racing the audio thread's block-end store is benign (same contract as
    // setPlaybackPlaying / setPlaybackRegion): worst case the seek lands one
    // mix block late.
    pb->playPos.store(static_cast<uint64_t>(frame) << 16, std::memory_order_relaxed);
}

double Engine::getPlaybackPositionSeconds(int instanceId) const
{
    auto* pb = findPlayback(instanceId);
    if (!pb) return 0.0;
    auto* clip = findClip(pb->clipId);
    if (!clip) return 0.0;

    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);

    if (clip->streaming) {
        // playPos counts consumed engine-rate frames; the seek fence + base
        // rebase it into file time (both zero for never-seeked streams and
        // live PCM streams).
        uint64_t flush = clip->streamFlushFrames.load(std::memory_order_acquire);
        int64_t base = clip->streamPosBaseFrames.load(std::memory_order_relaxed);
        uint64_t sinceFlush = pos > flush ? pos - flush : 0;
        double s = (static_cast<double>(base) + static_cast<double>(sinceFlush))
                   / static_cast<double>(sampleRate_);
        if (!(s > 0.0)) return 0.0;

        // A looping disk stream rewinds its decoder at EOF without a fence,
        // so the consumed count keeps growing across passes: fold it back
        // into file time. A stream that played out (not looping, EOF) reads
        // its duration rather than wrapping to the start of a pass it will
        // not play — including the few resampler-tail frames past the end.
        const double dur = clip->streamDurationSeconds.load(std::memory_order_relaxed);
        if (dur > 0.0 && s >= dur) {
            double r = std::fmod(s, dur);
            if (!pb->looping.load(std::memory_order_relaxed) &&
                clip->streamEnded.load(std::memory_order_acquire) && r < 0.05)
                r = dur;
            s = r;
        }
        return s;
    }

    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return 0.0;
    int64_t intPos = static_cast<int64_t>(pos >> 16);
    if (pb->looping.load(std::memory_order_relaxed)) intPos %= len;
    else if (intPos > len) intPos = len;
    return static_cast<double>(intPos) / static_cast<double>(sampleRate_);
}

float Engine::getPlaybackPosition(int instanceId) const
{
    auto* pb = findPlayback(instanceId);
    if (!pb) return 0.0f;
    auto* clip = findClip(pb->clipId);
    if (!clip) return 0.0f;

    if (clip->streaming) {
        // The ring is not the file: normalize file time by the file's length.
        const double dur = clip->streamDurationSeconds.load(std::memory_order_relaxed);
        if (!(dur > 0.0)) return 0.0f;
        return static_cast<float>(
            std::clamp(getPlaybackPositionSeconds(instanceId) / dur, 0.0, 1.0));
    }

    int re = pb->regionEnd.load(std::memory_order_relaxed);
    int rs = pb->regionStart.load(std::memory_order_relaxed);
    int end = re > 0 ? re : clip->numFrames();
    int len = end - rs;
    if (len <= 0) return 0.0f;
    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
    int intPos = static_cast<int>(pos >> 16);
    return static_cast<float>(intPos % len) / static_cast<float>(len);
}

// ---------------------------------------------------------------------------
// Audio file I/O convenience
// ---------------------------------------------------------------------------

int Engine::createClipFromFile(const char* path)
{
    return createClipFromFileEx(path, nullptr);
}

int Engine::createClipFromFileEx(const char* path, std::string* outError)
{
    auto fail = [&](const std::string& msg) {
        if (outError) *outError = msg;
        log(LogLevel::Error, "createClipFromFile: %s: %s",
            path ? path : "(null)", msg.c_str());
        return -1;
    };

    AudioFileData data = loadAudioFile(path);
    if (!data.valid()) {
        // Surface a known decode error (e.g. corrupt Vorbis stream) — the
        // int return can only say "failed", which reads as a bad path.
        return fail(!data.error.empty()
                        ? data.error
                        : "cannot open or decode file (supported formats: WAV, "
                          "FLAC, MP3, Ogg Vorbis)");
    }

    // Reject clips that exceed the decoded size limit
    if (maxClipDecodedBytes_ > 0 &&
        data.samples.size() * sizeof(float) > maxClipDecodedBytes_) {
        return fail("decoded audio ("
                    + std::to_string(data.samples.size() * sizeof(float) / (1024 * 1024))
                    + " MB) exceeds the clip size limit ("
                    + std::to_string(maxClipDecodedBytes_ / (1024 * 1024))
                    + " MB); use createStreamFromFile to disk-stream large files");
    }

    // Resample to engine sample rate if needed
    if (data.sampleRate != sampleRate_) {
        auto resampled = resample(data.samples.data(), data.numFrames,
                                  data.channels, data.sampleRate, sampleRate_);
        if (resampled.empty()) return fail("resampling failed");
        return createClip(resampled.data(),
                          static_cast<int>(resampled.size()),
                          data.channels);
    }

    return createClip(data.samples.data(),
                      static_cast<int>(data.samples.size()),
                      data.channels);
}

std::future<int> Engine::createClipFromFileAsync(const char* path)
{
    std::string pathStr(path ? path : "");
    return std::async(std::launch::async, [this, pathStr]() -> int {
        // Decode + resample run on this background thread; createClip locks
        // the control-plane media mutex, which is safe off the main thread.
        return createClipFromFileEx(pathStr.c_str(), nullptr);
    });
}

bool Engine::exportRecordingToWav(const char* path)
{
    if (recordOutput_.empty()) return false;
    const int ch = recordOutputChannels_;
    int numFrames = static_cast<int>(recordOutput_.size() / static_cast<size_t>(ch));
    return saveWav(path, recordOutput_.data(), numFrames, ch, sampleRate_);
}

} // namespace broaudio
