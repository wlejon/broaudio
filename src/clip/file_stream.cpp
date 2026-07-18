#include "broaudio/clip/file_stream.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>

namespace broaudio {

// Frames pulled from the decoder per step (at the file's rate). Small enough
// to keep worker latency low, large enough that a 2 s ring refills in a few
// dozen pulls.
static constexpr int kChunkFrames = 4096;

// Worker wake cadence. The ring holds seconds of audio, so a 50 ms tick gives
// the worker dozens of chances to top it up before the mixer can starve.
static constexpr auto kWakeInterval = std::chrono::milliseconds(50);

FileStreamRunner::FileStreamRunner(std::shared_ptr<AudioClip> clip,
                                   std::shared_ptr<ClipPlayback> playback,
                                   std::unique_ptr<AudioFileStream> decoder,
                                   int engineRate, int prebufferFrames)
    : clip_(std::move(clip)),
      playback_(std::move(playback)),
      decoder_(std::move(decoder)),
      engineRate_(engineRate),
      prebufferFrames_(prebufferFrames),
      playbackId_(playback_->id)
{
    decodeBuf_.resize(static_cast<size_t>(kChunkFrames) * clip_->channels);
}

FileStreamRunner::~FileStreamRunner()
{
    requestStop();
    join();
    if (resampler_) {
        SDL_DestroyAudioStream(resampler_);
        resampler_ = nullptr;
    }
}

void FileStreamRunner::start()
{
    thread_ = std::thread([this] { worker(); });
}

void FileStreamRunner::requestStop()
{
    stop_.store(true, std::memory_order_release);
    {
        // Lock so a worker between its stop_ check and cv wait can't miss the
        // notify and sleep a full interval (harmless but sloppy on teardown).
        std::lock_guard<std::mutex> lk(mutex_);
    }
    cv_.notify_all();
}

void FileStreamRunner::join()
{
    if (thread_.joinable()) thread_.join();
}

void FileStreamRunner::requestSeek(double seconds)
{
    seekSeconds_.store(seconds < 0.0 ? 0.0 : seconds, std::memory_order_release);
    {
        // Same lock-then-notify as requestStop: a worker between its check
        // and the cv wait must not miss the wake and add a full interval of
        // seek latency.
        std::lock_guard<std::mutex> lk(mutex_);
    }
    cv_.notify_all();
}

void FileStreamRunner::performSeek(double seconds)
{
    const int fileRate = decoder_->sampleRate();
    if (fileRate <= 0) return;

    uint64_t target = static_cast<uint64_t>(seconds * fileRate + 0.5);
    uint64_t total = decoder_->totalFrames();
    if (total > 0 && target >= total) target = total > 0 ? total - 1 : 0;

    if (!decoder_->seekToFrame(target))
        return;  // unseekable — keep playing from the current position

    // Drop everything decoded but not yet heard: resampler tail + pending
    // frames on this thread, then fence off what already sits in the ring.
    if (resampler_) SDL_ClearAudioStream(resampler_);
    pending_.clear();
    pendingOffsetFrames_ = 0;
    decoderEof_ = false;
    clip_->streamEnded.store(false, std::memory_order_release);

    // Publish position base BEFORE the fence (readers pair them loosely; a
    // torn read only skews a position query for one block).
    clip_->streamPosBaseFrames.store(
        static_cast<int64_t>(static_cast<double>(target) * engineRate_ / fileRate + 0.5),
        std::memory_order_relaxed);
    clip_->streamFlushFrames.store(
        clip_->writeFrames.load(std::memory_order_relaxed),
        std::memory_order_release);
}

void FileStreamRunner::worker()
{
    // Streaming resampler (SDL_AudioStream keeps filter state across chunks —
    // the offline polyphase resample() would reset per chunk and click).
    if (decoder_->sampleRate() != engineRate_) {
        SDL_AudioSpec src{SDL_AUDIO_F32, clip_->channels, decoder_->sampleRate()};
        SDL_AudioSpec dst{SDL_AUDIO_F32, clip_->channels, engineRate_};
        resampler_ = SDL_CreateAudioStream(&src, &dst);
        // On failure fall through with resampler_ == nullptr: audio plays at
        // the wrong pitch rather than not at all, and creation only fails in
        // out-of-memory-grade situations.
    }

    while (!stop_.load(std::memory_order_acquire)) {
        // Apply a pending seek before decoding (also breaks the fill loop
        // below so a seek during a long ring top-up applies promptly).
        double sk = seekSeconds_.exchange(-1.0, std::memory_order_acq_rel);
        if (sk >= 0.0) performSeek(sk);

        // Top the ring up until it's full, the file is done, we're told to
        // stop, or a new seek arrives.
        while (!stop_.load(std::memory_order_acquire) &&
               seekSeconds_.load(std::memory_order_acquire) < 0.0 && step()) {}

        // Release playback once the prebuffer is decoded (or the whole file
        // is, if it's shorter than the prebuffer).
        if (!started_ &&
            (clip_->writeFrames.load(std::memory_order_relaxed) >=
                 static_cast<uint64_t>(prebufferFrames_) ||
             decoderEof_)) {
            started_ = true;
            playback_->playing.store(true, std::memory_order_release);
        }

        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait_for(lk, kWakeInterval, [this] {
            return stop_.load(std::memory_order_acquire) ||
                   seekSeconds_.load(std::memory_order_acquire) >= 0.0;
        });
    }
}

// Push as much of pending_ as fits. Space is recomputed by the caller.
void FileStreamRunner::pushPending(uint64_t space)
{
    const int ch = clip_->channels;
    size_t pendingFrames = pending_.size() / ch - pendingOffsetFrames_;
    if (pendingFrames == 0) return;

    size_t toPush = std::min<uint64_t>(pendingFrames, space);
    if (toPush > 0) {
        pushStreamFrames(*clip_, pending_.data() + pendingOffsetFrames_ * ch,
                         static_cast<int>(toPush));
        pendingOffsetFrames_ += toPush;
    }
    if (pendingOffsetFrames_ * static_cast<size_t>(ch) >= pending_.size()) {
        pending_.clear();
        pendingOffsetFrames_ = 0;
    }
}

bool FileStreamRunner::step()
{
    const int ch = clip_->channels;
    const uint64_t cap = static_cast<uint64_t>(clip_->ringFrames);

    uint64_t wf = clip_->writeFrames.load(std::memory_order_relaxed);
    uint64_t rf = playback_->playPos.load(std::memory_order_relaxed);
    uint64_t buffered = wf > rf ? wf - rf : 0;
    uint64_t space = cap > buffered ? cap - buffered : 0;

    // Deliver frames already decoded but not yet pushed.
    if (pending_.size() / ch > pendingOffsetFrames_) {
        pushPending(space);
        if (!pending_.empty()) return false;  // ring full — wait for the mixer
        wf = clip_->writeFrames.load(std::memory_order_relaxed);
        buffered = wf > rf ? wf - rf : 0;
        space = cap > buffered ? cap - buffered : 0;
    }

    if (decoderEof_) {
        // Loop enabled after the file finished: restart from the top.
        if (playback_->looping.load(std::memory_order_relaxed) &&
            decoder_->seekToStart()) {
            decoderEof_ = false;
            clip_->streamEnded.store(false, std::memory_order_release);
            return true;
        }
        clip_->streamEnded.store(true, std::memory_order_release);
        return false;  // parked until stop (or a late loop enable)
    }

    // Make sure a full chunk of output fits before decoding, so decoded audio
    // never sits split across ring pushes longer than necessary.
    uint64_t maxOut = kChunkFrames;
    if (resampler_)
        maxOut = static_cast<uint64_t>(kChunkFrames) * engineRate_ /
                     std::max(decoder_->sampleRate(), 1) + 64;
    if (space < maxOut) return false;

    int got = decoder_->readFrames(decodeBuf_.data(), kChunkFrames);
    if (got > 0) {
        if (resampler_) {
            SDL_PutAudioStreamData(resampler_, decodeBuf_.data(),
                                   got * ch * static_cast<int>(sizeof(float)));
        } else {
            pending_.assign(decodeBuf_.begin(),
                            decodeBuf_.begin() + static_cast<size_t>(got) * ch);
            pendingOffsetFrames_ = 0;
        }
    } else {
        // Decoder EOF: seamless loop rewind, or flush the resampler tail and end.
        if (playback_->looping.load(std::memory_order_relaxed) &&
            decoder_->seekToStart()) {
            return true;
        }
        if (resampler_) SDL_FlushAudioStream(resampler_);
        decoderEof_ = true;
    }

    if (resampler_) {
        int availBytes = SDL_GetAudioStreamAvailable(resampler_);
        if (availBytes > 0) {
            size_t oldSize = pending_.size();
            pending_.resize(oldSize + static_cast<size_t>(availBytes) / sizeof(float));
            int gotBytes = SDL_GetAudioStreamData(
                resampler_, pending_.data() + oldSize, availBytes);
            if (gotBytes >= 0 && gotBytes < availBytes)
                pending_.resize(oldSize + static_cast<size_t>(gotBytes) / sizeof(float));
        }
    }

    pushPending(space);
    return true;
}

} // namespace broaudio
