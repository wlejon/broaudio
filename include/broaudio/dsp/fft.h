#pragma once

namespace broaudio {

// In-place radix-2 Cooley-Tukey FFT. n must be a power of 2.
void fft(float* real, float* imag, int n);

} // namespace broaudio
