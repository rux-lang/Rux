# Allocator

Allocation contracts and the allocators that meet them.

## Installation

```sh
rux add Rux/Allocator
```

## What it provides

| Module     | Covers                                                                    |
| ---------- | -------------------------------------------------------------------------- |
| `Layout`   | a validated size-and-alignment pair, and the layouts a type or array needs |
| `Error`    | `AllocError`, and the questions worth asking of one                        |
| `Contract` | the `Allocator` interface every allocator here implements                  |
| `System`   | `SystemAllocator`, which asks the operating system for pages               |
| `Box`      | `Box<T>`, one heap value with exactly one owner                             |
| `Arena`    | `Arena`, which advances a pointer and reclaims everything at once           |
| `FixedBuffer` | `FixedBuffer`, which hands out storage the caller already has            |
| `Pool`     | `Pool`, which serves small blocks from per-class free lists                 |

The module names describe where each declaration comes from; they are not part of an import path.

A `Layout` that exists is valid. `Layout::New` refuses an alignment that is not a
power of two, or a size whose rounding would pass the end of the address space — so nothing downstream checks again.
That second case is the one worth naming: it is how a request for a huge array becomes an allocation far smaller than
the caller asked for. Use `LayoutOf<T>()` or `LayoutOfArray<T>(count)` when storage follows a type; neither requires a
sample value, so describing a move-only type consumes nothing.

Four promises hold for every allocator here:

- **The address satisfies the layout.** Always.
- **The contents are undefined.** Nothing here zeroes; the page layer does, because the operating system does.
- **A release must match its allocation** — the same pointer, and an equal layout. The allocator needs the size to
  know what it is taking back.
- **A failure leaves everything alone.** A failed reallocation leaves the original block valid and still the
  caller's.

A zero-sized layout is legal and gives a non-null aligned address that nothing may be read from or written to, and
which must still be released. That is cheaper than a null case every caller has to branch on, and it keeps an empty
collection from being a special shape.

`Arena`, `Pool`, `FixedBuffer`, and `Box<T>` prohibit copying. The first two and the box use canonical destructors;
the fixed buffer owns no storage but still cannot be copied because duplicate bump state could hand out overlapping
blocks. Move an owner explicitly, for example `let destination <- source`. Destroying a non-empty box destroys its
value before returning the allocation, while `TryTake` moves the value to the caller and leaves the box empty.

Canonical construction is `SystemAllocator()`, `Arena(backing, blockSize)`, `Pool(backing, blocksPerChunk)`, and
`FixedBuffer(storage, capacity)`. The `New` methods remain temporary compatibility spellings.

An `Arena` hands out storage by advancing a pointer and takes it all back at once, which is the right shape whenever
a program builds many small things whose lives end together. Blocks come from a backing allocator and each is twice
the size of the last, so a long-lived arena stops going back to the system. `Reset` rewinds every allocation while
keeping the largest block, so a second round of the same work asks the system for nothing.

Borrow a concrete implementation directly for a short-lived interface view, for example
`let view: &var Allocator = system`. Arena, pool, and fixed-buffer handles are storable adapters for allocators whose
mutable state must stay with an owner. A handle stores a raw pointer because references cannot be fields or returned;
it must not outlive or survive a move of its owner.

An arena accepts a release rather than refusing one, so it can stand in for any allocator in generic code that
releases what it takes. Ordinary releases do nothing; the most recent allocation is rewound, which is what lets a
growing collection in an arena avoid wasting every earlier size it passed through.

A `FixedBuffer` is the arena's shape without the arena's appetite: it hands out storage the caller already has,
never asks anyone for more, and reports `OutOfMemory` when the buffer runs out. That makes it the allocator for the
places where allocation must not happen — a fixed working set, a signal-safe path, a target with no allocator
underneath at all. It owns nothing and has no destructor, but its copy operation is prohibited because copying its
state while it is in use would hand out the same storage twice.

A `Pool` is for the other shape — many small values whose lives end at different times and in no particular order.
A request is rounded up to one of five power-of-two classes, each keeping its own free list, so both allocation and
release are a pointer swap with no search and no coalescing. A class that runs dry takes one chunk from a backing
allocator and carves it, so the backing allocator sees one request per chunk rather than one per block. Rounding up
is what pays for the speed: a 17-byte request occupies a 32-byte block, and those 15 bytes are not recoverable until
the chunk is released. A request too large for the largest class, or needing a stricter alignment than a chunk is
taken at, goes straight to the backing allocator — decided from the layout alone, so a release reaches the same
conclusion as its allocation without the pool recording anything per block.

`SystemAllocator` is page-granular, so a small allocation costs a whole page. It is the backing store for an arena or
a pool rather than the allocator a program uses for many small values directly.

## Documentation

<https://rux-lang.dev/docs/api/allocator>

## License

Licensed under the [MIT License](LICENSE.md).
