#include "System/Allocator.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

#if !RUX_OS_WINDOWS
    #include <sys/mman.h>
#endif

namespace Rux::System::Allocator {
namespace {
// Slabs are 64 KiB, carved in order from one reserved range of 256 GiB. Each slab holds blocks of one size class, and
// the class of every slab is recorded by slab index, which is how a free finds the size of a block that has no header.
constexpr std::size_t kSlabShift = 16;
constexpr std::size_t kSlabSize = std::size_t{1} << kSlabShift;
constexpr std::size_t kReserveBytes = std::size_t{1} << 38;
constexpr std::size_t kSlabCount = kReserveBytes >> kSlabShift;
// Address space is made usable this many slabs at a time, so the operating system is asked rarely.
constexpr std::size_t kCommitSlabs = 64;

// Block sizes grow in steps of 16 bytes to 256, then by an eighth of each power of two, up to 8 KiB. Every size is a
// multiple of 16, so every block of a slab is 16-byte aligned.
constexpr std::size_t kMaxSmall = 8192;
constexpr std::size_t kClassCount = 56;

constexpr std::array<std::uint32_t, kClassCount> kBlockSizes = [] {
    std::array<std::uint32_t, kClassCount> sizes{};
    std::size_t index = 0;
    for (std::uint32_t size = 16; size <= 256; size += 16)
        sizes[index++] = size;
    for (std::uint32_t base = 256; base < kMaxSmall; base *= 2)
        for (std::uint32_t step = 1; step <= 8; ++step)
            sizes[index++] = base + step * (base / 8);
    return sizes;
}();

// The class of every request size up to kMaxSmall, indexed by the size rounded up to 16 bytes.
constexpr std::array<std::uint8_t, kMaxSmall / 16 + 1> kClassOfGranule = [] {
    std::array<std::uint8_t, kMaxSmall / 16 + 1> classes{};
    std::size_t sizeClass = 0;
    for (std::size_t granule = 0; granule < classes.size(); ++granule) {
        while (kBlockSizes[sizeClass] < granule * 16)
            ++sizeClass;
        classes[granule] = static_cast<std::uint8_t>(sizeClass);
    }
    return classes;
}();

static_assert(kBlockSizes[kClassCount - 1] == kMaxSmall);

/// A spin lock that needs no construction, so it works before and after static initialization. It guards only slow
/// paths: carving a slab and moving a free list to or from the depot. It yields rather than waiting on the address,
/// which would import the address-wait API set on Windows.
class SpinLock {
public:
    void Lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
    }

    void Unlock() noexcept {
        flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag flag;
};

class LockGuard {
public:
    explicit LockGuard(SpinLock &inputLock) noexcept
        : lock(inputLock) {
        lock.Lock();
    }

    ~LockGuard() {
        lock.Unlock();
    }

    LockGuard(const LockGuard &) = delete;
    LockGuard &operator=(const LockGuard &) = delete;

private:
    SpinLock &lock;
};

enum class ReserveState : int {
    Untried,
    Reserving,
    Ready,
    Unavailable,
};

std::atomic<char *> reservedBase{nullptr};
std::atomic<ReserveState> reserveState{ReserveState::Untried};
std::uint8_t slabClasses[kSlabCount];
SpinLock carveLock;
std::size_t slabsCarved = 0;
std::size_t slabsCommitted = 0;

// Freed blocks no thread is holding, per class: a list of chains, each chain a free list whose first block also links
// to the next chain.
struct Depot {
    SpinLock lock;
    void *chains = nullptr;
};

std::array<Depot, kClassCount> depots;

struct ClassCache {
    void *head = nullptr;
    std::size_t count = 0;
    char *bump = nullptr;
    char *end = nullptr;
};

struct ThreadCache {
    std::array<ClassCache, kClassCount> classes{};
};

thread_local ThreadCache threadCache;

void *&NextBlock(void *block) noexcept {
    return *static_cast<void **>(block);
}

void *&NextChain(void *block) noexcept {
    return static_cast<void **>(block)[1];
}

/// How many free blocks of `sizeClass` a thread keeps before handing its list to the depot.
constexpr std::array<std::uint32_t, kClassCount> kFlushLimits = [] {
    std::array<std::uint32_t, kClassCount> limits{};
    for (std::size_t sizeClass = 0; sizeClass < kClassCount; ++sizeClass) {
        const std::uint32_t limit = (std::uint32_t{256} * 1024) / kBlockSizes[sizeClass];
        limits[sizeClass] = limit < 32 ? 32 : limit;
    }
    return limits;
}();

std::size_t FlushLimit(const std::size_t sizeClass) noexcept {
    return kFlushLimits[sizeClass];
}

void PushChain(const std::size_t sizeClass, void *chain) noexcept {
    auto &depot = depots[sizeClass];
    const LockGuard guard(depot.lock);
    NextChain(chain) = depot.chains;
    depot.chains = chain;
}

void *PopChain(const std::size_t sizeClass) noexcept {
    auto &depot = depots[sizeClass];
    const LockGuard guard(depot.lock);
    void *chain = depot.chains;
    if (chain != nullptr)
        depot.chains = NextChain(chain);
    return chain;
}

/// Hands a finished thread's free blocks to the depot, so the threads that remain reuse them.
struct CacheFlusher {
    CacheFlusher() = default;
    CacheFlusher(const CacheFlusher &) = delete;
    CacheFlusher &operator=(const CacheFlusher &) = delete;

    ~CacheFlusher() {
        for (std::size_t sizeClass = 0; sizeClass < kClassCount; ++sizeClass) {
            auto &cache = threadCache.classes[sizeClass];
            if (cache.head != nullptr)
                PushChain(sizeClass, cache.head);
            cache.head = nullptr;
            cache.count = 0;
        }
    }
};

thread_local CacheFlusher threadFlusher;

bool Reserve() noexcept {
    auto state = reserveState.load(std::memory_order_acquire);
    while (state != ReserveState::Ready) {
        if (state == ReserveState::Unavailable)
            return false;
        if (state == ReserveState::Untried &&
            reserveState.compare_exchange_strong(state, ReserveState::Reserving, std::memory_order_acq_rel)) {
#if RUX_OS_WINDOWS
            void *base = VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE, PAGE_NOACCESS);
#else
            void *base = mmap(nullptr, kReserveBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED)
                base = nullptr;
#endif
            reservedBase.store(static_cast<char *>(base), std::memory_order_release);
            reserveState.store(base != nullptr ? ReserveState::Ready : ReserveState::Unavailable,
                               std::memory_order_release);
            return base != nullptr;
        }
        if (state == ReserveState::Reserving)
            std::this_thread::yield();
        state = reserveState.load(std::memory_order_acquire);
    }
    return true;
}

/// Make `count` slabs from `first` readable and writable.
bool Commit(char *first, const std::size_t count) noexcept {
#if RUX_OS_WINDOWS
    return VirtualAlloc(first, count * kSlabSize, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(first, count * kSlabSize, PROT_READ | PROT_WRITE) == 0;
#endif
}

/// A fresh slab for `sizeClass`, or null when the reserved range is used up or cannot be committed.
char *CarveSlab(const std::size_t sizeClass) noexcept {
    char *base = reservedBase.load(std::memory_order_acquire);
    const LockGuard guard(carveLock);
    if (slabsCarved == kSlabCount)
        return nullptr;
    if (slabsCarved == slabsCommitted) {
        std::size_t count = kSlabCount - slabsCommitted;
        if (count > kCommitSlabs)
            count = kCommitSlabs;
        if (!Commit(base + slabsCommitted * kSlabSize, count))
            return nullptr;
        slabsCommitted += count;
    }
    const std::size_t index = slabsCarved++;
    slabClasses[index] = static_cast<std::uint8_t>(sizeClass);
    return base + index * kSlabSize;
}

/// The slow path of a small allocation: refill the thread's list from the depot or a new slab.
void *Refill(const std::size_t sizeClass, ClassCache &cache) noexcept {
    // Constructing the flusher registers its destructor for this thread, which runs when the thread ends.
    static_cast<void>(&threadFlusher);
    if (void *chain = PopChain(sizeClass)) {
        cache.head = NextBlock(chain);
        cache.count = FlushLimit(sizeClass) / 2;
        return chain;
    }
    if (!Reserve())
        return nullptr;
    char *slab = CarveSlab(sizeClass);
    if (slab == nullptr)
        return nullptr;
    const std::size_t blockSize = kBlockSizes[sizeClass];
    cache.bump = slab + blockSize;
    cache.end = slab + (kSlabSize / blockSize) * blockSize;
    return slab;
}
} // namespace

void *Allocate(std::size_t size) noexcept {
    if (size > kMaxSmall)
        return std::malloc(size);
    const std::size_t sizeClass = kClassOfGranule[(size + 15) >> 4];
    auto &cache = threadCache.classes[sizeClass];
    if (void *block = cache.head) {
        cache.head = NextBlock(block);
        if (cache.count != 0)
            --cache.count;
        return block;
    }
    const std::size_t blockSize = kBlockSizes[sizeClass];
    if (cache.bump != nullptr && static_cast<std::size_t>(cache.end - cache.bump) >= blockSize) {
        void *block = cache.bump;
        cache.bump += blockSize;
        return block;
    }
    if (void *block = Refill(sizeClass, cache))
        return block;
    return std::malloc(size == 0 ? 1 : size);
}

bool OwnsBlock(const void *pointer) noexcept {
    const char *base = reservedBase.load(std::memory_order_relaxed);
    const auto *address = static_cast<const char *>(pointer);
    return base != nullptr && address >= base && address < base + kReserveBytes;
}

std::size_t BlockSize(const void *pointer) noexcept {
    const char *base = reservedBase.load(std::memory_order_relaxed);
    const auto offset = static_cast<std::size_t>(static_cast<const char *>(pointer) - base);
    return kBlockSizes[slabClasses[offset >> kSlabShift]];
}

void Deallocate(void *pointer) noexcept {
    if (pointer == nullptr)
        return;
    if (!OwnsBlock(pointer)) {
        std::free(pointer);
        return;
    }
    const char *base = reservedBase.load(std::memory_order_relaxed);
    const std::size_t sizeClass =
        slabClasses[static_cast<std::size_t>(static_cast<char *>(pointer) - base) >> kSlabShift];
    auto &cache = threadCache.classes[sizeClass];
    NextBlock(pointer) = cache.head;
    cache.head = pointer;
    if (++cache.count > FlushLimit(sizeClass)) {
        PushChain(sizeClass, cache.head);
        cache.head = nullptr;
        cache.count = 0;
    }
}
} // namespace Rux::System::Allocator
