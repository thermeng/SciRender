#include "fvm/Divergence.h"
#include "fvm/FaceOperator.h"
#include "fvm/Validation.h"

#include <algorithm>
#include <stdexcept>

namespace fvm {

namespace {

// The prescribed value of a component in a cell, when the cell prescribes it.
inline bool prescribedValue(const std::vector<std::vector<char>>& mask,
                            const std::vector<std::vector<double>>& values,
                            std::size_t d, std::size_t cell, double& out) {
    if (d >= mask.size() || mask[d].empty() || cell >= mask[d].size()) return false;
    if (!mask[d][cell]) return false;
    if (d >= values.size() || cell >= values[d].size()) return false;
    out = values[d][cell];
    return true;
}

} // namespace

template<std::size_t Dim>
void computeDivergenceInto(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity, Field<Dim>& div,
                           const std::vector<std::vector<char>>& dirichletMask,
                           const std::vector<std::vector<double>>& dirichletValues,
                           double* sumFlux) {
    const std::size_t n = mesh.nCells();
    validateVelocityMask(Dim, n, dirichletMask, dirichletValues);

    FaceOperator<Dim> op(mesh);
    op.divergence(velocity, div, dirichletMask, dirichletValues, sumFlux);
}

template<std::size_t Dim>
Field<Dim> computeDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity,
                             const std::vector<std::vector<char>>& dirichletMask,
                             const std::vector<std::vector<double>>& dirichletValues) {
    Field<Dim> div(mesh, 0.0);
    computeDivergenceInto(mesh, velocity, div, dirichletMask, dirichletValues);
    return div;
}

template<std::size_t Dim>
double maxDivergence(const Mesh<Dim>& mesh, const VectorField<Dim>& velocity,
                     const std::vector<std::vector<char>>& dirichletMask,
                     const std::vector<std::vector<double>>& dirichletValues) {
    Field<Dim> div(mesh, 0.0);
    computeDivergenceInto(mesh, velocity, div, dirichletMask, dirichletValues);

    double maxDiv = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        // A cell with a prescribed velocity is excluded: its mass balance
        // cannot be corrected by the pressure correction. The boundary cells
        // are included - the imbalance of a wall cell is a genuine solver
        // error, which is exactly what the residual reports.
        bool isPrescribed = false;
        for (std::size_t d = 0; d < Dim && !isPrescribed; ++d) {
            double prescribed = 0.0;
            isPrescribed = prescribedValue(dirichletMask, dirichletValues, d, i, prescribed);
        }
        if (isPrescribed) continue;
        maxDiv = std::max(maxDiv, std::abs(div[i]));
    }
    return maxDiv;
}

template void computeDivergenceInto<2>(const Mesh<2>&, const VectorField<2>&, Field<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&, double*);
template void computeDivergenceInto<3>(const Mesh<3>&, const VectorField<3>&, Field<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&, double*);

template Field<2> computeDivergence<2>(const Mesh<2>&, const VectorField<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
template Field<3> computeDivergence<3>(const Mesh<3>&, const VectorField<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

template double maxDivergence<2>(const Mesh<2>&, const VectorField<2>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);
template double maxDivergence<3>(const Mesh<3>&, const VectorField<3>&,
    const std::vector<std::vector<char>>&, const std::vector<std::vector<double>>&);

} // namespace fvm
