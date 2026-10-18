#pragma once

#include "AlignedAllocator.hpp"
#include <span>
#include <vector>
#include <algorithm>
#include <cstddef>

namespace fvm {

template<typename T, std::size_t Alignment = 64>
using AlignedVector = std::vector<T, AlignedAllocator<T, Alignment>>;

template<typename T, std::size_t Alignment = 64>
class AlignedBuffer {
public:
    static constexpr std::size_t alignment = Alignment;

    AlignedBuffer() noexcept = default;

    explicit AlignedBuffer(std::size_t n)
        : data_(n) {}

    AlignedBuffer(std::size_t n, const T& val)
        : data_(n, val) {}

    void resize(std::size_t n) { data_.resize(n); }

    void assign(std::size_t n, const T& val) { data_.assign(n, val); }

    void clear() { data_.clear(); }

    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }

    T& at(std::size_t i) { return data_.at(i); }
    const T& at(std::size_t i) const { return data_.at(i); }

    T* data() noexcept { return data_.data(); }
    const T* data() const noexcept { return data_.data(); }

    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    void fill(const T& val) { std::fill(data_.begin(), data_.end(), val); }

    std::span<T> span() { return std::span<T>(data_.data(), data_.size()); }
    std::span<const T> span() const { return std::span<const T>(data_.data(), data_.size()); }

    typename AlignedVector<T, Alignment>::iterator begin() { return data_.begin(); }
    typename AlignedVector<T, Alignment>::iterator end() { return data_.end(); }
    typename AlignedVector<T, Alignment>::const_iterator begin() const { return data_.begin(); }
    typename AlignedVector<T, Alignment>::const_iterator end() const { return data_.end(); }

private:
    AlignedVector<T, Alignment> data_;
};

} // namespace fvm
