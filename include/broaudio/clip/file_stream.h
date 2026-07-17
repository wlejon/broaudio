#pragma once

#include "broaudio/clip/clip.h"
#include "broaudio/io/audio_stream.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct SDL_AudioStream;

namespace broaudio {

// Disk-streamed clip source (Engine::createStreamFromFile): a cold worker
// thread pulls PCM from an AudioFileStream, resamples to the engine rate when
// needed, and appends into the clip's streaming ring. The real-time audio
// thread only consumes the ring (Engine::mixStreamPlayback) — it never takes
// a lock, touches the decoder, or blocks; on underrun it emits silence and
// bumps clip->streamUnderrunFrames.
//
// Threading: the data plane is the same SPSC ring discipline as live PCM
// streams (worker = single producer via pushStreamFrames, audio thread =
// consumer via acquire-loaded writeFrames). The control plane (start /
// requestStop / join) is mutex+condvar and main-thread only.
//
// Lifecycle: playback starts automatically once `prebufferFrames` are decoded
// (or the whole file is, if shorter). Looping follows the playback's live
// `looping` flag — the worker rewinds the decoder at EOF, so loops are
// seamless; enabling loop after the stream already finished restarts decode
// from the top of the file. When the stream ends (EOF, not looping) the
// worker waits for the ring to drain, then parks; Engine::closeStream stops
// and joins it.
class FileStreamRunner {
public:
    FileStreamRunner(std::shared_ptr<AudioClip> clip,
                     std::shared_ptr<ClipPlayback> playback,
                     std::unique_ptr<AudioFileStream> decoder,
                     int engineRate, int prebufferFrames);
    ~FileStreamRunner();  // requestStop + join

    FileStreamRunner(const FileStreamRunner&) = delete;
    FileStreamRunner& operator=(const FileStreamRunner&) = delete;

    void start();        // spawn the worker thread (call once)
    void requestStop();  // signal the worker to exit (non-blocking)
    void join();         // wait for the worker to exit

    int playbackId() const { return playbackId_; }

private:
    void worker();
    // Decode/resample one chunk and push what fits into the ring.
    // Returns true while forward progress is possible without waiting.
    bool step();
    void pushPending(uint64_t space);

    std::shared_ptr<AudioClip> clip_;
    std::shared_ptr<ClipPlayback> playback_;
    std::unique_ptr<AudioFileStream> decoder_;
    SDL_AudioStream* resampler_ = nullptr;  // null when file rate == engine rate
    int engineRate_ = 0;
    int prebufferFrames_ = 0;
    int playbackId_ = 0;

    std::vector<float> decodeBuf_;   // chunk from the decoder (file rate)
    std::vector<float> pending_;     // engine-rate frames awaiting ring space
    size_t pendingOffsetFrames_ = 0;
    bool decoderEof_ = false;        // decoder exhausted and resampler flushed
    bool started_ = false;           // playback released after prebuffer

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace broaudio
