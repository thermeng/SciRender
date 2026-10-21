#pragma once

#include "field/Field.h"
#include "field/VectorField.h"
#include "mesh/Mesh.h"

#include <array>
#include <cstddef>

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

} // namespace test_support
