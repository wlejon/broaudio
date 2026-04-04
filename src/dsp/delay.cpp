#include "broaudio/dsp/delay.h"

namespace broaudio {

void DelayEffect::init(int maxDelaySamples)
{
    buffer.assign(maxDelaySamples * 2, 0.0f);
    writePos = 0;
}

void DelayEffect::processStereo(float* buf, int numFrames)
{
    int bufFrames = static_cast<int>(buffer.size()) / 2;
    if (bufFrames == 0 || delaySamples <= 0) return;

    for (int i = 0; i < numFrames; i++) {
        int readPos = (writePos - delaySamples + bufFrames) % bufFrames;
        for (int ch = 0; ch < 2; ch++) {
            float delayed = buffer[readPos * 2 + ch];
            float dry = buf[i * 2 + ch];
            buf[i * 2 + ch] = dry * (1.0f - mix) + delayed * mix;
            buffer[writePos * 2 + ch] = dry + delayed * feedback;
        }
        writePos = (writePos + 1) % bufFrames;
    }
}

} // namespace broaudio
