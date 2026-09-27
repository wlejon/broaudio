#pragma once

// SynthGraph (host_audio_synth_graph.cpp): the JS face of synthesis graphs
// (broaudio/synth/synth_graph.h). Kept out of host_audio_internal.h.

#include "object_builder.h"

namespace broaudio::api {

// The global `SynthGraph` class (no engine needed: construct and render work
// in any realm, a Worker's included).
void installSynthGraphClass();

// AudioContext.prototype: createSynthGraph, playSynth, releaseSynth.
void registerAudioContextSynthGraph(ObjectBuilder& b);

} // namespace broaudio::api
