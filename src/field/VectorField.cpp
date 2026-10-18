#include "field/VectorField.h"

namespace fvm {

template<std::size_t Dim>
VectorField<Dim>::VectorField(const Mesh<Dim>& mesh, double value)
    : mesh_(std::ref(mesh)) {
    for (auto& c : components_) c = Field<Dim>(mesh, value);
}

template<std::size_t Dim>
Field<Dim>& VectorField<Dim>::operator[](std::size_t d) {
    return components_[d];
}

template<std::size_t Dim>
const Field<Dim>& VectorField<Dim>::operator[](std::size_t d) const {
    return components_[d];
}

template<std::size_t Dim>
Field<Dim>& VectorField<Dim>::x() {
    return components_[0];
}

template<std::size_t Dim>
Field<Dim>& VectorField<Dim>::y() requires (Dim >= 2) {
    return components_[1];
}

template<std::size_t Dim>
Field<Dim>& VectorField<Dim>::z() requires (Dim >= 3) {
    return components_[2];
}

template<std::size_t Dim>
const Field<Dim>& VectorField<Dim>::x() const {
    return components_[0];
}

template<std::size_t Dim>
const Field<Dim>& VectorField<Dim>::y() const requires (Dim >= 2) {
    return components_[1];
}

template<std::size_t Dim>
const Field<Dim>& VectorField<Dim>::z() const requires (Dim >= 3) {
    return components_[2];
}

template<std::size_t Dim>
std::array<double, Dim> VectorField<Dim>::value(std::size_t idx) const {
    std::array<double, Dim> v{};
    for (std::size_t d = 0; d < Dim; ++d) v[d] = components_[d][idx];
    return v;
}

template<std::size_t Dim>
void VectorField<Dim>::set(std::size_t idx, const std::array<double, Dim>& vals) {
    for (std::size_t d = 0; d < Dim; ++d) components_[d][idx] = vals[d];
}

template<std::size_t Dim>
void VectorField<Dim>::assign(double value) {
    for (auto& c : components_) c.assign(value);
}

template<std::size_t Dim>
void VectorField<Dim>::assign(std::size_t d, double value) {
    components_[d].assign(value);
}

template<std::size_t Dim>
const Mesh<Dim>& VectorField<Dim>::mesh() const {
    return mesh_->get();
}

template<std::size_t Dim>
std::size_t VectorField<Dim>::size() const {
    return components_[0].size();
}

template<std::size_t Dim>
void VectorField<Dim>::underRelax(double alpha, const VectorField& old) {
    for (std::size_t d = 0; d < Dim; ++d) {
        const auto n = components_[d].size();
        const double* __restrict__ oldData = old.components_[d].ptr();
        double* __restrict__ newData = components_[d].ptr();
        #pragma omp simd aligned(oldData, newData : 64)
        for (std::size_t i = 0; i < n; ++i) {
            newData[i] = alpha * newData[i] + (1.0 - alpha) * oldData[i];
        }
    }
}

template<std::size_t Dim>
std::span<const double> VectorField<Dim>::componentData(std::size_t d) const {
    return components_[d].values();
}

template<std::size_t Dim>
std::span<double> VectorField<Dim>::componentData(std::size_t d) {
    return components_[d].values();
}

template class VectorField<2>;
template class VectorField<3>;

} // namespace fvm
