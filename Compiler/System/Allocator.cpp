#include "System/Allocator.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if !RUX_OS_WINDOWS
    #include <sys/mman.h>
#endif

namespace Rux::System::Allocator {
namespace {
// Slabs are 64 KiB, carved in order from one reserved range of 256 GiB. Each slab holds blocks of one size class while
// any of them is in use, and the class of every slab is recorded by slab index, which is how a free finds the size of
// a block that has no header.
constexpr std::size_t kSlabShift = 16;
constexpr std::size_t kSlabSize = std::size_t{1} << kSlabShift;
constexpr std::size_t kReserveBytes = std::size_t{1} << 38;
constexpr std::size_t kSlabCount = kReserveBytes >> kSlabShift;
// Address space is made usable this many slabs at a time, so the operating system is asked rarely.
constexpr std::size_t kCommitSlabs = 64;
// Whole free slabs kept ready beyond this many are returned to the operating system, once there are a batch more.
constexpr std::size_t kIdleSlabLimit = (std::size_t{64} << 20) / kSlabSize;
constexpr std::size_t kReleaseBatch = (std::size_t{32} << 20) / kSlabSize;

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

// What a carved slab holds, besides the class of its blocks: nothing, either still committed or returned to the
// operating system, or nothing on its way back to the operating system.
constexpr std::uint8_t kIdleSlab = 0xFD;
constexpr std::uint8_t kReleasingSlab = 0xFE;
constexpr std::uint8_t kReleasedSlab = 0xFF;
static_assert(kClassCount < kIdleSlab);

/// A spin lock that needs no construction, so it works before and after static initialization. It guards only slow
/// paths: taking or freeing a slab and moving free blocks to or from the depot. It yields rather than waiting on the
/// address, which would import the address-wait API set on Windows.
class SpinLock {
public:
    void Lock() noexcept {
        while (flag.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
    }

    [[nodiscard]] bool TryLock() noexcept {
        return !flag.test_and_set(std::memory_order_acquire);
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

// The free blocks of a slab that lie in its class's depot: a list from `head` to `tail` of `count` blocks, given as
// offsets in 16-byte units. A slab with any blocks there is in the depot's list of such slabs, by `previous` and
// `next`.
struct SlabFreeList {
    std::uint16_t head = 0;
    std::uint16_t tail = 0;
    std::uint16_t count = 0;
    std::uint32_t previous = 0;
    std::uint32_t next = 0;
};

constexpr std::uint32_t kNoSlab = 0xFFFFFFFF;
static_assert(kSlabCount < kNoSlab && kSlabSize / 16 <= 0x10000);

// The free lists of the slabs lie at the start of the range, committed along with the slabs they describe.
constexpr std::size_t kFreeListSlabs = kSlabCount * sizeof(SlabFreeList) / kSlabSize;

std::atomic<char *> reservedBase{nullptr};
std::atomic<ReserveState> reserveState{ReserveState::Untried};
std::uint8_t slabClasses[kSlabCount];

// The carved part of the range, guarded by `slabLock`. Slabs from `kFreeListSlabs` to `slabsCarved` hold a class or
// are idle, releasing or released; the hints are the lowest slabs that may be idle or released.
SpinLock slabLock;
std::size_t slabsCarved = kFreeListSlabs;
std::size_t slabsCommitted = kFreeListSlabs;
std::size_t idleSlabs = 0;
std::size_t releasedSlabs = 0;
std::size_t idleHint = 0;
std::size_t releasedHint = 0;
// One release to the operating system at a time.
SpinLock releaseLock;

// Freed blocks no thread is holding, per class, kept by the slab they lie in, so a slab is known to be free the moment
// the last of its blocks arrives.
struct Depot {
    SpinLock lock;
    std::uint32_t slabs = kNoSlab;
};

std::array<Depot, kClassCount> depots;

// A thread's free blocks of one class: a list of exactly `count` blocks, a full spare list of `FlushLimit` blocks, and
// the untouched end of the slab the thread is carving.
struct ClassCache {
    void *head = nullptr;
    std::size_t count = 0;
    void *spare = nullptr;
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

char *SlabAt(const std::size_t index) noexcept {
    return reservedBase.load(std::memory_order_relaxed) + index * kSlabSize;
}

std::uint32_t SlabIndex(const void *block) noexcept {
    const char *base = reservedBase.load(std::memory_order_relaxed);
    return static_cast<std::uint32_t>(static_cast<std::size_t>(static_cast<const char *>(block) - base) >> kSlabShift);
}

SlabFreeList &FreeListOf(const std::uint32_t slab) noexcept {
    return reinterpret_cast<SlabFreeList *>(reservedBase.load(std::memory_order_relaxed))[slab];
}

std::uint16_t OffsetInSlab(const void *block) noexcept {
    const char *base = reservedBase.load(std::memory_order_relaxed);
    const auto offset = static_cast<std::size_t>(static_cast<const char *>(block) - base);
    return static_cast<std::uint16_t>((offset & (kSlabSize - 1)) >> 4);
}

void *BlockAt(const std::uint32_t slab, const std::uint16_t offset) noexcept {
    return SlabAt(slab) + (std::size_t{offset} << 4);
}

/// How many free blocks of `sizeClass` a thread keeps in a list before setting it aside as its spare.
constexpr std::array<std::uint32_t, kClassCount> kFlushLimits = [] {
    std::array<std::uint32_t, kClassCount> limits{};
    for (std::size_t sizeClass = 0; sizeClass < kClassCount; ++sizeClass) {
        const std::uint32_t limit = (std::uint32_t{64} * 1024) / kBlockSizes[sizeClass];
        limits[sizeClass] = limit < 32 ? 32 : limit;
    }
    return limits;
}();

// A refill takes whole slab lists from the depot, and a thread's list must stay within its limit.
static_assert([] {
    for (std::size_t sizeClass = 0; sizeClass < kClassCount; ++sizeClass)
        if (kSlabSize / kBlockSizes[sizeClass] > kFlushLimits[sizeClass])
            return false;
    return true;
}());

std::size_t FlushLimit(const std::size_t sizeClass) noexcept {
    return kFlushLimits[sizeClass];
}

/// Make `count` slabs from `first` readable and writable.
bool Commit(char *first, const std::size_t count) noexcept {
#if RUX_OS_WINDOWS
    return VirtualAlloc(first, count * kSlabSize, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(first, count * kSlabSize, PROT_READ | PROT_WRITE) == 0;
#endif
}

/// Make the free lists of `count` slabs from `first` readable and writable.
bool CommitFreeLists(const std::size_t first, const std::size_t count) noexcept {
    char *base = reservedBase.load(std::memory_order_relaxed);
#if RUX_OS_WINDOWS
    return VirtualAlloc(base + first * sizeof(SlabFreeList), count * sizeof(SlabFreeList), MEM_COMMIT,
                        PAGE_READWRITE) != nullptr;
#else
    // Protection changes whole pages, so the range starts at a slab boundary from the base: a whole number of pages
    // on every host, where a fixed 4 KiB would not be on one with 16 KiB pages such as macOS on AArch64.
    const std::size_t begin = first * sizeof(SlabFreeList) & ~(kSlabSize - 1);
    const std::size_t end = (first + count) * sizeof(SlabFreeList);
    return mprotect(base + begin, end - begin, PROT_READ | PROT_WRITE) == 0;
#endif
}

/// Return `count` slabs from `first` to the operating system, keeping their addresses reserved.
void Decommit(char *first, const std::size_t count) noexcept {
#if RUX_OS_WINDOWS
    static_cast<void>(VirtualFree(first, count * kSlabSize, MEM_DECOMMIT));
#else
    // Mapping fresh inaccessible pages over the slabs drops their contents and their charge at once.
    static_cast<void>(mmap(first, count * kSlabSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
#endif
}

/// The lowest carved slab from `hint` in `state`, or `slabsCarved` when there is none. Called with `slabLock` held.
std::size_t FindSlab(const std::uint8_t state, const std::size_t hint) noexcept {
    const std::size_t from = hint < kFreeListSlabs ? kFreeListSlabs : hint;
    if (from >= slabsCarved)
        return slabsCarved;
    const void *found = std::memchr(slabClasses + from, state, slabsCarved - from);
    return found == nullptr ? slabsCarved
                            : static_cast<std::size_t>(static_cast<const std::uint8_t *>(found) - slabClasses);
}

/// A slab for `sizeClass`: an idle one, else a released one committed again, else a fresh one from the range. Null
/// when the range is used up or cannot be committed. Tells whether idle slabs are left beyond the limit.
char *ReuseOrCarveSlab(const std::size_t sizeClass, bool &idleBeyondLimit) noexcept {
    const LockGuard guard(slabLock);
    idleBeyondLimit = idleSlabs > kIdleSlabLimit + kReleaseBatch;
    std::size_t index = slabsCarved;
    if (idleSlabs != 0) {
        index = FindSlab(kIdleSlab, idleHint);
        if (index != slabsCarved) {
            idleHint = index + 1;
            --idleSlabs;
        }
    }
    if (index == slabsCarved && releasedSlabs != 0) {
        index = FindSlab(kReleasedSlab, releasedHint);
        if (index != slabsCarved) {
            // Commit the released slabs that follow too, as idle ones, so the operating system is asked rarely.
            std::size_t count = 1;
            while (count < kCommitSlabs && index + count < slabsCarved && slabClasses[index + count] == kReleasedSlab)
                ++count;
            if (!Commit(SlabAt(index), count))
                return nullptr;
            releasedSlabs -= count;
            releasedHint = index + count;
            for (std::size_t next = index + 1; next < index + count; ++next)
                slabClasses[next] = kIdleSlab;
            idleSlabs += count - 1;
            if (count > 1 && idleHint > index + 1)
                idleHint = index + 1;
        }
    }
    if (index == slabsCarved) {
        if (slabsCarved == kSlabCount)
            return nullptr;
        if (slabsCarved == slabsCommitted) {
            std::size_t count = kSlabCount - slabsCommitted;
            if (count > kCommitSlabs)
                count = kCommitSlabs;
            if (!Commit(SlabAt(slabsCommitted), count) || !CommitFreeLists(slabsCommitted, count))
                return nullptr;
            slabsCommitted += count;
        }
        ++slabsCarved;
    }
    slabClasses[index] = static_cast<std::uint8_t>(sizeClass);
    return SlabAt(index);
}

/// Return idle slabs beyond the limit to the operating system, the highest first, a run of neighbours at a time. The
/// slabs are marked as releasing while the lock is dropped for the calls, so no thread takes them meanwhile; a thread
/// that finds another releasing leaves the work to it.
void ReleaseIdleSlabs() noexcept {
    if (!releaseLock.TryLock())
        return;

    struct SlabRun {
        std::size_t first;
        std::size_t count;
    };

    std::array<SlabRun, 64> runs{};
    std::size_t runCount = 0;
    std::size_t released = 0;
    {
        const LockGuard guard(slabLock);
        std::size_t excess = idleSlabs > kIdleSlabLimit ? idleSlabs - kIdleSlabLimit : 0;
        for (std::size_t index = slabsCarved; excess != 0 && index-- > kFreeListSlabs;) {
            if (slabClasses[index] != kIdleSlab)
                continue;
            if (runCount != 0 && runs[runCount - 1].first == index + 1) {
                --runs[runCount - 1].first;
                ++runs[runCount - 1].count;
            }
            else if (runCount == runs.size()) {
                break;
            }
            else {
                runs[runCount++] = SlabRun{index, 1};
            }
            slabClasses[index] = kReleasingSlab;
            --excess;
            ++released;
        }
        idleSlabs -= released;
    }
    for (std::size_t run = 0; run < runCount; ++run)
        Decommit(SlabAt(runs[run].first), runs[run].count);
    {
        const LockGuard guard(slabLock);
        for (std::size_t run = 0; run < runCount; ++run) {
            std::memset(slabClasses + runs[run].first, kReleasedSlab, runs[run].count);
            if (runs[run].first < releasedHint)
                releasedHint = runs[run].first;
        }
        releasedSlabs += released;
    }
    releaseLock.Unlock();
}

/// A slab for `sizeClass`, as `ReuseOrCarveSlab` finds one. Idle slabs beyond the limit are returned to the
/// operating system only here, while the program still allocates, so the memory freed as it ends costs no calls.
char *TakeSlab(const std::size_t sizeClass) noexcept {
    bool idleBeyondLimit = false;
    char *slab = ReuseOrCarveSlab(sizeClass, idleBeyondLimit);
    if (idleBeyondLimit)
        ReleaseIdleSlabs();
    return slab;
}

/// Make `count` slabs whose blocks are all free idle, ready for any class.
void FreeSlabs(const std::uint32_t *slabs, const std::size_t count) noexcept {
    const LockGuard guard(slabLock);
    for (std::size_t index = 0; index < count; ++index) {
        slabClasses[slabs[index]] = kIdleSlab;
        if (slabs[index] < idleHint)
            idleHint = slabs[index];
    }
    idleSlabs += count;
}

void UnlinkSlab(Depot &depot, const SlabFreeList &list) noexcept {
    if (list.previous == kNoSlab)
        depot.slabs = list.next;
    else
        FreeListOf(list.previous).next = list.next;
    if (list.next != kNoSlab)
        FreeListOf(list.next).previous = list.previous;
}

/// Consecutive blocks of one slab in a list handed to the depot, linked to each other already.
struct Run {
    void *first;
    void *last;
    std::uint32_t slab;
    std::uint32_t length;
};

constexpr std::size_t kRunBatch = 64;

/// Add `count` runs of `sizeClass` to their slabs' lists in the depot, then free the slabs that became whole.
void DepositRuns(const std::size_t sizeClass, const Run *runs, const std::size_t count) noexcept {
    const std::size_t capacity = kSlabSize / kBlockSizes[sizeClass];
    std::array<std::uint32_t, kRunBatch> freed{};
    std::size_t freedCount = 0;
    auto &depot = depots[sizeClass];
    {
        const LockGuard guard(depot.lock);
        for (std::size_t index = 0; index < count; ++index) {
            const Run &run = runs[index];
            SlabFreeList &list = FreeListOf(run.slab);
            if (list.count == 0) {
                NextBlock(run.last) = nullptr;
                list.tail = OffsetInSlab(run.last);
                list.previous = kNoSlab;
                list.next = depot.slabs;
                if (depot.slabs != kNoSlab)
                    FreeListOf(depot.slabs).previous = run.slab;
                depot.slabs = run.slab;
            }
            else {
                NextBlock(run.last) = BlockAt(run.slab, list.head);
            }
            list.head = OffsetInSlab(run.first);
            list.count = static_cast<std::uint16_t>(list.count + run.length);
            if (list.count == capacity) {
                UnlinkSlab(depot, list);
                list.count = 0;
                freed[freedCount++] = run.slab;
            }
        }
    }
    if (freedCount != 0)
        FreeSlabs(freed.data(), freedCount);
}

/// Hand a null-terminated list of blocks of `sizeClass` to the depot. Its blocks are gathered by slab first, so the
/// depot is updated once per slab rather than once per block.
void PushList(const std::size_t sizeClass, void *list) noexcept {
    // Which run of the batch last took a block of a slab, by the slab's low bits; a stale or colliding entry only
    // starts another run of the same slab.
    constexpr std::size_t kSlots = 256;
    std::array<std::uint8_t, kSlots> runOfSlab;
    runOfSlab.fill(0xFF);
    std::array<Run, kRunBatch> runs;
    std::size_t count = 0;
    Run *current = nullptr;
    for (void *block = list; block != nullptr;) {
        void *next = NextBlock(block);
        const std::uint32_t slab = SlabIndex(block);
        if (current == nullptr || current->slab != slab) {
            const std::uint8_t slot = runOfSlab[slab % kSlots];
            if (slot < count && runs[slot].slab == slab) {
                current = &runs[slot];
            }
            else {
                if (count == kRunBatch) {
                    DepositRuns(sizeClass, runs.data(), count);
                    runOfSlab.fill(0xFF);
                    count = 0;
                }
                runOfSlab[slab % kSlots] = static_cast<std::uint8_t>(count);
                runs[count] = Run{block, block, slab, 1};
                current = &runs[count++];
                block = next;
                continue;
            }
        }
        // Blocks already walked are relinked only, so `next` stays the list's own successor.
        NextBlock(current->last) = block;
        current->last = block;
        ++current->length;
        block = next;
    }
    if (count != 0)
        DepositRuns(sizeClass, runs.data(), count);
}

/// Free blocks of `sizeClass` from the depot, the whole lists of a few slabs, and their number; null when the depot
/// is empty. They are at most `FlushLimit` blocks, which a slab's list alone never exceeds: a slab whose blocks are all
/// in the depot is not in it. The lists are joined after the lock is dropped, since that touches a block of each.
void *PopList(const std::size_t sizeClass, std::size_t &length) noexcept {
    constexpr std::size_t kMaxSlabs = 16;
    const std::size_t limit = FlushLimit(sizeClass);
    std::array<std::pair<void *, void *>, kMaxSlabs> lists;
    std::size_t listCount = 0;
    length = 0;
    {
        auto &depot = depots[sizeClass];
        const LockGuard guard(depot.lock);
        while (length < limit / 2 && listCount < kMaxSlabs && depot.slabs != kNoSlab) {
            const std::uint32_t slab = depot.slabs;
            SlabFreeList &list = FreeListOf(slab);
            if (length + list.count > limit)
                break;
            depot.slabs = list.next;
            if (list.next != kNoSlab)
                FreeListOf(list.next).previous = kNoSlab;
            lists[listCount++] = {BlockAt(slab, list.head), BlockAt(slab, list.tail)};
            length += list.count;
            list.count = 0;
        }
    }
    if (listCount == 0)
        return nullptr;
    for (std::size_t index = 1; index < listCount; ++index)
        NextBlock(lists[index - 1].second) = lists[index].first;
    return lists[0].first;
}

/// Hands a finished thread's free blocks, and the untouched end of each slab it was carving, to the depot, so the
/// threads that remain reuse them and the slabs can be freed whole.
struct CacheFlusher {
    CacheFlusher() = default;
    CacheFlusher(const CacheFlusher &) = delete;
    CacheFlusher &operator=(const CacheFlusher &) = delete;

    ~CacheFlusher() {
        for (std::size_t sizeClass = 0; sizeClass < kClassCount; ++sizeClass) {
            auto &cache = threadCache.classes[sizeClass];
            if (cache.head != nullptr)
                PushList(sizeClass, cache.head);
            if (cache.spare != nullptr)
                PushList(sizeClass, cache.spare);
            const std::size_t blockSize = kBlockSizes[sizeClass];
            if (cache.bump != nullptr && static_cast<std::size_t>(cache.end - cache.bump) >= blockSize) {
                const std::size_t length = static_cast<std::size_t>(cache.end - cache.bump) / blockSize;
                for (std::size_t index = 0; index < length; ++index)
                    NextBlock(cache.bump + index * blockSize) =
                        index + 1 < length ? cache.bump + (index + 1) * blockSize : nullptr;
                PushList(sizeClass, cache.bump);
            }
            cache = ClassCache{};
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

/// The slow path of a small allocation: refill the thread's list from its spare, the depot or a slab.
void *Refill(const std::size_t sizeClass, ClassCache &cache) noexcept {
    // Constructing the flusher registers its destructor for this thread, which runs when the thread ends.
    static_cast<void>(&threadFlusher);
    if (void *spare = cache.spare) {
        cache.spare = nullptr;
        cache.head = NextBlock(spare);
        cache.count = FlushLimit(sizeClass) - 1;
        return spare;
    }
    std::size_t length = 0;
    if (void *list = PopList(sizeClass, length)) {
        cache.head = NextBlock(list);
        cache.count = length - 1;
        return list;
    }
    if (!Reserve())
        return nullptr;
    char *slab = TakeSlab(sizeClass);
    if (slab == nullptr)
        return nullptr;
    const std::size_t blockSize = kBlockSizes[sizeClass];
    cache.bump = slab + blockSize;
    cache.end = slab + (kSlabSize / blockSize) * blockSize;
    return slab;
}

/// The slow path of a small free: the thread's list is full, so it becomes the spare and a full spare goes to the
/// depot.
void SetListAside(const std::size_t sizeClass, ClassCache &cache) noexcept {
    static_cast<void>(&threadFlusher);
    if (cache.spare != nullptr)
        PushList(sizeClass, cache.spare);
    cache.spare = cache.head;
    cache.head = nullptr;
    cache.count = 0;
}
} // namespace

void *Allocate(std::size_t size) noexcept {
    if (size > kMaxSmall)
        return std::malloc(size);
    const std::size_t sizeClass = kClassOfGranule[(size + 15) >> 4];
    auto &cache = threadCache.classes[sizeClass];
    if (void *block = cache.head) {
        cache.head = NextBlock(block);
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
    return kBlockSizes[slabClasses[SlabIndex(pointer)]];
}

void Deallocate(void *pointer) noexcept {
    if (pointer == nullptr)
        return;
    if (!OwnsBlock(pointer)) {
        std::free(pointer);
        return;
    }
    const std::size_t sizeClass = slabClasses[SlabIndex(pointer)];
    auto &cache = threadCache.classes[sizeClass];
    if (cache.count == FlushLimit(sizeClass)) [[unlikely]]
        SetListAside(sizeClass, cache);
    NextBlock(pointer) = cache.head;
    cache.head = pointer;
    ++cache.count;
}

SlabUsage CurrentSlabUsage() noexcept {
    const LockGuard guard(slabLock);
    return SlabUsage{
        .carvedBytes = (slabsCarved - kFreeListSlabs) * kSlabSize,
        .idleBytes = idleSlabs * kSlabSize,
        .releasedBytes = releasedSlabs * kSlabSize,
    };
}
} // namespace Rux::System::Allocator
