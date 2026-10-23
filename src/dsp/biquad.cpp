#include "broaudio/dsp/biquad.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace broaudio {

void BiquadFilter::computeCoefficients(int sampleRate)
{
    const float sr = sampleRate > 0 ? static_cast<float>(sampleRate) : 44100.0f;
    const float nyquist = std::max(20.0f, sr * 0.499f);
    const float safeFreq = std::isfinite(frequency) ? std::clamp(frequency, 1.0f, nyquist) : 1000.0f;
    const float safeQ = (std::isfinite(Q) && Q > 0.0f) ? std::clamp(Q, 0.0001f, 1000.0f) : 1.0f;
    const float safeGainDB = std::isfinite(gainDB) ? std::clamp(gainDB, -120.0f, 120.0f) : 0.0f;

    float w0 = 2.0f * std::numbers::pi_v<float> * safeFreq / sr;
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.0f * safeQ);

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
            float A = std::pow(10.0f, safeGainDB / 40.0f);
            nb0 = 1.0f + alpha * A;
            nb1 = -2.0f * cosW0;
            nb2 = 1.0f - alpha * A;
            a0 = 1.0f + alpha / A;
            na1 = -2.0f * cosW0;
            na2 = 1.0f - alpha / A;
            break;
        }
        case Type::Lowshelf: {
            float A = std::pow(10.0f, safeGainDB / 40.0f);
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
            float A = std::pow(10.0f, safeGainDB / 40.0f);
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

    if (!std::isfinite(a0) || std::abs(a0) < 1e-12f) {
        b0t = 1.0f; b1t = 0.0f; b2t = 0.0f;
        a1t = 0.0f; a2t = 0.0f;
        return;
    }

    float cb0 = nb0 / a0;
    float cb1 = nb1 / a0;
    float cb2 = nb2 / a0;
    float ca1 = na1 / a0;
    float ca2 = na2 / a0;

    if (!std::isfinite(cb0) || !std::isfinite(cb1) || !std::isfinite(cb2) ||
        !std::isfinite(ca1) || !std::isfinite(ca2)) {
        b0t = 1.0f; b1t = 0.0f; b2t = 0.0f;
        a1t = 0.0f; a2t = 0.0f;
        return;
    }

    b0t = cb0;
    b1t = cb1;
    b2t = cb2;
    a1t = ca1;
    a2t = ca2;
}

} // namespace broaudio
