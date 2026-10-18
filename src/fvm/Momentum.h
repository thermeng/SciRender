#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/Convection.h"
#include "core/AlignedBuffer.hpp"
#include <vector>
#include <cstddef>

namespace fvm {

struct MomentumBCs {
    const std::vector<char>* slipWallMask = nullptr;
    const std::vector<std::vector<char>>* dirichletMaskAll = nullptr;
    const std::vector<std::vector<double>>* dirichletValsAll = nullptr;
    const std::vector<FaceVelocityBC>* faceBC = nullptr;
    const std::vector<char>* faceHasBC = nullptr;
};

template<std::size_t Dim>
class Momentum {
public:
    Momentum(const Mesh<Dim>& mesh, double rho, double nu);

    Field<Dim> assemble(
        std::size_t d,
        const VectorField<Dim>& velocity,
        const Field<Dim>& pressure,
        const Field<Dim>& source,
        const std::vector<char>& dirichletMask,
        const std::vector<double>& dirichletValues,
        SparseMatrix& A,
        Eigen::VectorXd& b,
        const Field<Dim>* nutField = nullptr,
        const std::array<Field<Dim>, Dim>* velGrad = nullptr);

    void assembleWithConvection(
        std::size_t d,
        const VectorField<Dim>& velocity,
        const Field<Dim>& pressure,
        const Field<Dim>& source,
        const std::vector<char>& dirichletMask,
        const std::vector<double>& dirichletValues,
        SparseMatrix& A,
        Eigen::VectorXd& b,
        Field<Dim>& diag,
        ConvectionScheme scheme = ConvectionScheme::Upwind,
        const Field<Dim>* nutField = nullptr,
        const std::array<Field<Dim>, Dim>* phiGrad = nullptr,
        const std::vector<std::vector<char>>& dirichletMaskAll = {},
        const std::vector<std::vector<double>>& dirichletValsAll = {},
        const MomentumBCs& bcs = {});

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
        const MomentumBCs& bcs = {});

    void assembleWithConvectionTransient(
        std::size_t d,
        const VectorField<Dim>& velocity,
        const std::array<Field<Dim>, Dim>& pressureGrad,
        const Field<Dim>& source,
        const std::vector<char>& dirichletMask,
        const std::vector<double>& dirichletValues,
        SparseMatrix& A,
        Eigen::VectorXd& b,
        Field<Dim>& diag,
        double dt,
        double dtPrev,
        const Field<Dim>* oldTime,
        const Field<Dim>* oldTimePrev,
        TimeScheme timeScheme,
        ConvectionScheme convScheme = ConvectionScheme::Upwind,
        const Field<Dim>* nutField = nullptr,
        const std::array<Field<Dim>, Dim>* phiGrad = nullptr,
        const MomentumBCs& bcs = {});

    void computePressureGradient(const Field<Dim>& pressure,
                                 std::array<Field<Dim>, Dim>& grad) const;
    // Fused Dim=3 single-pass pressure gradient — replaces Dim × O(nF) with 1 × O(nF)
    void computePressureGradientFused_(const Field<Dim>& pressure,
                                       std::array<Field<Dim>, Dim>& grad) const;

    void computeDCoeffAndRelax(SparseMatrix& A, Eigen::VectorXd& b,
                               const std::vector<char>& dirichletMask,
                               const Field<Dim>& velocityOld,
                               double alpha,
                               std::vector<double>& dCoeff,
                               bool simplec = false) const;

    const Mesh<Dim>& mesh() const;

private:
    double nuEff_(std::size_t P, std::size_t N, const Field<Dim>* nutField) const;
    Field<Dim> pressureGradientField_(std::size_t d, const Field<Dim>& pressure) const;

    const Mesh<Dim>& mesh_;
    double rho_;
    double nu_;

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
