#pragma once

#include "broaudio/types.h"
#include "broaudio/atomic_shared_ptr.h"
#include "broaudio/rcu.h"
#include "broaudio/core/ring_buffer.h"
#include "broaudio/dsp/params.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/compressor.h"
#include "broaudio/dsp/delay.h"
#include "broaudio/dsp/limiter.h"
#include "broaudio/mix/bus.h"
#include "broaudio/synth/modulation.h"
#include "broaudio/synth/voice.h"
#include "broaudio/synth/wavetable.h"
#include "broaudio/synth/synth_graph.h"
#include "broaudio/clip/clip.h"
#include "broaudio/mic_tap.h"
#include "broaudio/spatial/listener.h"
#include "broaudio/spatial/air_absorption.h"
#include "broaudio/spatial/voice_jit.h"
#include "broaudio/io/audio_file.h"
#include "broaudio/io/serialization.h"
#include "broaudio/dsp/jit/jit_compiler.h"
#include "broaudio/device.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace broaudio {

class FileStreamRunner;
class StreamResampler;   // private (src/device/stream_resampler.h)

// Device options for Engine::init. All optional.
struct AudioDeviceConfig {
    AudioBackendKind backend = AudioBackendKind::Auto;
    // Identity shown by the system mixer (PipeWire node / application
    // properties, SDL's app name). Empty = "broaudio".
    std::string appId;
    std::string appName;
    // Requested device period in frames at the engine rate (a hint; see
    // AudioStreamConfig::periodFrames). The engine processes in blocks of up
    // to MAX_SCRATCH_FRAMES regardless.
    int periodFrames = 128;
    // AudioDeviceInfo ids; empty = the system default, followed as it changes.
    std::string outputDevice;
    std::string inputDevice;
    // PipeWire media.role of the output stream; empty = none.
    std::string outputRole;
};

// Options for createStreamFromFile. Frame counts are at the ENGINE sample
// rate (the ring holds resampled audio).
struct FileStreamOptions {
    // Ring capacity; 0 = ~2 s at the engine rate. Small rings are honored —
    // the decode worker tops the ring up at least whenever it falls below
    // half full — but they leave less slack for a slow disk, so underruns
    // get likelier the smaller this is.
    int   ringFrames = 0;
    int   prebufferFrames = 0;  // decoded before playback starts; 0 = ~500 ms
    bool  loop = false;         // worker rewinds the decoder at EOF (seamless)
    float gain = 1.0f;
};

// Snapshot of a streaming playback's ring state (disk or live PCM streams).
struct StreamStats {
    uint64_t decodedFrames = 0;   // total frames ever pushed into the ring
    uint64_t playedFrames = 0;    // frames consumed by the mixer
    uint64_t bufferedFrames = 0;  // decoded - played (waiting in the ring)
    uint64_t underrunFrames = 0;  // silent frames emitted while starved
    bool finished = false;        // disk stream: EOF reached and ring drained
    bool valid = false;           // false when the id is not a streaming playback
    // Disk stream: the file's length in seconds (0 = unknown, and always 0
    // for live PCM streams). positionSeconds is getPlaybackPositionSeconds.
    double durationSeconds = 0.0;
    double positionSeconds = 0.0;
};

} // namespace broaudio
