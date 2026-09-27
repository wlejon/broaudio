#include "broaudio/spatial/spatial_chain.h"
#include "broaudio/synth/oscillator.h"
#include "broaudio/synth/synth_graph.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace broaudio {

bool chainSource(const VoiceChainParams& p, float* ch0, int n, bool compiled)
{
    const int from = std::clamp(p.sourceFrom, 0, n);
    const int to = std::clamp(p.sourceTo, from, n);
    if (from > 0) std::memset(ch0, 0, sizeof(float) * static_cast<size_t>(from));
    if (to < n) std::memset(ch0 + to, 0, sizeof(float) * static_cast<size_t>(n - to));
    if (!p.source || to == from) return true;
    return p.source->render(ch0 + from, to - from, compiled);
}

void chainAir(VoiceChainState& s, const float* poleTarget, const float* mixTarget,
              float* const* ch, int nch, int n)
{
    if (!s.airPrimed) {
        // First block: start settled at the target (a one-shot starts with
        // its distance's colour rather than sweeping into it).
        std::memcpy(s.airPole, poleTarget, sizeof(s.airPole));
        std::memcpy(s.airMix, mixTarget, sizeof(s.airMix));
        std::memset(s.airZ, 0, sizeof(s.airZ));
        std::memset(s.airX, 0, sizeof(s.airX));
        s.airPrimed = true;
    }
    float dp[kAirSections], dm[kAirSections];
    const float inv = n > 0 ? 1.0f / static_cast<float>(n) : 0.0f;
    for (int k = 0; k < kAirSections; ++k) {
        dp[k] = (poleTarget[k] - s.airPole[k]) * inv;
        dm[k] = (mixTarget[k] - s.airMix[k]) * inv;
    }

    for (int c = 0; c < nch; ++c) {
        float* x = ch[c];
        float z[kAirSections], xp[kAirSections], p[kAirSections], m[kAirSections];
        std::memcpy(z, s.airZ[c], sizeof(z));
        std::memcpy(xp, s.airX[c], sizeof(xp));
        std::memcpy(p, s.airPole, sizeof(p));
        std::memcpy(m, s.airMix, sizeof(m));
        for (int i = 0; i < n; ++i) {
            float v = x[i];
            for (int k = 0; k < kAirSections; ++k) {
                p[k] += dp[k];
                m[k] += dm[k];
                // Bilinear one-pole lowpass (zero at Nyquist), blended.
                z[k] = 0.5f * (1.0f - p[k]) * (v + xp[k]) + p[k] * z[k];
                xp[k] = v;
                v += m[k] * (z[k] - v);
            }
            x[i] = v;
        }
        std::memcpy(s.airZ[c], z, sizeof(z));
        std::memcpy(s.airX[c], xp, sizeof(xp));
    }
    std::memcpy(s.airPole, poleTarget, sizeof(s.airPole));
    std::memcpy(s.airMix, mixTarget, sizeof(s.airMix));
}

float chainDelay(VoiceChainState& s, PropagationDelayBuffer& buf, float target,
                 float maxDelay, float smooth, float* const* ch, int nch, int n)
{
    const int mask = buf.mask;
    const float lo = 2.0f;   // the 4-point interpolator reads one frame ahead
    const float hi = std::max(lo, std::min(maxDelay, static_cast<float>(buf.capacity - 4)));
    target = std::clamp(target, lo, hi);

    if (s.delayBufSeen != &buf) {
        s.delayBufSeen = &buf;
        s.writePos = 0;
        s.delayPrimed = false;
    }
    if (!s.delayPrimed) {
        // Snap: a one-shot's onset lands exactly at distance / c.
        s.delayS1 = s.delayS2 = s.delayOut = target;
        s.delayPrimed = true;
    }

    const int nc = std::min(nch, buf.channels);
    float* line[2] = {buf.channel(0), buf.channel(nc > 1 ? 1 : 0)};
    float s1 = s.delayS1, s2 = s.delayS2, d = s.delayOut;
    int w = s.writePos;
    for (int i = 0; i < n; ++i) {
        // Two cascaded one-poles: the delay (hence the read cursor's speed,
        // hence the Doppler pitch) moves continuously as the target steps at
        // the caller's frame rate. The per-sample change is clamped to
        // [-1, 0.5], i.e. a pitch ratio in [0.5, 2] like computeDopplerRatio.
        s1 += smooth * (target - s1);
        s2 += smooth * (s1 - s2);
        d += std::clamp(s2 - d, -1.0f, 0.5f);
        d = std::clamp(d, lo, hi);

        const float rp = static_cast<float>(w) - d;
        const float fl = std::floor(rp);
        const float t = rp - fl;
        const int i0 = static_cast<int>(fl);
        for (int c = 0; c < nc; ++c) {
            float* L = line[c];
            L[w] = ch[c][i];
            const float xm1 = L[(i0 - 1) & mask];
            const float x0 = L[i0 & mask];
            const float x1 = L[(i0 + 1) & mask];
            const float x2 = L[(i0 + 2) & mask];
            // 4-point, 3rd-order Hermite.
            const float c1 = 0.5f * (x1 - xm1);
            const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            ch[c][i] = ((c3 * t + c2) * t + c1) * t + x0;
        }
        w = (w + 1) & mask;
    }
    s.delayS1 = s1;
    s.delayS2 = s2;
    s.delayOut = d;
    s.writePos = w;
    return d;
}

void chainGainPan(VoiceChainState& s, float distanceGain, float pan,
                  const float* const* ch, int nch, float* L, float* R, int n)
{
    s.distanceGain.follow(distanceGain);
    s.pan.follow(pan);
    for (int i = 0; i < n; ++i) {
        const float g = s.gain.next() * s.distanceGain.next();
        const float p = s.pan.next();
        if (!s.panValid || p != s.panCached) {
            panGains(p, s.panL, s.panR);
            s.panCached = p;
            s.panValid = true;
        }
        if (nch == 2) {
            // Pan acts as balance: panL/panR crossfade the stereo image.
            const float sl = ch[0][i] * g, sr = ch[1][i] * g;
            L[i] = sl * s.panL + sr * (1.0f - s.panR);
            R[i] = sr * s.panR + sl * (1.0f - s.panL);
        } else {
            const float v = ch[0][i] * g;
            L[i] = v * s.panL;
            R[i] = v * s.panR;
        }
    }
}

void chainHead(VoiceChainState& s, const HeadParams& hp, float* L, float* R, int n)
{
    for (int i = 0; i < n; ++i) s.head.process(L[i], R[i], hp);
}

void chainTaps(VoiceChainState& s, const float* L, const float* R,
               float* bus, float* send, int n)
{
    for (int i = 0; i < n; ++i) {
        bus[2 * i] += L[i];
        bus[2 * i + 1] += R[i];
    }
    if (send) {
        for (int i = 0; i < n; ++i) {
            const float a = s.send.next();
            send[2 * i] += L[i] * a;
            send[2 * i + 1] += R[i] * a;
        }
    }
}

void runVoiceChain(const VoiceChainParams& p, VoiceChainState& s,
                   float* const* ch, float* L, float* R, int n)
{
    if (n <= 0) return;
    const int nch = p.channels == 2 ? 2 : 1;
    if (p.stages & kStageSource)
        chainSource(p, ch[0], n, false);
    if (p.stages & kStageAir)
        chainAir(s, p.airPole, p.airMix, ch, nch, n);
    if ((p.stages & kStageDelay) && p.delayBuf)
        chainDelay(s, *p.delayBuf, p.delayTarget, p.delayMax, p.delaySmooth, ch, nch, n);
    chainGainPan(s, p.distanceGain, p.pan, ch, nch, L, R, n);
    if (p.stages & kStageHead)
        chainHead(s, p.head, L, R, n);
    chainTaps(s, L, R, p.bus, (p.stages & kStageSend) ? p.send : nullptr, n);
}

} // namespace broaudio
