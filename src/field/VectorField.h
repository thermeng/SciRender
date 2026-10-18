#pragma once

#include "mesh/Mesh.h"
#include "field/Field.h"
#include <array>
#include <cstddef>
#include <span>
#include <optional>

namespace fvm {

template<std::size_t Dim>
class VectorField {
public:
    VectorField() = default;
    VectorField(const Mesh<Dim>& mesh, double value = 0.0);

    Field<Dim>& operator[](std::size_t d);
    const Field<Dim>& operator[](std::size_t d) const;

    Field<Dim>& x();
    Field<Dim>& y() requires (Dim >= 2);
    Field<Dim>& z() requires (Dim >= 3);
    const Field<Dim>& x() const;
    const Field<Dim>& y() const requires (Dim >= 2);
    const Field<Dim>& z() const requires (Dim >= 3);

    [[nodiscard]] std::array<double, Dim> value(std::size_t idx) const;

    void set(std::size_t idx, const std::array<double, Dim>& vals);

    void assign(double value);
    void assign(std::size_t d, double value);

    [[nodiscard]] const Mesh<Dim>& mesh() const;
    [[nodiscard]] std::size_t size() const;

    void underRelax(double alpha, const VectorField& old);

    [[nodiscard]] std::span<const double> componentData(std::size_t d) const;
    [[nodiscard]] std::span<double> componentData(std::size_t d);

private:
    std::array<Field<Dim>, Dim> components_{};
    std::optional<std::reference_wrapper<const Mesh<Dim>>> mesh_;
};

extern template class VectorField<2>;
extern template class VectorField<3>;

} // namespace fvm
