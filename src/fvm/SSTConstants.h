#pragma once

#include <cstddef>

namespace fvm {

struct SSTConstants {
    double sigma_k1 = 1.176;
    double sigma_w1 = 2.0;
    double beta1    = 0.075;
    double alpha1   = 0.5532;

    double sigma_k2 = 1.0;
    double sigma_w2 = 1.168;
    double beta2    = 0.0828;
    double alpha2   = 0.4403;

    double beta_star = 0.09;
    double a1        = 0.31;
    double kappa     = 0.41;
};

constexpr double nonOrthCorrectionFactor = 1.0;
constexpr double minFaceDist = 1e-15;
constexpr double minDivisor = 1e-30;
constexpr double wallDistFactor = 500.0;
constexpr double tvdRatioClamp = 100.0;
constexpr double tvdCosThetaLow = 0.5;
constexpr double tvdCosThetaHigh = 0.8;
constexpr double velocityUnderRelax = 0.7;
constexpr double pressureUnderRelax = 0.1;

constexpr double simplexMinDenomRatio = 0.3;
constexpr double sstProductionLimitFactor = 10.0;
constexpr double nutUnderRelax = 0.8;
constexpr double divergenceStallThreshold = 0.95;
constexpr double divergenceAlphaPReduction = 0.5;
constexpr double divergenceAbortAlphaPMin = 0.01;
constexpr std::size_t residualHistoryWindow = 20;
constexpr std::size_t divergenceAbortCount = 20;

} // namespace fvm
