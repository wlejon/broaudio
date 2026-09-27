#pragma once

// bro.ear internals shared by host_audio_ear.cpp (measure / compare /
// spectrogram) and the fit binding (host_audio_ear_fit*.cpp). Kept out of
// host_audio_internal.h.

#include "object_builder.h"

#include <broaudio/ear/ear.h>

namespace broaudio::api {

// Reads an EarClip argument (a path, an AudioBuffer, {samples, sampleRate,
// channels?} or a bare Float32Array at `fallbackRate`) into `out`, mixed to
// mono. A bad argument throws into the script (the embed throw* calls
// unwind); returns false only after throwing.
bool readEarClip(Value v, double fallbackRate, const char* who, broaudio::ear::Clip& out);

// compare()'s result object.
Value earComparisonValue(const broaudio::ear::Comparison& r);

// {samples: Float32Array, sampleRate, channels: 1}: a clip every bro.ear
// function (and a CLAP model's score) accepts.
Value earClipValue(const broaudio::ear::Clip& c);

// bro.ear.fit (host_audio_ear_fit.cpp).
void registerEarFit(ObjectBuilder& ear);

// The async fits' JS-thread half (host_audio_ear_fit_job.cpp): scoring
// requests, progress and completion are delivered here. tickAsyncJobs calls
// the first; shutdownAsyncJobs the second.
void tickEarFitJobs();
void shutdownEarFitJobs();

} // namespace broaudio::api
