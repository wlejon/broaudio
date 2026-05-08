#include "broaudio/dsp/biquad.h"
#include <cmath>
#include <numbers>

namespace broaudio {

void BiquadFilter::computeCoefficients(int sampleRate)
{
    float w0 = 2.0f * std::numbers::pi_v<float> * frequency / static_cast<float>(sampleRate);
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.0f * Q);

    float a0 = 1.0f;
    float nb0 = b0t, nb1 = b1t, nb2 = b2t;
    float na1 = a1t, na2 = a2t;

    switch (type) {
        case Type::Lowpass:
            nb0 = (1.0f - cosW0) / 2.0f;
            nb1 = 1.0f - cosW0;
            nb2 = (1.0f - cosW0) / 2.0f;
            a0 = 1.0f + alpha;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha;
            break;
        case Type::Highpass:
            nb0 = (1.0f + cosW0) / 2.0f;
            nb1 = -(1.0f + cosW0);
            nb2 = (1.0f + cosW0) / 2.0f;
            a0 = 1.0f + alpha;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha;
            break;
        case Type::Bandpass:
            nb0 = alpha;
            nb1 = 0.0f;
            nb2 = -alpha;
            a0 = 1.0f + alpha;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha;
            break;
        case Type::Notch:
            nb0 = 1.0f;
            nb1 = -2.0f * cosW0;
            nb2 = 1.0f;
            a0 = 1.0f + alpha;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha;
            break;
        case Type::Allpass:
            nb0 = 1.0f - alpha;
            nb1 = -2.0f * cosW0;
            nb2 = 1.0f + alpha;
            a0 = 1.0f + alpha;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha;
            break;
        case Type::Peaking: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            nb0 = 1.0f + alpha * A;
            nb1 = -2.0f * cosW0;
            nb2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha / A;
            break;
        }
        case Type::Lowshelf: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha);
            nb1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosW0);
            nb2 = A * ((A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha;
            na1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosW0);
            na2 = (A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha;
            break;
        }
        case Type::Highshelf: {
            float A = std::pow(10.0f, gainDB / 40.0f);
            float twoSqrtAAlpha = 2.0f * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 + twoSqrtAAlpha);
            nb1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosW0);
            nb2 = A * ((A + 1.0f) + (A - 1.0f) * cosW0 - twoSqrtAAlpha);
            a0 = (A + 1.0f) - (A - 1.0f) * cosW0 + twoSqrtAAlpha;
            na1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosW0);
            na2 = (A + 1.0f) - (A - 1.0f) * cosW0 - twoSqrtAAlpha;
            break;
        }
    }

    b0t = nb0 / a0;
    b1t = nb1 / a0;
    b2t = nb2 / a0;
    a1t = na1 / a0;
    a2t = na2 / a0;
}

} // namespace broaudio
