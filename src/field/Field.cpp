#include "field/Field.h"

namespace fvm {

template<std::size_t Dim>
Field<Dim>::Field(const Mesh<Dim>& mesh, double value)
    : mesh_(std::ref(mesh)), data_(mesh.nCells(), value) {}

template<std::size_t Dim>
void Field<Dim>::assign(double value) {
    data_.fill(value);
}

template<std::size_t Dim>
double& Field<Dim>::operator[](std::size_t idx) {
    return data_[idx];
}

template<std::size_t Dim>
double Field<Dim>::operator[](std::size_t idx) const {
    return data_[idx];
}

template<std::size_t Dim>
std::size_t Field<Dim>::size() const {
    return data_.size();
}

template<std::size_t Dim>
const Mesh<Dim>& Field<Dim>::mesh() const {
    return mesh_->get();
}

template<std::size_t Dim>
std::span<const double> Field<Dim>::values() const noexcept {
    return data_.span();
}

template<std::size_t Dim>
std::span<double> Field<Dim>::values() noexcept {
    return data_.span();
}

template<std::size_t Dim>
void Field<Dim>::addScaled(double alpha, const Field& other) {
    const auto n = data_.size();
    const double* __restrict__ src = other.data_.data();
    double* __restrict__ dst = data_.data();
    #pragma omp simd aligned(dst, src : 64)
    for (std::size_t i = 0; i < n; ++i) {
        dst[i] += alpha * src[i];
    }
}

template class Field<2>;
template class Field<3>;

} // namespace fvm
