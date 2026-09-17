#pragma once

#include "broaudio/engine.h"

namespace broaudio::api {

/// Mount Web Audio API constructors and globals (AudioContext, AudioNode, GainNode,
/// OscillatorNode, BiquadFilterNode, AnalyserNode, AudioBuffer, PannerNode, DelayNode, etc.)
/// into the current Bronze realm.
void installAudio();

/// Mount the `bro.mic` live audio streaming API into the current Bronze realm.
void installMic();

/// Drain pending microphone audio chunks into JS onChunk callback.
/// Safe and recommended to invoke once per frame / tick from the main JS thread.
void drainMicChunks();

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
