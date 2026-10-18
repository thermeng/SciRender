#pragma once

#include "mesh/Mesh.h"
#include "core/AlignedBuffer.hpp"
#include <span>
#include <vector>
#include <optional>
#include <cstddef>

namespace fvm {

template<std::size_t Dim>
class Field {
public:
    static constexpr std::size_t alignment = 64;

    Field() = default;
    Field(const Mesh<Dim>& mesh, double value = 0.0);

    Field(const Field&) = default;
    Field(Field&&) noexcept = default;
    Field& operator=(const Field&) = default;
    Field& operator=(Field&&) noexcept = default;

    void assign(double value);

    double& operator[](std::size_t idx);
    double operator[](std::size_t idx) const;

    template<typename... Coord>
     requires (sizeof...(Coord) == Dim)
    double& at(Coord... coords) {
        const auto& m = mesh_->get();
        if (m.isUnstructured()) throw std::logic_error("Field::at(coords) not valid for unstructured mesh");
        return data_[m.cellIndex(coords...)];
    }

    template<typename... Coord>
     requires (sizeof...(Coord) == Dim)
    double at(Coord... coords) const {
        const auto& m = mesh_->get();
        if (m.isUnstructured()) throw std::logic_error("Field::at(coords) not valid for unstructured mesh");
        return data_[m.cellIndex(coords...)];
    }

    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] const Mesh<Dim>& mesh() const;

    [[nodiscard]] std::span<const double> values() const noexcept;
    [[nodiscard]] std::span<double> values() noexcept;

    [[nodiscard]] const double* ptr() const noexcept { return data_.data(); }
    [[nodiscard]] double* ptr() noexcept { return data_.data(); }

    void addScaled(double alpha, const Field& other);

private:
    std::optional<std::reference_wrapper<const Mesh<Dim>>> mesh_;
    AlignedBuffer<double> data_;
};

extern template class Field<2>;
extern template class Field<3>;

} // namespace fvm
