#pragma once

// The compiler's general-purpose allocator, which `operator new` and `operator delete` are replaced with in the
// compiler executables (`AllocatorNew.cpp`).
//
// A compilation makes tens of millions of small allocations — strings, type trees, IR nodes and instructions — and
// frees most of them again within the same pass. The system heap serializes them on a lock and walks its own
// bookkeeping on every call, which made it the single largest cost of a build. Small requests are served instead from
// per-thread free lists of fixed-size blocks, carved from slabs inside one reserved address range: an allocation or a
// free is a few instructions on the thread's own list, and a block's size is known from the slab it lies in, so blocks
// carry no header. Large requests, and every request on a host that cannot reserve the range, go to `std::malloc`.
//
// Freed blocks go to the freeing thread's list, wherever they were allocated; a list that grows long is handed to a
// shared depot other threads refill from. Memory is never returned to the operating system before the process exits.

#include <cstddef>

namespace Rux::System::Allocator {
/// At least `size` bytes aligned to 16, or null when the system is out of memory.
[[nodiscard]] void *Allocate(std::size_t size) noexcept;

/// Release a block from `Allocate`. Null is ignored.
void Deallocate(void *pointer) noexcept;

/// Whether `pointer` lies in the allocator's slabs rather than in the system heap.
[[nodiscard]] bool OwnsBlock(const void *pointer) noexcept;

/// The usable size of the block `pointer` addresses, for a block `OwnsBlock` accepts.
[[nodiscard]] std::size_t BlockSize(const void *pointer) noexcept;
} // namespace Rux::System::Allocator
