// Mesh geometry: the foundation every FVM operator rests on.
//
// If the geometry is wrong, every operator is wrong, so these tests pin the
// quantities the operators consume: cell volumes and centres, face areas,
// normals, distances and the exact interpolation weight.
#include "doctest/doctest.h"
#include "support/fvm_fixtures.h"

#include <array>
#include <cmath>

using namespace test_support;
using fvm::Mesh;

namespace {

// A face normal must be a unit vector along one axis.
bool isAxisNormal(const std::array<double, 2>& n) {
    const int nonzero = (std::abs(n[0]) > 1e-12) + (std::abs(n[1]) > 1e-12);
    return nonzero == 1 && std::abs(std::abs(n[0]) + std::abs(n[1]) - 1.0) < 1e-12;
}

bool isAxisNormal(const std::array<double, 3>& n) {
    const int nonzero =
        (std::abs(n[0]) > 1e-12) + (std::abs(n[1]) > 1e-12) + (std::abs(n[2]) > 1e-12);
    return nonzero == 1 && std::abs(std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]) - 1.0) < 1e-12;
}

} // namespace

TEST_SUITE("cfd") {

// A uniform 4x4 unit square: h = 0.25, so every cell has area h^2, every face
// has length h, interior faces are h apart and boundary faces h/2.
TEST_CASE("cfd.mesh.uniform2d_geometry") {
    const int n = 4;
    const double L = 1.0;
    const double h = L / n;
    const Mesh<2> mesh = uniform2D(n, L);

    CHECK_MESSAGE(mesh.nCells() == 16, "cell count");
    CHECK_MESSAGE(mesh.nFaces() == 2 * n * (n + 1), "face count");
    CHECK_MESSAGE(mesh.isUniform(), "mesh must report itself as uniform");
    CHECK_MESSAGE(!mesh.isUnstructured(), "mesh must not report itself as unstructured");

    // Cell volumes and centres.
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(mesh.cellVolume(i) - h * h) < 1e-15, "cell volume == h^2");
        const auto c = mesh.cellCoords(i);
        const auto centre = mesh.cellCenter(i);
        CHECK_MESSAGE(std::abs(centre[0] - (static_cast<double>(c[0]) + 0.5) * h) < 1e-12,
                      "cell centre x");
        CHECK_MESSAGE(std::abs(centre[1] - (static_cast<double>(c[1]) + 0.5) * h) < 1e-12,
                      "cell centre y");
    }

    // Faces.
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        CAPTURE(f);
        CHECK_MESSAGE(std::abs(mesh.faceArea(f) - h) < 1e-15, "face area == h");
        CHECK_MESSAGE(isAxisNormal(mesh.faceNormal(f)), "face normal is a unit axis vector");
        const bool boundary = (mesh.neighbor(f) == std::size_t(-1));
        const double expectedDist = boundary ? 0.5 * h : h;
        CHECK_MESSAGE(std::abs(mesh.faceDist(f) - expectedDist) < 1e-12, "face distance");
        if (!boundary) {
            // The exact linear-interpolation weight is 0.5 on a uniform grid.
            // A boundary face has no neighbour, so its weight is meaningless.
            CHECK_MESSAGE(std::abs(mesh.faceWeight(f) - 0.5) < 1e-12, "face weight == 0.5");
        }
    }
}

// A stretched grid: the local spacing varies per cell, the average spacing
// is still L/N, and the cell volumes reflect the local spacing.
TEST_CASE("cfd.mesh.stretched2d_geometry") {
    const int n = 4;
    const Mesh<2> mesh = stretched2D(n, 1.2);

    CHECK_MESSAGE(mesh.nCells() == 16, "cell count");
    CHECK_MESSAGE(!mesh.isUniform(), "a stretched mesh must not report itself as uniform");

    for (std::size_t d = 0; d < 2; ++d) {
        CAPTURE(d);
        // The average spacing is the domain length over the cell count.
        CHECK_MESSAGE(std::abs(mesh.dx(d) - 0.25) < 1e-12, "average spacing == L/N");
        // The local spacings must differ from each other.
        double lo = 1e30;
        double hi = -1e30;
        double sum = 0.0;
        for (std::size_t c = 0; c < static_cast<std::size_t>(n); ++c) {
            const std::size_t idx = (d == 0) ? mesh.cellIndex(c, 0) : mesh.cellIndex(0, c);
            const double h = mesh.dx(d, idx);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
            sum += h;
        }
        MESSAGE("direction ", d, ": spacings from ", lo, " to ", hi, ", sum ", sum);
        CHECK_MESSAGE(hi - lo > 1e-6, "a stretched mesh must vary its spacing");
        // The local spacings must add up to the domain length.
        CHECK_MESSAGE(std::abs(sum - 1.0) < 1e-12, "local spacings sum to the domain length");
    }

    // The cell area is the product of the two local spacings.
    for (std::size_t j = 0; j < static_cast<std::size_t>(n); ++j) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n); ++i) {
            CAPTURE(i);
            CAPTURE(j);
            const std::size_t idx = mesh.cellIndex(i, j);
            const double expected = mesh.dx(0, idx) * mesh.dx(1, idx);
            MESSAGE("cell ", i, ",", j, ": volume ", mesh.cellVolume(idx), " expected ", expected);
            CHECK_MESSAGE(std::abs(mesh.cellVolume(idx) - expected) < 1e-12, "cell area");
        }
    }
}

// An unstructured triangle mesh: areas by the shoelace formula, unit face
// normals, and - the point of the fixture - genuinely non-orthogonal interior
// faces, so the exact interpolation weight differs from 0.5.
TEST_CASE("cfd.mesh.unstructured2d_geometry") {
    const Mesh<2> mesh = triangles2D();

    CHECK_MESSAGE(mesh.isUnstructured(), "mesh must report itself as unstructured");
    CHECK_MESSAGE(mesh.nCells() == 8, "cell count");

    double totalArea = 0.0;
    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        const double area = mesh.cellVolume(i);
        CHECK_MESSAGE(area > 0.0, "cell area must be positive");
        totalArea += area;
    }
    // The triangles tile the unit square exactly.
    CHECK_MESSAGE(std::abs(totalArea - 1.0) < 1e-12, "the cells must tile the unit square");

    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        CAPTURE(f);
        CHECK_MESSAGE(mesh.faceArea(f) > 0.0, "face area must be positive");
        const double norm = std::sqrt(mesh.faceNormal(f)[0] * mesh.faceNormal(f)[0] +
                                      mesh.faceNormal(f)[1] * mesh.faceNormal(f)[1]);
        CHECK_MESSAGE(std::abs(norm - 1.0) < 1e-12, "face normal is a unit vector");
    }

    // At least one interior face must be non-orthogonal, otherwise the fixture
    // cannot distinguish an exact interpolation from a 0.5 average.
    double maxSkew = 0.0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1))
            continue;
        maxSkew = std::max(maxSkew, std::abs(mesh.faceWeight(f) - 0.5));
    }
    CHECK_MESSAGE(maxSkew > 0.01, "the unstructured fixture must contain a skewed interior face");

    // The structured-only accessors must refuse to work on an unstructured
    // mesh instead of returning nonsense.
    CHECK_THROWS_AS(mesh.dx(0), std::logic_error);
    CHECK_THROWS_AS(mesh.cellCoords(0), std::logic_error);
}

// A uniform 4x4x4 cube: volumes h^3, face areas h^2, three normal directions.
TEST_CASE("cfd.mesh.uniform3d_geometry") {
    const int n = 4;
    const double L = 1.0;
    const double h = L / n;
    const Mesh<3> mesh = uniform3D(n, L);

    CHECK_MESSAGE(mesh.nCells() == 64, "cell count");
    CHECK_MESSAGE(mesh.nFaces() == 3 * n * n * (n + 1), "face count");

    for (std::size_t i = 0; i < mesh.nCells(); ++i) {
        CAPTURE(i);
        CHECK_MESSAGE(std::abs(mesh.cellVolume(i) - h * h * h) < 1e-15, "cell volume == h^3");
    }

    int directionsSeen = 0;
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        CAPTURE(f);
        CHECK_MESSAGE(std::abs(mesh.faceArea(f) - h * h) < 1e-15, "face area == h^2");
        CHECK_MESSAGE(isAxisNormal(mesh.faceNormal(f)), "face normal is a unit axis vector");
        const bool boundary = (mesh.neighbor(f) == std::size_t(-1));
        const double expectedDist = boundary ? 0.5 * h : h;
        CHECK_MESSAGE(std::abs(mesh.faceDist(f) - expectedDist) < 1e-12, "face distance");
        for (std::size_t d = 0; d < 3; ++d) {
            if (std::abs(mesh.faceNormal(f)[d]) > 0.5) {
                ++directionsSeen;
                break;
            }
        }
    }
    CHECK_MESSAGE(directionsSeen == static_cast<int>(mesh.nFaces()),
                  "every face must point along exactly one of the three axes");
}

} // TEST_SUITE("cfd")
