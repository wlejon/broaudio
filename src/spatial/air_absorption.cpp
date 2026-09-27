#include "broaudio/spatial/air_absorption.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace broaudio {

double isoAirAbsorptionDbPerMetre(double f, double temperatureC,
                                  double relativeHumidityPct, double pressureKPa)
{
    constexpr double pr = 101.325;   // reference pressure, kPa
    constexpr double T0 = 293.15;    // reference temperature, K
    constexpr double T01 = 273.16;   // triple-point isotherm, K
    const double pa = pressureKPa > 0.0 ? pressureKPa : pr;
    const double T = temperatureC + 273.15;
    const double rh = std::clamp(relativeHumidityPct, 0.0, 100.0);

    // Molar concentration of water vapour, % (ISO 9613-1 Annex B).
    const double C = -6.8346 * std::pow(T01 / T, 1.261) + 4.6151;
    const double psatOverPr = std::pow(10.0, C);
    const double h = rh * psatOverPr / (pa / pr);

    // Relaxation frequencies of oxygen and nitrogen, Hz.
    const double frO = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = (pa / pr) * std::pow(T / T0, -0.5)
                     * (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(T / T0, -1.0 / 3.0) - 1.0)));

    const double f2 = f * f;
    const double classical = 1.84e-11 * (pr / pa) * std::sqrt(T / T0);
    const double relaxO = 0.01275 * std::exp(-2239.1 / T) / (frO + f2 / frO);
    const double relaxN = 0.1068 * std::exp(-3352.0 / T) / (frN + f2 / frN);
    return 8.686 * f2 * (classical + std::pow(T / T0, -2.5) * (relaxO + relaxN));
}

namespace {

constexpr double kPiD = 3.141592653589793;
constexpr double kLn10Over20 = 2.302585092994046 / 20.0;
constexpr int P = 2 * kAirSections;   // fitted parameters: depths, then log2 corners

double mixFromDepth(double g) { return 1.0 - std::exp(-g * kLn10Over20); }

double poleFromCorner(double hz, double fs) {
    const double K = std::tan(kPiD * hz / fs);
    return (1.0 - K) / (1.0 + K);
}

// Attenuation in dB (positive) of one section (pole p, mix m) at a frequency
// whose e^{-jw} is (c, -s).
double sectionDb(double p, double m, double c, double s) {
    // H = b (1 + e^{-jw}) / (1 - p e^{-jw}),  b = (1 - p) / 2
    const double b = 0.5 * (1.0 - p);
    const double nr = b * (1.0 + c), ni = -b * s;
    const double dr = 1.0 - p * c, di = p * s;
    const double den = dr * dr + di * di;
    const double hr = (nr * dr + ni * di) / den;
    const double hi = (ni * dr - nr * di) / den;
    const double re = (1.0 - m) + m * hr;
    const double im = m * hi;
    return -10.0 * std::log10(std::max(re * re + im * im, 1e-30));
}

struct Fitter {
    double fs = 44100.0;
    double uMin = 0.0, uMax = 0.0;   // log2 corner bounds
    std::vector<double> cosw, sinw, alpha;

    double model(const double* x, size_t j) const {
        double s = 0.0;
        for (int k = 0; k < kAirSections; ++k) {
            const double p = poleFromCorner(std::exp2(x[kAirSections + k]), fs);
            s += sectionDb(p, mixFromDepth(x[k]), cosw[j], sinw[j]);
        }
        return s;
    }

    void residuals(const double* x, double d, std::vector<double>& r) const {
        r.resize(cosw.size());
        for (size_t j = 0; j < cosw.size(); ++j) {
            const double target = std::min(80.0, d * alpha[j]);
            const double w = 1.0 / std::max(1.0, 0.15 * target);
            r[j] = (std::min(model(x, j), 95.0) - target) * w;
        }
    }

    static double cost(const std::vector<double>& r) {
        double c = 0.0;
        for (double v : r) c += v * v;
        return c;
    }

    void clampParams(double* x) const {
        for (int k = 0; k < kAirSections; ++k) {
            x[k] = std::clamp(x[k], 0.0, 120.0);
            x[kAirSections + k] = std::clamp(x[kAirSections + k], uMin, uMax);
        }
    }

    // Levenberg-Marquardt over depths (dB) and corners (octaves), bounded.
    void fit(double* x, double d) const {
        std::vector<double> r, r2, rj;
        residuals(x, d, r);
        double c = cost(r);
        double lambda = 1e-2;
        const size_t M = cosw.size();
        std::vector<double> J(M * P);
        for (int iter = 0; iter < 60; ++iter) {
            for (int k = 0; k < P; ++k) {
                double xk[P];
                std::copy(x, x + P, xk);
                const double h = k < kAirSections ? 0.01 : 0.002;
                xk[k] += h;
                residuals(xk, d, rj);
                for (size_t j = 0; j < M; ++j) J[j * P + k] = (rj[j] - r[j]) / h;
            }
            double A[P][P + 1];
            for (int a = 0; a < P; ++a) {
                for (int b = 0; b < P; ++b) {
                    double s = 0.0;
                    for (size_t j = 0; j < M; ++j) s += J[j * P + a] * J[j * P + b];
                    A[a][b] = s;
                }
                double s = 0.0;
                for (size_t j = 0; j < M; ++j) s += J[j * P + a] * r[j];
                A[a][P] = -s;
            }
            bool improved = false;
            for (int tries = 0; tries < 10 && !improved; ++tries) {
                double S[P][P + 1];
                for (int a = 0; a < P; ++a) {
                    for (int b = 0; b <= P; ++b) S[a][b] = A[a][b];
                    S[a][a] += lambda * (A[a][a] + 1e-6);
                }
                // Gauss-Jordan with partial pivoting.
                for (int col = 0; col < P; ++col) {
                    int piv = col;
                    for (int row = col + 1; row < P; ++row)
                        if (std::fabs(S[row][col]) > std::fabs(S[piv][col])) piv = row;
                    for (int b = 0; b <= P; ++b) std::swap(S[col][b], S[piv][b]);
                    const double diag = S[col][col];
                    if (std::fabs(diag) < 1e-18) continue;
                    for (int row = 0; row < P; ++row) {
                        if (row == col) continue;
                        const double f = S[row][col] / diag;
                        for (int b = col; b <= P; ++b) S[row][b] -= f * S[col][b];
                    }
                }
                double trial[P];
                for (int k = 0; k < P; ++k)
                    trial[k] = x[k] + (std::fabs(S[k][k]) > 1e-18 ? S[k][P] / S[k][k] : 0.0);
                clampParams(trial);
                residuals(trial, d, r2);
                const double c2 = cost(r2);
                if (c2 < c) {
                    std::copy(trial, trial + P, x);
                    const double gain = c - c2;
                    r.swap(r2);
                    c = c2;
                    lambda = std::max(lambda / 3.0, 1e-7);
                    improved = true;
                    if (gain < 1e-10 * (1.0 + c)) return;
                } else {
                    lambda *= 4.0;
                }
            }
            if (!improved) return;
        }
    }
};

} // namespace

void AirFilterTable::lookup(float metres, float* outPole, float* outMix) const
{
    const float* plo;
    const float* phi;
    const float* glo;
    const float* ghi;
    float t = 0.0f;
    float scale = 1.0f;
    if (!(metres > kMinMetres)) {
        plo = phi = pole[0];
        glo = ghi = depthDb[0];
        scale = metres > 0.0f ? metres / kMinMetres : 0.0f;
    } else {
        const float pos = std::log(metres / kMinMetres) / std::log(kMaxMetres / kMinMetres)
                        * static_cast<float>(kPoints - 1);
        const int i = std::min(static_cast<int>(pos), kPoints - 2);
        t = std::min(pos - static_cast<float>(i), 1.0f);
        plo = pole[i]; phi = pole[i + 1];
        glo = depthDb[i]; ghi = depthDb[i + 1];
    }
    for (int k = 0; k < kAirSections; ++k) {
        outPole[k] = plo[k] + t * (phi[k] - plo[k]);
        const float g = (glo[k] + t * (ghi[k] - glo[k])) * scale;
        outMix[k] = 1.0f - std::exp(-g * static_cast<float>(kLn10Over20));
    }
}

float AirFilterTable::responseDb(const float* p, const float* mix, float freqHz) const
{
    const double w = 2.0 * kPiD * freqHz / sampleRate;
    const double c = std::cos(w), s = std::sin(w);
    double sum = 0.0;
    for (int k = 0; k < kAirSections; ++k) sum += sectionDb(p[k], mix[k], c, s);
    return static_cast<float>(sum);
}

std::shared_ptr<const AirFilterTable> buildAirFilterTable(int sampleRate,
                                                          float temperatureC,
                                                          float humidityPct)
{
    auto table = std::make_shared<AirFilterTable>();
    table->sampleRate = sampleRate > 0 ? sampleRate : 44100;
    table->temperatureC = temperatureC;
    table->humidityPct = humidityPct;

    Fitter fitter;
    fitter.fs = table->sampleRate;
    fitter.uMin = std::log2(100.0);
    fitter.uMax = std::log2(0.49 * fitter.fs);
    const double fMax = std::min(20000.0, 0.45 * fitter.fs);
    for (int b = -10; b <= 13; ++b) {
        const double f = 1000.0 * std::pow(2.0, b / 3.0);
        if (f > fMax) break;
        const double w = 2.0 * kPiD * f / fitter.fs;
        fitter.cosw.push_back(std::cos(w));
        fitter.sinw.push_back(std::sin(w));
        fitter.alpha.push_back(isoAirAbsorptionDbPerMetre(f, temperatureC, humidityPct));
    }

    // Start: flat (depth 0) with corners spread over the band where air acts.
    double x[P] = {};
    const double startHz[kAirSections] = {1000., 2000., 4000., 6000., 8000., 12000., 16000.};
    for (int k = 0; k < kAirSections; ++k) x[kAirSections + k] = std::log2(startHz[k]);
    fitter.clampParams(x);

    for (int i = 0; i < AirFilterTable::kPoints; ++i) {
        const double d = AirFilterTable::kMinMetres
                       * std::pow(static_cast<double>(AirFilterTable::kMaxMetres / AirFilterTable::kMinMetres),
                                  static_cast<double>(i) / (AirFilterTable::kPoints - 1));
        fitter.fit(x, d);   // warm-started from the previous distance
        for (int k = 0; k < kAirSections; ++k) {
            table->depthDb[i][k] = static_cast<float>(x[k]);
            table->pole[i][k] = static_cast<float>(poleFromCorner(std::exp2(x[kAirSections + k]), fitter.fs));
        }
    }
    return table;
}

} // namespace broaudio
