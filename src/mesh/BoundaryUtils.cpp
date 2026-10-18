#include "mesh/BoundaryUtils.h"
#include "mesh/Mesh.h"
#include <array>

namespace fvm {

template<std::size_t Dim>
void forEachBoundaryCell(const Mesh<Dim>& mesh, std::size_t d, int sign,
                         std::function<void(std::size_t)> callback) {
    std::array<std::size_t, Dim> c{};
    auto traverse = [&](auto&& self, std::size_t dim) -> void {
        if (dim == Dim) {
            callback(mesh.cellIndex(c));
            return;
        }
        if (dim == d) {
            c[dim] = (sign < 0) ? 0 : mesh.n(dim) - 1;
            self(self, dim + 1);
        } else {
            for (std::size_t i = 0; i < mesh.n(dim); ++i) {
                c[dim] = i;
                self(self, dim + 1);
            }
        }
    };
    traverse(traverse, 0);
}

template<std::size_t Dim>
void forEachBoundaryFace(const Mesh<Dim>& mesh,
                         std::function<void(std::size_t, std::size_t)> callback) {
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1)) {
            callback(f, mesh.owner(f));
        }
    }
}

template<std::size_t Dim>
void forEachBoundaryFace(const Mesh<Dim>& mesh, std::size_t d, int sign,
                         std::function<void(std::size_t, std::size_t)> callback) {
    const auto& cached = mesh.boundaryFaces(d, sign);
    if (!cached.empty()) {
        for (std::size_t f : cached) callback(f, mesh.owner(f));
        return;
    }
    for (std::size_t f = 0; f < mesh.nFaces(); ++f) {
        if (mesh.neighbor(f) == std::size_t(-1)) {
            const auto& normal = mesh.faceNormal(f);
            const double nd = normal[d];
            double maxComp = 0.0;
            for (std::size_t dd = 0; dd < Dim; ++dd)
                maxComp = std::max(maxComp, std::abs(normal[dd]));
            if (std::abs(nd) >= 0.9 * maxComp
                && ((sign < 0 && nd < -0.5) || (sign > 0 && nd > 0.5))) {
                callback(f, mesh.owner(f));
            }
        }
    }
}

template void forEachBoundaryCell<2>(const Mesh<2>&, std::size_t, int,
                                     std::function<void(std::size_t)>);
template void forEachBoundaryCell<3>(const Mesh<3>&, std::size_t, int,
                                     std::function<void(std::size_t)>);

template void forEachBoundaryFace<2>(const Mesh<2>&,
                                     std::function<void(std::size_t, std::size_t)>);
template void forEachBoundaryFace<3>(const Mesh<3>&,
                                     std::function<void(std::size_t, std::size_t)>);

template void forEachBoundaryFace<2>(const Mesh<2>&, std::size_t, int,
                                     std::function<void(std::size_t, std::size_t)>);
template void forEachBoundaryFace<3>(const Mesh<3>&, std::size_t, int,
                                     std::function<void(std::size_t, std::size_t)>);

} // namespace fvm
