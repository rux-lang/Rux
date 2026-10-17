// Replaces the global `operator new` and `operator delete` with the compiler allocator (`Allocator.h`). Linked
// directly into each compiler executable, never into a library, so embedding the compiler keeps the host's allocator.

#include "System/Allocator.h"
#include "Target/Platform.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
using Rux::System::Allocator::Allocate;
using Rux::System::Allocator::Deallocate;

// Blocks are 16-byte aligned; a stricter alignment goes to the C runtime, which must then also free it.
constexpr std::size_t kNaturalAlignment = 16;

void *AllocateOrThrow(const std::size_t size) {
    if (void *pointer = Allocate(size))
        return pointer;
    throw std::bad_alloc();
}

void *AllocateAligned(const std::size_t size, const std::align_val_t alignment) noexcept {
    const auto align = static_cast<std::size_t>(alignment);
    if (align <= kNaturalAlignment)
        return Allocate(size);
#if RUX_OS_WINDOWS
    return _aligned_malloc(size == 0 ? 1 : size, align);
#else
    const std::size_t rounded = size == 0 ? align : (size + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
#endif
}

void DeallocateAligned(void *pointer, const std::align_val_t alignment) noexcept {
    if (static_cast<std::size_t>(alignment) <= kNaturalAlignment) {
        Deallocate(pointer);
        return;
    }
#if RUX_OS_WINDOWS
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void *AllocateAlignedOrThrow(const std::size_t size, const std::align_val_t alignment) {
    if (void *pointer = AllocateAligned(size, alignment))
        return pointer;
    throw std::bad_alloc();
}
} // namespace

void *operator new(const std::size_t size) {
    return AllocateOrThrow(size);
}

void *operator new[](const std::size_t size) {
    return AllocateOrThrow(size);
}

void *operator new(const std::size_t size, const std::nothrow_t &) noexcept {
    return Allocate(size);
}

void *operator new[](const std::size_t size, const std::nothrow_t &) noexcept {
    return Allocate(size);
}

void *operator new(const std::size_t size, const std::align_val_t alignment) {
    return AllocateAlignedOrThrow(size, alignment);
}

void *operator new[](const std::size_t size, const std::align_val_t alignment) {
    return AllocateAlignedOrThrow(size, alignment);
}

void *operator new(const std::size_t size, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    return AllocateAligned(size, alignment);
}

void *operator new[](const std::size_t size, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    return AllocateAligned(size, alignment);
}

void operator delete(void *pointer) noexcept {
    Deallocate(pointer);
}

void operator delete[](void *pointer) noexcept {
    Deallocate(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept {
    Deallocate(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept {
    Deallocate(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept {
    Deallocate(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept {
    Deallocate(pointer);
}

void operator delete(void *pointer, const std::align_val_t alignment) noexcept {
    DeallocateAligned(pointer, alignment);
}

void operator delete[](void *pointer, const std::align_val_t alignment) noexcept {
    DeallocateAligned(pointer, alignment);
}

void operator delete(void *pointer, std::size_t, const std::align_val_t alignment) noexcept {
    DeallocateAligned(pointer, alignment);
}

void operator delete[](void *pointer, std::size_t, const std::align_val_t alignment) noexcept {
    DeallocateAligned(pointer, alignment);
}

void operator delete(void *pointer, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    DeallocateAligned(pointer, alignment);
}

void operator delete[](void *pointer, const std::align_val_t alignment, const std::nothrow_t &) noexcept {
    DeallocateAligned(pointer, alignment);
}
