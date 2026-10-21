#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include <cstddef>
#include <cmath>
#include <vector>

namespace fvm {

// The divergence of a velocity field.
//
// The face velocity follows the rule of the momentum assembly:
//   - a cell with a prescribed velocity contributes that value to the WHOLE
//     face it shares with a neighbour, and the neighbour takes priority;
//   - otherwise the face velocity is the exact linear interpolation between the
//     two cell centres (the mesh's faceWeight);
//   - at a boundary face the velocity is the prescribed value of the cell that
//     owns the face.
//
// The boundary condition is an explicit input: pass an empty mask when it is
// unknown, and a boundary face then carries the velocity of the cell that owns
// it.
template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           const std::vector<std::vector<char>>& dirichletMask,
                           const std::vector<std::vector<double>>& dirichletValues,
                           double* sumFlux = nullptr);

template<std::size_t Dim>
Field<Dim> computeDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity,
                             const std::vector<std::vector<char>>& dirichletMask,
                             const std::vector<std::vector<double>>& dirichletValues);

// The largest absolute divergence over the cells whose mass balance the
// pressure correction can still change - the same set the residual reports: a
// cell with a prescribed velocity is excluded (its mass balance cannot be
// corrected), the boundary cells are included (the imbalance of a wall cell is
// a genuine solver error).
template<std::size_t Dim>
double maxDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity,
                     const std::vector<std::vector<char>>& dirichletMask,
                     const std::vector<std::vector<double>>& dirichletValues);

extern template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&, double*);
extern template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&, double*);

extern template Field<2> computeDivergence<2>(const Mesh<2>&, const VectorField<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
extern template Field<3> computeDivergence<3>(const Mesh<3>&, const VectorField<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

extern template double maxDivergence<2>(const Mesh<2>&, const VectorField<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
extern template double maxDivergence<3>(const Mesh<3>&, const VectorField<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

} // namespace fvm
