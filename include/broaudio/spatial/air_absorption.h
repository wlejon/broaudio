#pragma once

#include <memory>

namespace broaudio {

// ISO 9613-1:1993 pure-tone atmospheric absorption coefficient in dB per
// metre at `freqHz`, for air at `temperatureC` and `relativeHumidityPct`
// (0-100) and `pressureKPa` (101.325 = sea level). Classical + rotational
// absorption plus the oxygen and nitrogen vibrational relaxation terms, so
// the shape is right: ~f² through the audible band, highs falling far faster
// than lows over hundreds of metres (20 °C / 50 %: 4.7 dB/km at 1 kHz,
// 105 dB/km at 8 kHz).
double isoAirAbsorptionDbPerMetre(double freqHz, double temperatureC,
                                  double relativeHumidityPct,
                                  double pressureKPa = 101.325);

// The spatializer's air filter: a cascade of kAirSections first-order
// sections, section k being a bilinear one-pole lowpass (zero at Nyquist)
// blended with its input:
//
//   lp_k = b_k (x + x_prev) + p_k lp_k,   b_k = (1 - p_k) / 2
//   y    = x + m_k (lp_k - x)
//
// i.e. a high shelf with floor (1 - m_k) that becomes a plain lowpass as
// m_k -> 1. Per voice the parameters are just a pole p_k in (-1, 1) and a mix
// m_k in [0, 1] per section; any linear interpolation of them is still a
// stable filter, so the chain moves them per sample as the source moves (and
// a JIT kernel takes them as plain params).
//
// The poles and mixes that best match ISO 9613-1 attenuation over a distance
// are fitted off the audio thread per (sample rate, temperature, humidity): a
// weighted Levenberg-Marquardt fit, in dB, of all 14 parameters against the
// exact digital response at 1/3-octave points from 100 Hz to
// min(20 kHz, 0.45 fs). Targets are capped at 80 dB and errors weighted by
// 1 / max(1 dB, 15 %), so the audible part of the curve is what gets matched.
// A fit per point of a log-spaced grid from 1 m to 20 km, warm-started from
// its neighbour; the audio thread only interpolates the grid.
constexpr int kAirSections = 7;

struct AirFilterTable {
    static constexpr int kPoints = 64;
    static constexpr float kMinMetres = 1.0f;
    static constexpr float kMaxMetres = 20000.0f;

    int sampleRate = 44100;
    float temperatureC = 20.0f;
    float humidityPct = 50.0f;
    float pole[kPoints][kAirSections] = {};      // p_k per grid distance
    float depthDb[kPoints][kAirSections] = {};   // shelf depth, m = 1 - 10^(-depth/20)

    // Section poles and mixes for an absorbing path of `metres` (already
    // scaled by the absorption strength). Below 1 m the depths scale linearly
    // to 0; beyond 20 km they hold the 20 km fit. RT-safe (a few exp calls).
    void lookup(float metres, float* outPole, float* outMix) const;

    // Model attenuation (positive dB) at `freqHz` for the given parameters:
    // the exact magnitude of the digital cascade. For tests and diagnostics.
    float responseDb(const float* pole, const float* mix, float freqHz) const;
};

// Fit a table. Cost is tens of milliseconds; call off the audio thread.
std::shared_ptr<const AirFilterTable> buildAirFilterTable(int sampleRate,
                                                          float temperatureC,
                                                          float humidityPct);

} // namespace broaudio
