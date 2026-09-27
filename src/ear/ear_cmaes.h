#pragma once

// CMA-ES in the unit cube, for ear::fit (include/broaudio/ear/fit.h).
// Internal. Hansen's (mu/mu_w, lambda)-CMA-ES ("The CMA Evolution Strategy:
// A Tutorial", 2016): weighted recombination, cumulative step-size
// adaptation, rank-one + rank-mu covariance updates, the eigendecomposition
// by cyclic Jacobi. Samples are reflected into [0, 1]^n and the update uses
// the reflected points. Deterministic: all randomness comes from the
// generator passed in.

#include <cstdint>
#include <vector>

namespace broaudio::ear::detail {

// xoshiro256** seeded through splitmix64, plus a Box-Muller normal.
class FitRng {
public:
    explicit FitRng(uint64_t seed);
    uint64_t next();
    double uniform();  // [0, 1)
    double normal();

private:
    uint64_t s_[4];
    bool haveSpare_ = false;
    double spare_ = 0;
};

class Cmaes {
public:
    // `start` in the unit cube; `lambda` <= 0 picks 2 (4 + floor(3 ln n)),
    // at least 10.
    Cmaes(const std::vector<double>& start, double sigma, int lambda);

    int dimension() const { return n_; }
    int lambda() const { return lambda_; }
    double sigma() const { return sigma_; }
    const std::vector<double>& mean() const { return m_; }

    // The next generation's lambda candidates, each in [0, 1]^n.
    std::vector<std::vector<double>> ask(FitRng& rng);
    // Their fitness, in ask() order (lower is better; NaN counts as +inf).
    void tell(const std::vector<std::vector<double>>& xs, const std::vector<double>& fitness);

    // Converged: the step in every coordinate is below 1e-5, the best
    // fitness of the last 10 + 30 n / lambda generations spans less than
    // 1e-10, the run's best improved by under 5 % over the last
    // 20 + 50 n / lambda generations, or the covariance is ill-conditioned.
    bool converged() const;

private:
    void updateEigen();

    int n_ = 0;
    int lambda_ = 0, mu_ = 0;
    std::vector<double> w_;
    double mueff_ = 0, cs_ = 0, ds_ = 0, cc_ = 0, c1_ = 0, cmu_ = 0, chiN_ = 0;
    std::vector<double> m_, ps_, pc_;
    std::vector<double> C_, B_, D_;  // n x n row-major; D = sqrt(eigenvalues)
    double sigma_ = 0;
    int gen_ = 0;
    std::vector<double> bestHistory_;  // best fitness per generation
    std::vector<double> runBest_;      // best so far, per generation
    bool illConditioned_ = false;
};

} // namespace broaudio::ear::detail
