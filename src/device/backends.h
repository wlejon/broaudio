#pragma once

// Backend factories behind createAudioBackend (device.cpp). Private to
// broaudio: each backend's own headers stay inside its translation unit.

#include "broaudio/device.h"

#include <memory>
#include <string>

namespace broaudio::detail {

// Initialises SDL's audio subsystem (refcounted by SDL); null on failure.
std::unique_ptr<AudioBackend> createSdlAudioBackend(std::string* error);

// Never fails.
std::unique_ptr<AudioBackend> createNullAudioBackend();

#if BROAUDIO_HAS_PIPEWIRE
// Connects to the PipeWire daemon; null when none is reachable.
std::unique_ptr<AudioBackend> createPipeWireAudioBackend(std::string* error);
#endif

} // namespace broaudio::detail
