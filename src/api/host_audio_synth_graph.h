#pragma once

// SynthGraph (host_audio_synth_graph.cpp): the JS face of synthesis graphs
// (broaudio/synth/synth_graph.h). Kept out of host_audio_internal.h.

#include "object_builder.h"

#include <memory>

namespace broaudio {
class SynthGraph;
}

namespace broaudio::api {

// The graph a SynthGraph argument names: an instance, or a description
// (a plain object or its JSON) parsed on the spot; a bad description throws
// the constructor's TypeError / RangeError into the script.
std::shared_ptr<const SynthGraph> synthGraphFromValue(Value v);

// The global `SynthGraph` class (no engine needed: construct and render work
// in any realm, a Worker's included).
void installSynthGraphClass();

// AudioContext.prototype: createSynthGraph, playSynth, releaseSynth.
void registerAudioContextSynthGraph(ObjectBuilder& b);

} // namespace broaudio::api
