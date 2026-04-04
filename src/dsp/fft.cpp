#include "broaudio/dsp/fft.h"
#include <cmath>
#include <algorithm>
#include <numbers>

namespace broaudio {

void fft(float* real, float* imag, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * std::numbers::pi_v<float> / static_cast<float>(len);
        float wReal = std::cos(angle);
        float wImag = std::sin(angle);
        for (int i = 0; i < n; i += len) {
            float curReal = 1.0f, curImag = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float tReal = curReal * real[i + j + len/2] - curImag * imag[i + j + len/2];
                float tImag = curReal * imag[i + j + len/2] + curImag * real[i + j + len/2];
                real[i + j + len/2] = real[i + j] - tReal;
                imag[i + j + len/2] = imag[i + j] - tImag;
                real[i + j] += tReal;
                imag[i + j] += tImag;
                float newCurReal = curReal * wReal - curImag * wImag;
                curImag = curReal * wImag + curImag * wReal;
                curReal = newCurReal;
            }
        }
    }
}

} // namespace broaudio
