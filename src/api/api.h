#pragma once

#include "broaudio/engine.h"

#include <functional>
#include <string>

namespace broaudio::api {

/// Mount Web Audio API constructors and globals (AudioContext, AudioNode, GainNode,
/// OscillatorNode, BiquadFilterNode, AnalyserNode, AudioBuffer, PannerNode, DelayNode, etc.)
/// into the current Bronze realm.
void installAudio();

/// Mount the `bro.mic` live audio streaming API into the current Bronze realm.
void installMic();

/// Drain pending microphone audio chunks into JS onChunk callback, and settle
/// the background work the AudioContext started (tickAsyncJobs below).
/// Safe and recommended to invoke once per frame / tick from the main JS thread.
void drainMicChunks();

/// Settle background work the JS API started on worker threads — today
/// `createClipFromFileAsync`, which decodes and resamples off the JS thread
/// and resolves its promise here. Also the control-rate tick of AudioParam
/// automation: scheduled ramps and curves are evaluated here and written to
/// the sources already playing, and a finished AudioBufferSourceNode's
/// `onended` handler runs here. Joins the finished jobs and resolves or
/// rejects their promises on the calling (JS) thread; never blocks on work
/// still running. drainMicChunks() calls it, so a host that already pumps
/// the mic once per frame gets it for free; a host that does not calls this
/// once per frame itself. Settling runs promise reactions only when the
/// host next drains microtasks.
void tickAsyncJobs();

/// How a path handed to the file loaders and savers (`createClipFromFile`,
/// `createClipFromFileAsync`, `createStreamFromFile`, `decodeAudioFile`,
/// `saveWav`, `exportRecordingToWav`, `savePreset`, `loadPreset`) becomes a
/// filesystem path. Unset, the path is used as given; a host sets its `fs`
/// resolver so a relative path or a mount path ("/app/assets/hit.ogg") means
/// what it means to the app. A path about to be WRITTEN resolves through its
/// parent directory (which exists) so the file lands beside it. Process-wide:
/// every realm shares the host's filesystem.
void setPathResolver(std::function<std::string(const std::string&)> resolver);

/// Retrieve the active broaudio::Engine pointer.
/// If no engine has been set via setAudioEngine, lazily creates and initializes
/// a shared default engine instance (trying device output first, falling back to headless).
broaudio::Engine* getAudioEngine();

/// Explicitly provide an external broaudio::Engine to back the Web Audio and Mic APIs.
void setAudioEngine(broaudio::Engine* engine);

/// Shut down and reset the default engine instance if created.
void shutdownAudio();

} // namespace broaudio::api

// Global namespace aliases for compatibility
using broaudio::api::installAudio;
using broaudio::api::installMic;
using broaudio::api::shutdownAudio;
