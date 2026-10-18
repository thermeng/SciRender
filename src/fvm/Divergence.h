#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include "field/VectorField.h"
#include <cstddef>
#include <cmath>
#include <vector>

namespace fvm {

template<std::size_t Dim>
Field<Dim> computeDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity);

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div);

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           const std::vector<std::vector<char>>& dirichletMask,
                           const std::vector<std::vector<double>>& dirichletValues);

template<std::size_t Dim>
double maxDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity);

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           double& sumFlux);

extern template Field<2> computeDivergence<2>(const Mesh<2>&, const VectorField<2>&);
extern template Field<3> computeDivergence<3>(const Mesh<3>&, const VectorField<3>&);

extern template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&);
extern template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&);

extern template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
extern template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

extern template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&, double&);
extern template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&, double&);

extern template double maxDivergence<2>(const Mesh<2>&, const VectorField<2>&);
extern template double maxDivergence<3>(const Mesh<3>&, const VectorField<3>&);

} // namespace fvm
