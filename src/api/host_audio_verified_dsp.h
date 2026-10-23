#pragma once

// JavaScript bindings for Live Lock-Free Verified JS Audio DSP Kernels
// (`audioCtx.createVerifiedDspNode`, `audioCtx.compileDspKernel`,
//  `bro.audio.createVerifiedDspNode`, `bro.audio.compileDspKernel`,
//  and global `VerifiedDspNode`).

#include "host_audio_internal.h"

namespace broaudio::api {

void attachAudioContextVerifiedDspMethods(ObjectBuilder& b);
void attachBroAudioVerifiedDspMethods(ObjectBuilder& b);
void installAudioVerifiedDspGlobals();

} // namespace broaudio::api
