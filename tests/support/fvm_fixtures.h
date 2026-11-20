#pragma once

#include "field/Field.h"
#include "field/VectorField.h"
#include "mesh/Mesh.h"
#include "fvm/Residual.h"

#include <array>
#include <cstddef>
#include <vector>

namespace test_support {

// ---------------------------------------------------------------------------
// Meshes
//
// Small, explicit meshes so the expected operator entries can be written out
// by hand instead of being copied from the implementation's own output.
// ---------------------------------------------------------------------------

// Uniform n x n structured mesh on [0, L]^2.
fvm::Mesh<2> uniform2D(int n, double L = 1.0);

// Geometrically stretched n x n structured mesh on [0, 1]^2.
// ratio > 1 clusters the cells towards the upper end of the axis.
fvm::Mesh<2> stretched2D(int n, double ratio);

// Uniform n x n x n structured mesh on [0, L]^3.
fvm::Mesh<3> uniform3D(int n, double L = 1.0);

// Unit square split into 2 x 2 x 2 = 8 triangles. The shared interior vertex
// is deliberately pushed off the grid centre so the interior faces are
// genuinely non-orthogonal (the faceWeight is then clearly different from 0.5,
// which is what separates a correct interpolation from a 0.5 average).
fvm::Mesh<2> triangles2D();

// ---------------------------------------------------------------------------
// Analytic field fillers
// ---------------------------------------------------------------------------

// f(x, y, z) -> value, evaluated at the cell centres.
template <std::size_t Dim, typename F>
void fillScalar(fvm::Field<Dim>& field, F fn) {
    const fvm::Mesh<Dim>& mesh = field.mesh();
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto c = mesh.cellCenter(i);
        field[i] = fn(c[0], c[1], c[2]);
    }
}

// f(x, y, z) -> {u, v, w}, evaluated at the cell centres.
template <std::size_t Dim, typename F>
void fillVector(fvm::VectorField<Dim>& field, F fn) {
    const fvm::Mesh<Dim>& mesh = field.mesh();
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        const auto c = mesh.cellCenter(i);
        field.set(i, fn(c[0], c[1], c[2]));
    }
}

// ---------------------------------------------------------------------------
// Divergence checks for tests
//
// Production computes divergence only as part of the continuity residual.
// That residual fills the same per-cell buffer with the same face values, so
// tests read it back from there instead of depending on a standalone operator.
// ---------------------------------------------------------------------------
template <std::size_t Dim>
void computeDivergenceInto(const fvm::Mesh<Dim>& mesh,
                           const fvm::VectorField<Dim>& velocity,
                           fvm::Field<Dim>& div,
                           const std::vector<std::vector<char>>& dirichletMask = {},
                           const std::vector<std::vector<double>>& dirichletValues = {}) {
    const fvm::VectorField<Dim> velocityOld(mesh, 0.0);
    const std::array<fvm::Field<Dim>, Dim> diag{};
    fvm::computeResiduals(mesh, velocity, velocityOld, diag, div,
                          dirichletMask, dirichletValues);
}

} // namespace test_support
