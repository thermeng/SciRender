#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "fvm/Types.h"
#include "core/AlignedBuffer.hpp"
#include <array>
#include <vector>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class Convection {
public:
    Convection(const Mesh<Dim>& mesh);

    void setScheme(ConvectionScheme scheme);
    ConvectionScheme scheme() const;

    void discretize(SparseMatrix& A, Eigen::VectorXd& b,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const std::vector<char>& dirichletMask = {},
                    const std::vector<double>& dirichletValues = {},
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    void discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const std::vector<char>& dirichletMask = {},
                    const std::vector<double>& dirichletValues = {},
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr);

    void discretize(std::vector<Triplet>& triplets, Eigen::VectorXd& b,
                    std::vector<double>& diagInc,
                    const Field<Dim>& phi,
                    const std::array<const Field<Dim>*, Dim>& velocities,
                    const std::vector<char>& dirichletMask = {},
                    const std::vector<double>& dirichletValues = {},
                    const std::array<Field<Dim>, Dim>* phiGrad = nullptr,
                    const std::vector<std::vector<char>>& dirichletMaskAll = {},
                    const std::vector<std::vector<double>>& dirichletValsAll = {});

    const Mesh<Dim>& mesh() const;

private:
    template<typename TripletContainer>
    void discretizeImpl(TripletContainer& triplets, Eigen::VectorXd& b,
                        const Field<Dim>& phi,
                        const std::array<const Field<Dim>*, Dim>& velocities,
                        const std::vector<char>& dirichletMask,
                        const std::vector<double>& dirichletValues,
                        const std::array<Field<Dim>, Dim>* phiGrad);

    template<typename TripletContainer>
    void discretizeImplUnified(TripletContainer& triplets, Eigen::VectorXd& b,
                               std::vector<double>& diagInc,
                               const Field<Dim>& phi,
                               const std::array<const Field<Dim>*, Dim>& velocities,
                               const std::vector<char>& dirichletMask,
                               const std::vector<double>& dirichletValues,
                               const std::array<Field<Dim>, Dim>* phiGrad,
                               const std::vector<std::vector<char>>& dirichletMaskAll,
                               const std::vector<std::vector<double>>& dirichletValsAll);

    const Mesh<Dim>& mesh_;
    ConvectionScheme scheme_ = ConvectionScheme::Upwind;

    mutable AlignedBuffer<double> diagInc_;
    mutable bool workspaceInitialized_ = false;

    void ensureWorkspace_(std::size_t n) const;
};

extern template class Convection<2>;
extern template class Convection<3>;

} // namespace fvm
