#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Convection.h"
#include "fvm/SlopeLimiter.h"
#include "core/AlignedBuffer.hpp"
#include <vector>
#include <cstddef>

namespace fvm {

struct MomentumBCs {
    const std::vector<char>* slipWallMask = nullptr;
    const std::vector<std::vector<char>>* dirichletMaskAll = nullptr;
    const std::vector<std::vector<double>>* dirichletValsAll = nullptr;
    const std::vector<FaceVelocityBC>* faceBC = nullptr;
};

// Time terms for the implicit momentum assembly (transient path): the
// old-time and old-time-prev fields plus the time scheme. The steady path
// passes the default and no time terms are added.
template<std::size_t Dim>
struct TransientParams {
    bool hasTime = false;
    double dt = 0.0;
    double dtPrev = 0.0;
    TimeScheme timeScheme = TimeScheme::Euler;
    const Field<Dim>* oldTime = nullptr;
    const Field<Dim>* oldTimePrev = nullptr;
};

template<std::size_t Dim>
class Momentum {
public:
    Momentum(const Mesh<Dim>& mesh, double rho, double nu);

    // Assemble the momentum operator (convection + diffusion) into A and b.
    // With `tp.hasTime` the BDF2/Euler time terms are added on top (the
    // transient path); the steady path passes the default, which adds none.
    void assembleWithConvection(
        std::size_t d,
        const VectorField<Dim>& velocity,
        const std::array<Field<Dim>, Dim>& pressureGrad,
        const Field<Dim>& source,
        const std::vector<char>& dirichletMask,
        const std::vector<double>& dirichletValues,
        SparseMatrix& A,
        Eigen::VectorXd& b,
        Field<Dim>& diag,
        ConvectionScheme scheme = ConvectionScheme::Upwind,
        const Field<Dim>* nutField = nullptr,
        const std::array<Field<Dim>, Dim>* phiGrad = nullptr,
        const MomentumBCs& bcs = {},
        const TransientParams<Dim>& tp = TransientParams<Dim>{});

    // The slope limiter applied to the second-order-upwind reconstruction of the
    // momentum components. It limits the face value, never the base upwind
    // operator, so it cannot change the upwind or central schemes.
    void setLimiter(const SlopeLimiterConfig& config) { limiter_.configure(config); }

    void computeDCoeffAndRelax(SparseMatrix& A, Eigen::VectorXd& b,
                               const std::vector<char>& dirichletMask,
                               const Field<Dim>& velocityOld,
                               double alpha,
                               std::vector<double>& dCoeff,
                               bool simplec = false) const;

    const Mesh<Dim>& mesh() const;

private:
    double nuEff_(std::size_t P, std::size_t N, const Field<Dim>* nutField) const;

    const Mesh<Dim>& mesh_;
    double rho_;
    double nu_;
    SlopeLimiter<Dim> limiter_;

    mutable AlignedBuffer<double> diagSum_;
    mutable AlignedBuffer<double> diagInc_;
    mutable std::vector<Triplet> triplets_;
    mutable bool workspaceInitialized_ = false;

    // Pattern cache: O(nnz log nnz) sort only on topology change, otherwise O(nnz) scatter
    mutable std::array<std::size_t, 3> momPatternHash_{};
    mutable std::array<std::vector<int>, 3> momTripletPos_{};
    mutable std::array<bool, 3> momPatternBuilt_{};
    // Diagonal positions for O(n) dCoeff (avoids O(n*degree) coeff scan)
    mutable std::array<std::vector<int>, 3> momDiagPos_{};

    void ensureWorkspace_(std::size_t n) const;
    void buildMomentumScatterMap_(SparseMatrix& A, std::size_t d) const;
    void updateMomentumValues_(SparseMatrix& A, std::size_t d) const;
};

extern template class Momentum<2>;
extern template class Momentum<3>;

} // namespace fvm
