#pragma once
#include <memory>
#include <cstdint>

#include "field/Field.h"
#include "field/VectorField.h"

namespace fvm {
template<std::size_t Dim> class VectorField;
template<std::size_t Dim> class Field;
}

struct TransientStep {
    int step = 0;
    double time = 0.0;
    std::shared_ptr<const fvm::VectorField<2>> velocity;
    std::shared_ptr<const fvm::Field<2>> pressure;
    uint64_t gen = 0;
};
