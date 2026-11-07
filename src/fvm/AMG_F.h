#pragma once
// Own F-cycle hierarchy — vendor/amgcl is NOT modified.
// Uses amgcl building blocks (coarsening, relaxation, backend) directly
// to implement the true F recursion that amgcl::amg::cycle lacks.
#include <amgcl/coarsening/smoothed_aggregation.hpp>
#include <amgcl/relaxation/gauss_seidel.hpp>
#include <amgcl/backend/builtin.hpp>
#include <amgcl/backend/interface.hpp>
#include <Eigen/Sparse>
#include <memory>
#include <vector>
#include <list>
#include <algorithm>
#include <cmath>
#include "fvm/AMG.h"

namespace fvm {

class FAMGHierarchy {
public:
    explicit FAMGHierarchy(AMGHierarchy::Config cfg) : cfg_(cfg) {}

    bool build(const SparseMatrix& A);
    bool applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const;

    bool isBuilt() const { return built_; }
    int fineSize_() const { return n_; }
    const std::vector<std::pair<size_t,double>>& flexVisits() const { return lastFlexVisit_; }
    int numLevels() const { return (int)levels_.size(); }
    std::vector<int> levelSizes() const {
        std::vector<int> s;
        for (auto& l : levels_) s.push_back((int)::amgcl::backend::rows(*l.A));
        return s;
    }

private:
    using Backend = ::amgcl::backend::builtin<double>;
    using BuildMat = typename Backend::matrix;
    using Coars = ::amgcl::coarsening::smoothed_aggregation<Backend>;
    using Relax = ::amgcl::relaxation::gauss_seidel<Backend>;

    struct Level {
        std::shared_ptr<typename Backend::vector> f, u, t;
        std::shared_ptr<BuildMat> A, P, R;
        std::shared_ptr<Relax> relax;
        std::shared_ptr<typename Backend::direct_solver> solve;
    };

    void buildLevels_(std::shared_ptr<BuildMat> A);
    void f_cycle(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    void f_cycle_coarse(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    void v_cycle_coarse(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    // Flexible: residual-driven level visits.
    double residualNorm_(size_t idx, const ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    void flex_cycle(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    void flex_cycle_coarse(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const;
    mutable std::vector<std::pair<size_t,double>> lastFlexVisit_;

    AMGHierarchy::Config cfg_;
    std::list<Level> levels_;
    std::vector<double> scale_;
    bool built_ = false;
    int n_ = 0;
};

inline bool FAMGHierarchy::build(const SparseMatrix& A) {
    const int n = (int)A.rows();
    n_ = n;
    scale_.clear();
    Eigen::SparseMatrix<double, Eigen::RowMajor> AR = A;
    AR.makeCompressed();
    // Symmetric diagonal scaling (same as AMGHierarchy)
    if (cfg_.scaling == AMGHierarchy::Config::ProportionalScaling::Automatic) {
        scale_.assign(n, 1.0);
        for (int i = 0; i < n; ++i) {
            double d = A.coeff(i, i);
            if (std::abs(d) > 0) scale_[i] = 1.0 / std::sqrt(std::abs(d));
        }
        const int* outer = AR.outerIndexPtr();
        const int* inner = AR.innerIndexPtr();
        double* vals = AR.valuePtr();
        for (int c = 0; c < n; ++c)
            for (int k = outer[c]; k < outer[c+1]; ++k) {
                int r = inner[k];
                vals[k] *= scale_[r] * scale_[c];
            }
    }
    levels_.clear();
    built_ = false;
    try {
        auto Amat = std::make_shared<BuildMat>(AR);
        ::amgcl::backend::sort_rows(*Amat);
        buildLevels_(Amat);
        built_ = !levels_.empty();
    } catch (...) {
        built_ = false;
    }
    return built_;
}

inline void FAMGHierarchy::buildLevels_(std::shared_ptr<BuildMat> A) {
    // Coarsening params mirroring AMG.cpp: eps_strong = 0.5*threshold
    Coars::params cprm;
    cprm.aggr.eps_strong = (float)(0.5 * cfg_.strengthThreshold);
    // Group: scalar back to 1 (Auto padding TODO, causes segfault with block>1).
    {
        int nrows = (int)::amgcl::backend::rows(*A);
        if (cfg_.groupSizeControl == AMGHierarchy::Config::GroupSizeControl::Manual &&
            cfg_.blockSize > 1 && nrows % cfg_.blockSize == 0)
            cprm.aggr.block_size = static_cast<unsigned>(cfg_.blockSize);
    }
    Coars C(cprm);
    typename Backend::params bprm{};

    bool direct_coarse_solve = true;
    auto cur = A;
    while ((int)::amgcl::backend::rows(*cur) > cfg_.coarseEnough) {
        Level lv;
        lv.f = Backend::create_vector(::amgcl::backend::rows(*cur), bprm);
        lv.u = Backend::create_vector(::amgcl::backend::rows(*cur), bprm);
        lv.t = Backend::create_vector(::amgcl::backend::rows(*cur), bprm);
        lv.A = Backend::copy_matrix(cur, bprm);
        Relax::params rprm;
        rprm.serial = true; // avoid OpenMP barrier slowdown (see AMG.cpp)
        lv.relax = std::make_shared<Relax>(*cur, rprm, bprm);
        // transfer operators
        std::shared_ptr<BuildMat> P, R;
        try {
            std::tie(P, R) = C.transfer_operators(*cur);
        } catch (...) {
            direct_coarse_solve = false;
            levels_.push_back(std::move(lv));
            break;
        }
        ::amgcl::backend::sort_rows(*P);
        ::amgcl::backend::sort_rows(*R);
        lv.P = Backend::copy_matrix(P, bprm);
        lv.R = Backend::copy_matrix(R, bprm);
        // coarse operator
        auto nextA = C.coarse_operator(*cur, *P, *R);
        ::amgcl::backend::sort_rows(*nextA);
        levels_.push_back(std::move(lv));
        if ((int)levels_.size() >= cfg_.maxLevels) { direct_coarse_solve = false; break; }
        cur = std::make_shared<BuildMat>(std::move(*nextA));
    }
    if (!cur || (int)::amgcl::backend::rows(*cur) > cfg_.coarseEnough)
        direct_coarse_solve = false;
    if (direct_coarse_solve) {
        Level cl;
        cl.f = Backend::create_vector(::amgcl::backend::rows(*cur), bprm);
        cl.u = Backend::create_vector(::amgcl::backend::rows(*cur), bprm);
        cl.A = Backend::copy_matrix(cur, bprm);
        cl.solve = Backend::create_solver(cur, bprm);
        levels_.push_back(std::move(cl));
    }
}

inline bool FAMGHierarchy::applyCycle(const Eigen::VectorXd& b, Eigen::VectorXd& x) const {
    if (!cfg_.flexStruct.empty() && cfg_.flexStruct[0] == AMGHierarchy::Config::FlexMove::Terminate)
        return true;
    if (!built_) return false;
    const bool hasScale = (int)scale_.size() == (int)b.size();
    ::amgcl::backend::numa_vector<double> bn(b.size()), xn(x.size());
    for (int i = 0; i < (int)b.size(); ++i) bn[i] = hasScale ? b(i) * scale_[i] : b(i);
    for (int i = 0; i < (int)x.size(); ++i) xn[i] = hasScale ? x(i) / scale_[i] : x(i);
    if (cfg_.cycleType == AMGHierarchy::Config::CycleType::Flexible)
        flex_cycle(0, bn, xn);
    else
        f_cycle(0, bn, xn);
    for (int i = 0; i < (int)x.size(); ++i) x(i) = hasScale ? xn[i] * scale_[i] : xn[i];
    return x.allFinite();
}

inline void FAMGHierarchy::f_cycle(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    auto it = levels_.begin();
    std::advance(it, (ptrdiff_t)idx);
    auto& lv = *it;
    if (idx + 1 >= levels_.size()) {
        if (lv.solve) (*lv.solve)(rhs, x);
        return;
    }
    auto itNext = std::next(it);
    for (int i = 0; i < cfg_.preSweeps; ++i) lv.relax->apply_pre(*lv.A, rhs, x, *lv.t);
    // First correction: F
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
    ::amgcl::backend::clear(*itNext->u);
    f_cycle_coarse(idx + 1, *itNext->f, *itNext->u);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.P, *itNext->u, ::amgcl::math::identity<double>(), x);
    for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
    // Second correction: V
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
    ::amgcl::backend::clear(*itNext->u);
    v_cycle_coarse(idx + 1, *itNext->f, *itNext->u);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.P, *itNext->u, ::amgcl::math::identity<double>(), x);
    for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
}

inline void FAMGHierarchy::f_cycle_coarse(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    auto it = levels_.begin();
    std::advance(it, (ptrdiff_t)idx);
    auto& lv = *it;
    if (idx + 1 >= levels_.size()) {
        if (lv.solve) (*lv.solve)(rhs, x);
        return;
    }
    auto itNext = std::next(it);
    for (int i = 0; i < cfg_.preSweeps; ++i) lv.relax->apply_pre(*lv.A, rhs, x, *lv.t);
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
    ::amgcl::backend::clear(*itNext->u);
    f_cycle_coarse(idx + 1, *itNext->f, *itNext->u);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.P, *itNext->u, ::amgcl::math::identity<double>(), x);
    for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
    ::amgcl::backend::clear(*itNext->u);
    v_cycle_coarse(idx + 1, *itNext->f, *itNext->u);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.P, *itNext->u, ::amgcl::math::identity<double>(), x);
    for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
}

inline void FAMGHierarchy::v_cycle_coarse(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    auto it = levels_.begin();
    std::advance(it, (ptrdiff_t)idx);
    auto& lv = *it;
    if (idx + 1 >= levels_.size()) {
        if (lv.solve) (*lv.solve)(rhs, x);
        else {
            for (int i = 0; i < cfg_.preSweeps; ++i) lv.relax->apply_pre(*lv.A, rhs, x, *lv.t);
            for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
        }
        return;
    }
    auto itNext = std::next(it);
    for (int i = 0; i < cfg_.preSweeps; ++i) lv.relax->apply_pre(*lv.A, rhs, x, *lv.t);
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
    ::amgcl::backend::clear(*itNext->u);
    v_cycle_coarse(idx + 1, *itNext->f, *itNext->u);
    ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.P, *itNext->u, ::amgcl::math::identity<double>(), x);
    for (int i = 0; i < cfg_.postSweeps; ++i) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
}

inline double FAMGHierarchy::residualNorm_(size_t idx, const ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    auto it = levels_.begin();
    std::advance(it, (ptrdiff_t)idx);
    auto& lv = *it;
    ::amgcl::backend::residual(rhs, *lv.A, x, *lv.t);
    double s = 0.0;
    for (size_t i = 0; i < lv.t->size(); ++i) s += (*lv.t)[i] * (*lv.t)[i];
    return std::sqrt(s);
}

inline void FAMGHierarchy::flex_cycle(size_t idx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    flex_cycle_coarse(idx, rhs, x);
}

inline void FAMGHierarchy::flex_cycle_coarse(size_t startIdx, ::amgcl::backend::numa_vector<double>& rhs, ::amgcl::backend::numa_vector<double>& x) const {
    // Honest Fluent Flexible walk (Ch. 18.6.3, Fig 18.6.3):
    // iterative, revisiting levels. A level is visited repeatedly;
    // R_i < alpha*R0 -> go finer, R_i > beta*R_{i-1} -> go coarser.
    // Example walk: 0-1-2-3-2-3-2-1-0-1-2-1-0
    lastFlexVisit_.clear();
    const size_t L = levels_.size();
    if (L == 0) return;
    const double alpha = cfg_.flexAlpha; // 0.1
    const double beta = cfg_.flexBeta;   // 0.7
    // Per-level stored residual at entry and previous iteration
    std::vector<double> R0(L, 0.0), Rprev(L, 0.0);
    std::vector<int> sweeps(L, 0);

    // Helper to get level iterator
    auto levelAt = [&](size_t i) {
        auto it = levels_.begin();
        std::advance(it, (ptrdiff_t)i);
        return it;
    };

    // For idx 0, rhs/x are external; for idx>0, rhs/x are f/u of that level.
    // We keep "curRhs/curX" pointing via lambdas that fetch correct buffer.
    size_t cur = startIdx;
    // Initialize finest R0
    {
        auto it = levelAt(cur);
        R0[cur] = residualNorm_(cur, rhs, x);
        Rprev[cur] = R0[cur];
    }

    const int maxSteps = cfg_.flexMaxSweeps * (int)L * 3 + 10;
    for (int step = 0; step < maxSteps; ++step) {
        lastFlexVisit_.emplace_back(cur, R0[cur]);
        ++sweeps[cur];
        auto itCur = levelAt(cur);
        auto& lv = *itCur;

        // Get rhs/x for current level
        auto getRHSCur = [&]() -> ::amgcl::backend::numa_vector<double>* {
            if (cur == 0) return &rhs;
            return lv.f.get();
        };
        auto getXCur = [&]() -> ::amgcl::backend::numa_vector<double>* {
            if (cur == 0) return &x;
            return lv.u.get();
        };

        const bool coarsest = (cur + 1 >= L);
        if (coarsest) {
            if (lv.solve) (*lv.solve)(*lv.f, *lv.u);
            else {
                for (int k = 0; k < cfg_.preSweeps; ++k) lv.relax->apply_pre(*lv.A, *lv.f, *lv.u, *lv.t);
            }
            if (cur == 0) return;
            // go finer: prolong u to parent x
            auto itParent = levelAt(cur - 1);
            auto& parent = *itParent;
            auto* parentX = (cur - 1 == 0) ? &x : parent.u.get();
            ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *parent.P, *lv.u, ::amgcl::math::identity<double>(), *parentX);
            for (int k = 0; k < cfg_.postSweeps; ++k) parent.relax->apply_post(*parent.A, (cur - 1 == 0 ? rhs : *parent.f), *parentX, *parent.t);
            --cur;
            // Update Rprev for new level already visited will be recomputed
            Rprev[cur] = residualNorm_(cur, (cur == 0 ? rhs : *levelAt(cur)->f), (cur == 0 ? x : *levelAt(cur)->u));
            continue;
        }

        // Relax one sweep at current level
        {
            auto* curRhs = getRHSCur();
            auto* curX = getXCur();
            lv.relax->apply_pre(*lv.A, *curRhs, *curX, *lv.t);
        }
        double Ri;
        {
            auto* curRhs = getRHSCur();
            auto* curX = getXCur();
            // residualNorm_ needs buffers; for cur>0 use f/u directly
            ::amgcl::backend::residual(*curRhs, *lv.A, *curX, *lv.t);
            double s = 0;
            for (size_t i = 0; i < lv.t->size(); ++i) s += (*lv.t)[i] * (*lv.t)[i];
            Ri = std::sqrt(s);
        }

        // Termination: converged → go finer
        if (Ri < alpha * R0[cur] || Ri == 0) {
            if (cur == 0) {
                for (int k = 0; k < cfg_.postSweeps; ++k) lv.relax->apply_post(*lv.A, rhs, x, *lv.t);
                return;
            }
            auto itParent = levelAt(cur - 1);
            auto& parent = *itParent;
            auto* parentX = (cur - 1 == 0) ? &x : parent.u.get();
            ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *parent.P, *lv.u, ::amgcl::math::identity<double>(), *parentX);
            for (int k = 0; k < cfg_.postSweeps; ++k) parent.relax->apply_post(*parent.A, (cur - 1 == 0 ? rhs : *parent.f), *parentX, *parent.t);
            --cur;
            Rprev[cur] = residualNorm_(cur, (cur == 0 ? rhs : *levelAt(cur)->f), (cur == 0 ? x : *levelAt(cur)->u));
            continue;
        }

        // Reduction test: too slow → go coarser
        bool slow = (sweeps[cur] <= 1) ? (Ri > beta * R0[cur]) : (Ri > beta * Rprev[cur]);
        if (slow && sweeps[cur] <= cfg_.flexMaxSweeps) {
            auto itNext = std::next(itCur);
            ::amgcl::backend::residual(*getRHSCur(), *lv.A, *getXCur(), *lv.t);
            ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
            ::amgcl::backend::clear(*itNext->u);
            ++cur;
            R0[cur] = residualNorm_(cur, *levelAt(cur)->f, *levelAt(cur)->u);
            Rprev[cur] = R0[cur];
            continue;
        }

        // Otherwise stay and sweep again, unless max sweeps hit → force coarser
        if (sweeps[cur] >= cfg_.flexMaxSweeps) {
            auto itNext = std::next(itCur);
            ::amgcl::backend::residual(*getRHSCur(), *lv.A, *getXCur(), *lv.t);
            ::amgcl::backend::spmv(::amgcl::math::identity<double>(), *lv.R, *lv.t, ::amgcl::math::zero<double>(), *itNext->f);
            ::amgcl::backend::clear(*itNext->u);
            ++cur;
            R0[cur] = residualNorm_(cur, *levelAt(cur)->f, *levelAt(cur)->u);
            Rprev[cur] = R0[cur];
            continue;
        }

        Rprev[cur] = Ri;
    }
    // maxSteps exhausted: return current state
    return;
}

} // namespace fvm
