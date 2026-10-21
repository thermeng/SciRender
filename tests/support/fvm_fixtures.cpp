#include "support/fvm_fixtures.h"

#include <vector>

namespace test_support {

fvm::Mesh<2> uniform2D(int n, double L) {
    return fvm::Mesh<2>(std::array<std::size_t, 2>{static_cast<std::size_t>(n),
                                                     static_cast<std::size_t>(n)},
                         std::array<double, 2>{L, L});
}

fvm::Mesh<2> stretched2D(int n, double ratio) {
    return fvm::Mesh<2>::makeStretching(
        std::array<std::size_t, 2>{static_cast<std::size_t>(n), static_cast<std::size_t>(n)},
        std::array<double, 2>{1.0, 1.0}, std::array<double, 2>{ratio, ratio});
}

fvm::Mesh<3> uniform3D(int n, double L) {
    return fvm::Mesh<3>(std::array<std::size_t, 3>{static_cast<std::size_t>(n),
                                                     static_cast<std::size_t>(n),
                                                     static_cast<std::size_t>(n)},
                         std::array<double, 2 + 1>{L, L, L});
}

fvm::Mesh<2> triangles2D() {
    // 3 x 3 vertex grid on the unit square. The middle vertex is pushed to
    // (0.56, 0.44) so the interior faces are skewed: the owner->neighbour
    // direction is then not perpendicular to the face normal, and the exact
    // interpolation weight differs visibly from 0.5.
    std::vector<std::array<double, 2>> vertices = {
        {0.00, 0.00}, {0.50, 0.00}, {1.00, 0.00},
        {0.00, 0.50}, {0.56, 0.44}, {1.00, 0.50},
        {0.00, 1.00}, {0.50, 1.00}, {1.00, 1.00},
    };
    std::vector<std::vector<std::size_t>> cells = {
        {0, 1, 4}, {0, 4, 3},   // lower-left quad
        {1, 2, 5}, {1, 5, 4},   // lower-right quad
        {3, 4, 7}, {3, 7, 6},   // upper-left quad
        {4, 5, 8}, {4, 8, 7},   // upper-right quad
    };
    return fvm::Mesh<2>(std::move(vertices), std::move(cells));
}

} // namespace test_support
