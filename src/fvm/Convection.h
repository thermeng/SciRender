#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include "fvm/SIMPLETypes.h"
#include "fvm/SlopeLimiter.h"
#include <array>
#include <vector>
#include <cstddef>

namespace fvm {

// The boundary conditions of a convection operator:
//   - the prescribed value of the convected field, per cell;
//   - the prescribed values of the velocity components, per component.
//
// The velocity values are needed because the face velocity follows the rule of
// the momentum assembly: a cell with a prescribed velocity owns the WHOLE face
// it shares with a neighbour (the neighbour takes priority).
struct ConvectionBCs {
    std::vector<char> scalarMask;
    std::vector<double> scalarValues;
    std::vector<std::vector<char>> velocityMask;
    std::vector<std::vector<double>> velocityValues;
    // Optional per-face prescribed scalar values at inlet boundaries. When
    // non-empty and a boundary face has F < 0 (inflow through a velocity
    // inlet/MovingWall), the convective flux is added with this prescribed
    // face value instead of being suppressed. Indexed by face index.
    std::vector<double> faceScalarInletValues;
    // Optional per-face momentum boundary conditions: when present, the
    // boundary path of the assembly resolves the face velocity through
    // boundaryFaceState() — the shared BC policy (no-penetration, backflow)
    // that the momentum assembly uses — instead of the raw owner-cell values.
    const std::vector<FaceVelocityBC>* faceBC = nullptr;
};

template<std::size_t Dim>
class Convection {
public:
    Convection(const Mesh<Dim>& mesh);

    void setScheme(ConvectionScheme scheme);
    ConvectionScheme scheme() const;

    // The slope limiter applied to the second-order-upwind reconstruction. It
    // limits the face value, never the base upwind operator, so changing it
    // cannot change the upwind or central schemes.
    void setLimiter(const SlopeLimiterConfig& config);
    const SlopeLimiterConfig& limiterConfig() const { return limiter_.config(); }

    // Assembles into a sparse matrix, which is ADDED to (the caller has
    // usually filled it with the diffusion operator first). The diagonal is
    // part of the matrix.
    void discretize(SparseMatrix& A, Eigen::VectorXd& b,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const ConvectionBCs& bcs,
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    // Assembles into triplets; the diagonal is returned separately, for the
    // fused assembly that adds it itself.
    void discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b, std::vector<double>& diagInc,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const ConvectionBCs& bcs,
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    // Assembles into triplets; the diagonal is part of them.
    void discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const ConvectionBCs& bcs,
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    const Mesh<Dim>& mesh() const;

private:
    // The one implementation all three entry points share.
    void discretizeImpl(std::vector<Triplet>& triplets, Eigen::VectorXd& b, std::vector<double>& diagInc,
                        const Field<Dim>& phi,
                        const std::array<const Field<Dim>*, Dim>& velocities,
                        const ConvectionBCs& bcs,
                        const std::array<Field<Dim>, Dim>* phiGrad);

    const Mesh<Dim>& mesh_;
    ConvectionScheme scheme_ = ConvectionScheme::Upwind;
    SlopeLimiter<Dim> limiter_;
};

extern template class Convection<2>;
extern template class Convection<3>;

} // namespace fvm
