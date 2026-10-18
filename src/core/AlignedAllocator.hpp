#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace fvm {

template<typename T, std::size_t Alignment = 64>
struct AlignedAllocator {
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template<typename U>
    struct rebind { using other = AlignedAllocator<U, Alignment>; };

    AlignedAllocator() noexcept = default;

    template<typename U>
    constexpr AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type n) {
        if (n > std::numeric_limits<size_type>::max() / sizeof(T))
            throw std::bad_array_new_length();

        void* ptr = nullptr;
#ifdef _WIN32
        ptr = _aligned_malloc(n * sizeof(T), Alignment);
        if (!ptr) throw std::bad_alloc();
#else
        if (posix_memalign(&ptr, Alignment, n * sizeof(T)) != 0)
            throw std::bad_alloc();
#endif
        return static_cast<pointer>(ptr);
    }

    void deallocate(pointer p, size_type) noexcept {
#ifdef _WIN32
        _aligned_free(p);
#else
        free(p);
#endif
    }

    template<typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template<typename U>
    void destroy(U* p) { p->~U(); }

    constexpr bool operator==(const AlignedAllocator&) const noexcept { return true; }
    constexpr bool operator!=(const AlignedAllocator&) const noexcept { return false; }
};

} // namespace fvm
