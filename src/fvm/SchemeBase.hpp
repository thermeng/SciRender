#pragma once

#include "core/AlignedBuffer.hpp"
#include <cstddef>

namespace fvm {

template<typename Derived, std::size_t Dim>
class DiscretizationScheme {
public:
    DiscretizationScheme() = default;

    template<typename MeshType, typename Triplets, typename Vector>
    void assemble(Triplets& triplets, Vector& b, const MeshType& mesh) const {
        static_cast<const Derived*>(this)->assembleImpl(triplets, b, mesh);
    }

    void ensureWorkspace(std::size_t n) {
        if (workspace_.size() < n) {
            workspace_.resize(n);
        }
    }

    std::span<double> workspace() noexcept { return workspace_.span(); }

protected:
    ~DiscretizationScheme() = default;

private:
    AlignedBuffer<double> workspace_;
};

} // namespace fvm
