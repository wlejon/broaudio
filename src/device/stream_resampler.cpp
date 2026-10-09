#include "stream_resampler.h"

#include <SDL3/SDL.h>

namespace broaudio {

namespace {
SDL_AudioStream* as(void* p) { return static_cast<SDL_AudioStream*>(p); }
}

std::unique_ptr<StreamResampler> StreamResampler::create(int channels, int srcRate, int dstRate)
{
    if (channels < 1 || srcRate <= 0 || dstRate <= 0) return nullptr;
    SDL_AudioSpec src{}, dst{};
    src.format = SDL_AUDIO_F32; src.channels = channels; src.freq = srcRate;
    dst.format = SDL_AUDIO_F32; dst.channels = channels; dst.freq = dstRate;
    // SDL_CreateAudioStream needs no SDL_Init: it is a plain converter.
    SDL_AudioStream* s = SDL_CreateAudioStream(&src, &dst);
    if (!s) return nullptr;
    std::unique_ptr<StreamResampler> r(new StreamResampler());
    r->stream_ = s;
    r->frameBytes_ = channels * static_cast<int>(sizeof(float));
    return r;
}

StreamResampler::~StreamResampler()
{
    if (stream_) SDL_DestroyAudioStream(as(stream_));
}

bool StreamResampler::put(const float* frames, int numFrames)
{
    if (numFrames <= 0) return true;
    return SDL_PutAudioStreamData(as(stream_), frames, numFrames * frameBytes_);
}

int StreamResampler::available() const
{
    int bytes = SDL_GetAudioStreamAvailable(as(stream_));
    return bytes > 0 ? bytes / frameBytes_ : 0;
}

int StreamResampler::get(float* out, int maxFrames)
{
    if (maxFrames <= 0) return 0;
    int got = SDL_GetAudioStreamData(as(stream_), out, maxFrames * frameBytes_);
    return got > 0 ? got / frameBytes_ : 0;
}

void StreamResampler::flush() { SDL_FlushAudioStream(as(stream_)); }

void StreamResampler::clear() { SDL_ClearAudioStream(as(stream_)); }

} // namespace broaudio
