// CMA-ES for ear::fit. See ear_cmaes.h.

#include "ear_cmaes.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace broaudio::ear::detail {

namespace {

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

// Reflect into [0, 1] (a mirror at each face, periodic beyond).
double reflect(double x) {
    if (x >= 0.0 && x <= 1.0) return x;
    if (!std::isfinite(x)) return 0.5;
    double t = std::fmod(std::fabs(x), 2.0);
    return t > 1.0 ? 2.0 - t : t;
}

// Eigendecomposition of the symmetric n x n matrix `a` (row-major, destroyed)
// by cyclic Jacobi rotations: eigenvalues into `evals`, eigenvectors into the
// columns of `v`.
void jacobiEigen(std::vector<double>& a, int n, std::vector<double>& evals, std::vector<double>& v) {
    v.assign(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) v[i * n + i] = 1.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0, diag = 0.0;
        for (int i = 0; i < n; ++i) {
            diag += a[i * n + i] * a[i * n + i];
            for (int j = i + 1; j < n; ++j) off += a[i * n + j] * a[i * n + j];
        }
        if (off <= 1e-30 * diag || off == 0.0) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[p * n + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double app = a[p * n + p], aqq = a[q * n + q];
                const double theta = (aqq - app) / (2.0 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = v[k * n + p], vkq = v[k * n + q];
                    v[k * n + p] = c * vkp - s * vkq;
                    v[k * n + q] = s * vkp + c * vkq;
                }
            }
        }
    }
    evals.resize(n);
    for (int i = 0; i < n; ++i) evals[i] = a[i * n + i];
}

} // namespace

FitRng::FitRng(uint64_t seed) {
    uint64_t x = seed;
    for (auto& s : s_) s = splitmix(x);
}

uint64_t FitRng::next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

double FitRng::uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

double FitRng::normal() {
    if (haveSpare_) {
        haveSpare_ = false;
        return spare_;
    }
    double u1 = uniform();
    while (u1 <= 1e-300) u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double a = 6.283185307179586 * u2;
    spare_ = r * std::sin(a);
    haveSpare_ = true;
    return r * std::cos(a);
}

Cmaes::Cmaes(const std::vector<double>& start, double sigma, int lambda) {
    n_ = static_cast<int>(start.size());
    const double n = n_;
    // Twice Hansen's default: sound objectives are rugged (a larger
    // population recovered eight pluck parameters far more reliably).
    lambda_ = lambda > 0 ? lambda : std::max(10, 2 * (4 + static_cast<int>(std::floor(3.0 * std::log(n)))));
    mu_ = lambda_ / 2;
    w_.resize(mu_);
    double sw = 0.0, sw2 = 0.0;
    for (int i = 0; i < mu_; ++i) {
        w_[i] = std::log(mu_ + 0.5) - std::log(i + 1.0);
        sw += w_[i];
    }
    for (double& w : w_) {
        w /= sw;
        sw2 += w * w;
    }
    mueff_ = 1.0 / sw2;
    cs_ = (mueff_ + 2.0) / (n + mueff_ + 5.0);
    ds_ = 1.0 + 2.0 * std::max(0.0, std::sqrt((mueff_ - 1.0) / (n + 1.0)) - 1.0) + cs_;
    cc_ = (4.0 + mueff_ / n) / (n + 4.0 + 2.0 * mueff_ / n);
    c1_ = 2.0 / ((n + 1.3) * (n + 1.3) + mueff_);
    cmu_ = std::min(1.0 - c1_, 2.0 * (mueff_ - 2.0 + 1.0 / mueff_) / ((n + 2.0) * (n + 2.0) + mueff_));
    chiN_ = std::sqrt(n) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));

    m_ = start;
    for (double& x : m_) x = std::clamp(x, 0.0, 1.0);
    ps_.assign(n_, 0.0);
    pc_.assign(n_, 0.0);
    C_.assign(static_cast<size_t>(n_) * n_, 0.0);
    B_.assign(static_cast<size_t>(n_) * n_, 0.0);
    D_.assign(n_, 1.0);
    for (int i = 0; i < n_; ++i) C_[i * n_ + i] = B_[i * n_ + i] = 1.0;
    sigma_ = sigma;
}

std::vector<std::vector<double>> Cmaes::ask(FitRng& rng) {
    std::vector<std::vector<double>> xs(lambda_, std::vector<double>(n_));
    std::vector<double> z(n_), bdz(n_);
    for (int k = 0; k < lambda_; ++k) {
        for (int i = 0; i < n_; ++i) z[i] = D_[i] * rng.normal();
        for (int i = 0; i < n_; ++i) {
            double s = 0.0;
            for (int j = 0; j < n_; ++j) s += B_[i * n_ + j] * z[j];
            bdz[i] = s;
        }
        for (int i = 0; i < n_; ++i) xs[k][i] = reflect(m_[i] + sigma_ * bdz[i]);
    }
    return xs;
}

void Cmaes::tell(const std::vector<std::vector<double>>& xs, const std::vector<double>& fitness) {
    const int lam = static_cast<int>(xs.size());
    std::vector<int> order(lam);
    std::iota(order.begin(), order.end(), 0);
    auto fit = [&](int i) { return std::isnan(fitness[i]) ? INFINITY : fitness[i]; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return fit(a) < fit(b); });
    bestHistory_.push_back(fit(order[0]));
    runBest_.push_back(runBest_.empty() ? bestHistory_.back() : std::min(runBest_.back(), bestHistory_.back()));

    const int mu = std::min(mu_, lam);
    // y_k = (x_k - m) / sigma for the mu best, and their weighted mean.
    std::vector<std::vector<double>> y(mu, std::vector<double>(n_));
    std::vector<double> yw(n_, 0.0);
    for (int k = 0; k < mu; ++k) {
        const auto& x = xs[order[k]];
        for (int i = 0; i < n_; ++i) {
            y[k][i] = (x[i] - m_[i]) / sigma_;
            yw[i] += w_[k] * y[k][i];
        }
    }
    for (int i = 0; i < n_; ++i) m_[i] += sigma_ * yw[i];

    // C^{-1/2} y_w = B D^{-1} B^T y_w.
    std::vector<double> t(n_, 0.0), cy(n_, 0.0);
    for (int j = 0; j < n_; ++j) {
        double s = 0.0;
        for (int i = 0; i < n_; ++i) s += B_[i * n_ + j] * yw[i];
        t[j] = s / D_[j];
    }
    for (int i = 0; i < n_; ++i) {
        double s = 0.0;
        for (int j = 0; j < n_; ++j) s += B_[i * n_ + j] * t[j];
        cy[i] = s;
    }
    const double csn = std::sqrt(cs_ * (2.0 - cs_) * mueff_);
    double psNorm = 0.0;
    for (int i = 0; i < n_; ++i) {
        ps_[i] = (1.0 - cs_) * ps_[i] + csn * cy[i];
        psNorm += ps_[i] * ps_[i];
    }
    psNorm = std::sqrt(psNorm);
    ++gen_;
    const double denom = std::sqrt(1.0 - std::pow(1.0 - cs_, 2.0 * gen_));
    const bool hsig = psNorm / denom < (1.4 + 2.0 / (n_ + 1.0)) * chiN_;
    const double ccn = std::sqrt(cc_ * (2.0 - cc_) * mueff_);
    for (int i = 0; i < n_; ++i) pc_[i] = (1.0 - cc_) * pc_[i] + (hsig ? ccn * yw[i] : 0.0);

    const double keep = 1.0 - c1_ - cmu_ + (hsig ? 0.0 : c1_ * cc_ * (2.0 - cc_));
    for (int i = 0; i < n_; ++i) {
        for (int j = 0; j <= i; ++j) {
            double rankMu = 0.0;
            for (int k = 0; k < mu; ++k) rankMu += w_[k] * y[k][i] * y[k][j];
            const double v = keep * C_[i * n_ + j] + c1_ * pc_[i] * pc_[j] + cmu_ * rankMu;
            C_[i * n_ + j] = C_[j * n_ + i] = v;
        }
    }
    sigma_ *= std::exp((cs_ / ds_) * (psNorm / chiN_ - 1.0));
    sigma_ = std::clamp(sigma_, 1e-12, 1.0);
    updateEigen();
}

void Cmaes::updateEigen() {
    std::vector<double> a = C_, evals;
    jacobiEigen(a, n_, evals, B_);
    double lo = INFINITY, hi = 0.0;
    for (int i = 0; i < n_; ++i) {
        const double e = std::max(evals[i], 1e-20);
        D_[i] = std::sqrt(e);
        lo = std::min(lo, e);
        hi = std::max(hi, e);
    }
    illConditioned_ = hi > 1e14 * lo;
}

bool Cmaes::converged() const {
    if (illConditioned_) return true;
    bool small = true;
    for (int i = 0; i < n_; ++i) {
        if (sigma_ * std::sqrt(C_[i * n_ + i]) > 1e-5) small = false;
    }
    if (small) return true;
    const size_t span = static_cast<size_t>(10 + std::ceil(30.0 * n_ / lambda_));
    if (bestHistory_.size() >= span) {
        double lo = INFINITY, hi = -INFINITY;
        for (size_t i = bestHistory_.size() - span; i < bestHistory_.size(); ++i) {
            lo = std::min(lo, bestHistory_[i]);
            hi = std::max(hi, bestHistory_[i]);
        }
        if (std::isfinite(hi) && hi - lo <= 1e-10 * std::max(1.0, std::fabs(lo))) return true;
    }
    // Stagnation: the run's best improved by less than 5 % over the last
    // 20 + 50 n / lambda generations (a rough objective lets sigma hover
    // without ever meeting tolX).
    const size_t slow = static_cast<size_t>(20 + std::ceil(50.0 * n_ / lambda_));
    if (runBest_.size() > slow) {
        const double now = runBest_.back(), then = runBest_[runBest_.size() - 1 - slow];
        if (std::isfinite(then) && now > 0.95 * then) return true;
    }
    return false;
}

} // namespace broaudio::ear::detail
