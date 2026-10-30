#include "fvm/Convection.h"
#include "fvm/Gradient.h"
#include <stdexcept>

namespace fvm {

// Helper: Barth-Jespersen limited SOU correction
template<std::size_t Dim>
inline double limitedSOUCorrection(
    const Mesh<Dim>& mesh,
    std::size_t up,
    std::size_t f,
    double gradDotR,
    double phiUp,
    const Field<Dim>& phi,
    const std::vector<char>& dirichletMask,
    const std::vector<double>& dirichletValues)
{
    // Find min/max among neighbors of up (including up itself)
    double phiMin = phiUp;
    double phiMax = phiUp;
    const auto& neighbors = mesh.cellNeighbors(up);
    for (std::size_t nb : neighbors) {
        double val;
        if (!dirichletMask.empty() && nb < dirichletMask.size() && dirichletMask[nb] &&
            nb < dirichletValues.size()) {
            val = dirichletValues[nb];
        } else {
            val = phi[nb];
        }
        if (val < phiMin) phiMin = val;
        if (val > phiMax) phiMax = val;
    }
    double phiF = phiUp + gradDotR;
    // Also consider up itself for bounds already, but clamp
    if (phiF > phiMax) phiF = phiMax;
    else if (phiF < phiMin) phiF = phiMin;
    return phiF - phiUp;
}

template<std::size_t Dim>
Convection<Dim>::Convection(const Mesh<Dim>& mesh) : mesh_(mesh) {}

template<std::size_t Dim>
void Convection<Dim>::setScheme(ConvectionScheme scheme) {
    scheme_ = scheme;
}

template<std::size_t Dim>
ConvectionScheme Convection<Dim>::scheme() const {
    return scheme_;
}

// ---------------------------------------------------------------------------
// The one implementation.
//
// The face velocity follows the rule of the momentum assembly: a cell with a
// prescribed velocity owns the WHOLE face it shares with a neighbour (the
// neighbour takes priority), otherwise the face velocity is the exact linear
// interpolation between the two cell centres.
//
// Second order upwind reconstructs the face value from the cell gradient.
// Without a gradient the scheme would silently degrade to plain upwind, so
// the call is rejected instead.
// ---------------------------------------------------------------------------
template<std::size_t Dim>
void Convection<Dim>::discretizeImpl(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                                     std::vector<double>& diagInc,
                                     const Field<Dim>& phi,
                                     const std::array<const Field<Dim>*, Dim>& velocities,
                                     const ConvectionBCs& bcs,
                                     const std::array<Field<Dim>, Dim>* phiGrad) {
    const std::size_t n = mesh_.nCells();
    const std::size_t nF = mesh_.nFaces();
    const bool hasMask = !bcs.scalarMask.empty();
    const bool hasMaskAll = !bcs.velocityMask.empty();
    const bool useSOU = (scheme_ == ConvectionScheme::SecondOrderUpwind);
    const bool isUpwindBase = (scheme_ == ConvectionScheme::Upwind || useSOU);

    if (useSOU && phiGrad == nullptr)
        throw std::invalid_argument(
            "Convection::discretize: second order upwind needs the cell gradient");

    const std::array<Field<Dim>, Dim>* gradPtr = phiGrad;
    diagInc.assign(n, 0.0);

    // The flux of a face, split between the two cells it separates.
    auto addConvFlux = [&](double flux, std::size_t from, std::size_t to) {
        if (isUpwindBase) {
            if (flux >= 0.0) {
                diagInc[from] += flux;
            } else {
                if (hasMask && bcs.scalarMask[to]) b(from) -= flux * bcs.scalarValues[to];
                else if (!(hasMask && bcs.scalarMask[from])) triplets.emplace_back(from, to, flux);
            }
        } else {
            // Central
            diagInc[from] += 0.5 * flux;
            if (hasMask && bcs.scalarMask[to]) b(from) -= 0.5 * flux * bcs.scalarValues[to];
            else if (!(hasMask && bcs.scalarMask[from])) triplets.emplace_back(from, to, 0.5 * flux);
        }
    };

    for (std::size_t f = 0; f < nF; ++f) {
        const std::size_t P = mesh_.owner(f);
        const std::size_t N = mesh_.neighbor(f);
        if (N == std::size_t(-1)) {
            // Boundary face. The base flux has to be added for EVERY scheme:
            // the previous code only handled it inside the SOU branch, so with
            // plain upwind, central differencing or second-order upwind the convective flux
            // through the boundary was dropped entirely and the transported
            // quantity did not balance at the outlet.
            double un = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd) {
                double uFace = 0.0;
                if (hasMaskAll && !bcs.velocityMask[dd].empty() && bcs.velocityMask[dd][P])
                    uFace = bcs.velocityValues[dd][P];
                else
                    uFace = (*velocities[dd])[P];
                un += uFace * mesh_.faceNormal(f)[dd];
            }
            const double F = un * mesh_.faceArea(f);

            if (hasMask && bcs.scalarMask[P]) {
                // The owner cell is prescribed: its row is an identity row, so
                // the face contributes nothing.
                continue;
            }

            if (F >= 0.0) {
                // Outflow: the face value is the owner value.
                diagInc[P] += F;
                if (useSOU) {
                    const double* xF = mesh_.faceCenterPtr(f);
                    const double* xUp = mesh_.cellCenterPtr(P);
                    double gradDotR = 0.0;
                    for (std::size_t d = 0; d < Dim; ++d)
                        gradDotR += (*gradPtr)[d][P] * (xF[d] - xUp[d]);
                    double limited = limitedSOUCorrection(mesh_, P, f, gradDotR, phi[P], phi,
                                                           bcs.scalarMask, bcs.scalarValues);
                    // Known LHS term -> RHS with a sign flip (see the SOU branch).
                    b(P) -= F * limited;
                }
            } else {
                // Inflow at the boundary (a reversed outflow): the upwind
                // value is outside the domain and unknown, so the convective
                // flux is suppressed rather than adding a destabilising
                // negative diagonal.
            }
            continue;
        }
        double w = mesh_.faceWeight(f);
        double un = 0.0;
        for (std::size_t dd = 0; dd < Dim; ++dd) {
            double uFace;
            if (hasMaskAll && !bcs.velocityMask[dd].empty() && bcs.velocityMask[dd][N])
                uFace = bcs.velocityValues[dd][N];
            else if (hasMaskAll && !bcs.velocityMask[dd].empty() && bcs.velocityMask[dd][P])
                uFace = bcs.velocityValues[dd][P];
            else
                uFace = (1.0 - w) * (*velocities[dd])[P] + w * (*velocities[dd])[N];
            un += uFace * mesh_.faceNormal(f)[dd];
        }
        double F = un * mesh_.faceArea(f);
        addConvFlux(F, P, N);
        addConvFlux(-F, N, P);
        if (useSOU) {
            const double* xF = mesh_.faceCenterPtr(f);
            // Upwind cell is P for F >= 0 and N for F < 0. The deferred
            // correction belongs to both cells; the downwind cell always needs
            // its share, even when the upwind cell is prescribed (then the
            // upwind contribution of this face was moved to the downwind
            // cell's right-hand side by addConvFlux and the two must still
            // cancel).
            const bool upIsP = (F >= 0.0);
            const std::size_t upCell = upIsP ? P : N;
            const std::size_t downCell = upIsP ? N : P;
            const bool upPrescribed = hasMask && bcs.scalarMask[upCell];
            const bool downPrescribed = hasMask && bcs.scalarMask[downCell];
            const double* xUp = mesh_.cellCenterPtr(upCell);
            double gradDotR = 0.0;
            for (std::size_t d = 0; d < Dim; ++d) gradDotR += (*gradPtr)[d][upCell] * (xF[d] - xUp[d]);
            double phiUp = upPrescribed ? bcs.scalarValues[upCell] : phi[upCell];
            double limited = limitedSOUCorrection(mesh_, upCell, f, gradDotR, phiUp, phi,
                                                   bcs.scalarMask, bcs.scalarValues);
            if (upIsP) {
                if (!upPrescribed) b(P) -= F * limited;
                if (!downPrescribed) b(N) += F * limited;
            } else {
                if (!upPrescribed) b(N) -= (-F) * limited;
                if (!downPrescribed) b(P) += (-F) * limited;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The three entry points. They differ only in where the result is delivered,
// so they always assemble the same operator.
// ---------------------------------------------------------------------------

template<std::size_t Dim>
void Convection<Dim>::discretize(SparseMatrix& A, Eigen::VectorXd& b,
                                 const Field<Dim>& phi,
                                 const std::array<const Field<Dim>*, Dim>& velocities,
                                 const ConvectionBCs& bcs,
                                 const std::array<Field<Dim>, Dim>* phiGrad) {
    const std::size_t n = mesh_.nCells();
    std::vector<Triplet> convTriplets;
    std::vector<double> diagInc;
    convTriplets.reserve(n * (2 * Dim + 1));
    discretizeImpl(convTriplets, b, diagInc, phi, velocities, bcs, phiGrad);
    for (std::size_t i = 0; i < n; ++i)
        if (diagInc[i] != 0.0) convTriplets.emplace_back(static_cast<int>(i), static_cast<int>(i), diagInc[i]);
    SparseMatrix Aconv(n, n);
    Aconv.setFromTriplets(convTriplets.begin(), convTriplets.end());
    A += Aconv;
}

template<std::size_t Dim>
void Convection<Dim>::discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                                 std::vector<double>& diagInc,
                                 const Field<Dim>& phi,
                                 const std::array<const Field<Dim>*, Dim>& velocities,
                                 const ConvectionBCs& bcs,
                                 const std::array<Field<Dim>, Dim>* phiGrad) {
    discretizeImpl(triplets, b, diagInc, phi, velocities, bcs, phiGrad);
}

template<std::size_t Dim>
void Convection<Dim>::discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                                 const Field<Dim>& phi,
                                 const std::array<const Field<Dim>*, Dim>& velocities,
                                 const ConvectionBCs& bcs,
                                 const std::array<Field<Dim>, Dim>* phiGrad) {
    const std::size_t n = mesh_.nCells();
    std::vector<double> diagInc;
    discretizeImpl(triplets, b, diagInc, phi, velocities, bcs, phiGrad);
    for (std::size_t i = 0; i < n; ++i)
        if (diagInc[i] != 0.0) triplets.emplace_back(static_cast<int>(i), static_cast<int>(i), diagInc[i]);
}

template<std::size_t Dim>
const Mesh<Dim>& Convection<Dim>::mesh() const { return mesh_; }

template class Convection<2>;
template class Convection<3>;

} // namespace fvm
