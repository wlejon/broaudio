#include "broaudio/dsp/partitioned_convolver.h"
#include "broaudio/dsp/param_ramp.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace broaudio {

std::shared_ptr<PartitionedConvolver>
PartitionedConvolver::create(const float* ir, int frames, int channels, int blockSize)
{
    if (!ir || frames <= 0 || (channels != 1 && channels != 2)) return nullptr;
    blockSize = std::clamp(blockSize, 16, 8192);
    blockSize = static_cast<int>(std::bit_ceil(static_cast<unsigned>(blockSize)));

    std::shared_ptr<PartitionedConvolver> c(new (std::nothrow) PartitionedConvolver());
    if (!c) return nullptr;
    try {
        c->B_ = blockSize;
        c->N_ = 2 * blockSize;
        c->bins_ = blockSize + 1;
        c->P_ = (frames + blockSize - 1) / blockSize;
        c->irCh_ = channels;
        c->irFrames_ = frames;
        const int N = c->N_;

        c->cosT_.resize(N / 2);
        c->sinT_.resize(N / 2);
        for (int k = 0; k < N / 2; ++k) {
            const double a = 2.0 * 3.14159265358979323846 * k / N;
            c->cosT_[k] = static_cast<float>(std::cos(a));
            c->sinT_[k] = static_cast<float>(-std::sin(a));
        }
        c->bitrev_.resize(N);
        const int bits = std::countr_zero(static_cast<unsigned>(N));
        for (int i = 0; i < N; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            c->bitrev_[i] = r;
        }

        c->workRe_.assign(N, 0.0f);
        c->workIm_.assign(N, 0.0f);
        c->accRe_.assign(c->bins_, 0.0f);
        c->accIm_.assign(c->bins_, 0.0f);
        const size_t spec = static_cast<size_t>(c->P_) * c->bins_;
        for (int ch = 0; ch < 2; ++ch) {
            c->fdlRe_[ch].assign(spec, 0.0f);
            c->fdlIm_[ch].assign(spec, 0.0f);
            c->time_[ch].assign(N, 0.0f);
            c->outBlk_[ch].assign(blockSize, 0.0f);
        }
        for (int ch = 0; ch < channels; ++ch) {
            c->irRe_[ch].assign(spec, 0.0f);
            c->irIm_[ch].assign(spec, 0.0f);
            for (int p = 0; p < c->P_; ++p) {
                std::fill(c->workRe_.begin(), c->workRe_.end(), 0.0f);
                std::fill(c->workIm_.begin(), c->workIm_.end(), 0.0f);
                for (int k = 0; k < blockSize; ++k) {
                    const int f = p * blockSize + k;
                    if (f >= frames) break;
                    c->workRe_[k] = ir[static_cast<size_t>(f) * channels + ch];
                }
                c->fft(c->workRe_.data(), c->workIm_.data());
                std::memcpy(&c->irRe_[ch][static_cast<size_t>(p) * c->bins_], c->workRe_.data(), c->bins_ * sizeof(float));
                std::memcpy(&c->irIm_[ch][static_cast<size_t>(p) * c->bins_], c->workIm_.data(), c->bins_ * sizeof(float));
            }
        }
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
    return c;
}

void PartitionedConvolver::fft(float* re, float* im) const
{
    const int N = N_;
    for (int i = 0; i < N; ++i) {
        const int j = bitrev_[i];
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= N; len <<= 1) {
        const int half = len >> 1;
        const int stride = N / len;
        for (int i = 0; i < N; i += len) {
            for (int j = 0; j < half; ++j) {
                const float wr = cosT_[j * stride];
                const float wi = sinT_[j * stride];
                const int a = i + j, b = a + half;
                const float vr = re[b] * wr - im[b] * wi;
                const float vi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - vr;
                im[b] = im[a] - vi;
                re[a] += vr;
                im[a] += vi;
            }
        }
    }
}

void PartitionedConvolver::clear()
{
    for (int ch = 0; ch < 2; ++ch) {
        std::fill(fdlRe_[ch].begin(), fdlRe_[ch].end(), 0.0f);
        std::fill(fdlIm_[ch].begin(), fdlIm_[ch].end(), 0.0f);
        std::fill(time_[ch].begin(), time_[ch].end(), 0.0f);
        std::fill(outBlk_[ch].begin(), outBlk_[ch].end(), 0.0f);
    }
    head_ = 0;
    pos_ = 0;
}

void PartitionedConvolver::processBlock()
{
    const int B = B_, N = N_, bins = bins_, P = P_;
    const float invN = 1.0f / static_cast<float>(N);
    for (int ch = 0; ch < 2; ++ch) {
        // Forward transform of [previous block | current block].
        std::memcpy(workRe_.data(), time_[ch].data(), N * sizeof(float));
        std::fill(workIm_.begin(), workIm_.end(), 0.0f);
        fft(workRe_.data(), workIm_.data());
        float* sr = &fdlRe_[ch][static_cast<size_t>(head_) * bins];
        float* si = &fdlIm_[ch][static_cast<size_t>(head_) * bins];
        std::memcpy(sr, workRe_.data(), bins * sizeof(float));
        std::memcpy(si, workIm_.data(), bins * sizeof(float));
        std::memcpy(time_[ch].data(), time_[ch].data() + B, B * sizeof(float));

        // Multiply-accumulate every partition against its delayed input.
        const int irc = ch < irCh_ ? ch : 0;
        float* __restrict ar = accRe_.data();
        float* __restrict ai = accIm_.data();
        std::fill(accRe_.begin(), accRe_.end(), 0.0f);
        std::fill(accIm_.begin(), accIm_.end(), 0.0f);
        int slot = head_;
        for (int p = 0; p < P; ++p) {
            const float* __restrict xr = &fdlRe_[ch][static_cast<size_t>(slot) * bins];
            const float* __restrict xi = &fdlIm_[ch][static_cast<size_t>(slot) * bins];
            const float* __restrict hr = &irRe_[irc][static_cast<size_t>(p) * bins];
            const float* __restrict hi = &irIm_[irc][static_cast<size_t>(p) * bins];
            for (int k = 0; k < bins; ++k) {
                ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
                ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
            }
            slot = (slot == 0) ? P - 1 : slot - 1;
        }

        // Inverse via the conjugate trick on the Hermitian-completed spectrum.
        for (int k = 0; k < bins; ++k) { workRe_[k] = ar[k]; workIm_[k] = -ai[k]; }
        for (int k = bins; k < N; ++k) { workRe_[k] = ar[N - k]; workIm_[k] = ai[N - k]; }
        fft(workRe_.data(), workIm_.data());
        float* out = outBlk_[ch].data();
        for (int k = 0; k < B; ++k) out[k] = workRe_[B + k] * invN;
    }
    head_ = (head_ + 1 == P) ? 0 : head_ + 1;
}

void PartitionedConvolver::processWet(const float* inL, const float* inR,
                                      float* outL, float* outR, int n)
{
    float* curL = time_[0].data() + B_;
    float* curR = time_[1].data() + B_;
    for (int i = 0; i < n; ++i) {
        curL[pos_] = inL[i];
        curR[pos_] = inR[i];
        outL[i] = outBlk_[0][pos_];
        outR[i] = outBlk_[1][pos_];
        if (++pos_ == B_) { processBlock(); pos_ = 0; }
    }
}

void PartitionedConvolver::processInterleaved(float* buf, int n, ParamRamp& mix)
{
    float* curL = time_[0].data() + B_;
    float* curR = time_[1].data() + B_;
    for (int i = 0; i < n; ++i) {
        const float dl = buf[2 * i], dr = buf[2 * i + 1];
        curL[pos_] = dl;
        curR[pos_] = dr;
        const float m = mix.next();
        buf[2 * i]     = dl + m * (outBlk_[0][pos_] - dl);
        buf[2 * i + 1] = dr + m * (outBlk_[1][pos_] - dr);
        if (++pos_ == B_) { processBlock(); pos_ = 0; }
    }
}

} // namespace broaudio
