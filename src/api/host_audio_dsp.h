#pragma once

#include "host_audio_internal.h"

namespace broaudio::api {

void processDynamicsCompressor(HostDynamicsCompressorNode* comp,
                               float* buffer,
                               int frames,
                               int channels,
                               int sampleRate,
                               double curTime);

} // namespace broaudio::api
