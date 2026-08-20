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

The module names describe where each declaration comes from; they are not part of an import path.

A `Layout` that exists is valid. `Layout::New` is the only way to build one and refuses an alignment that is not a
power of two, or a size whose rounding would pass the end of the address space — so nothing downstream checks again.
That second case is the one worth naming: it is how a request for a huge array becomes an allocation far smaller than
the caller asked for.

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

`Box<T>` implements `Drop`, so it is move-only: handing one to a function or another binding transfers it,
and reading the source afterwards is rejected while compiling. It is *returned* rather than written
through an out-parameter, which every other fallible call here does — ownership is what forces the
difference. The compiler tracks a binding's initialization, and a write through a pointer is invisible to
that tracking, so a box delivered that way would be owned by a binding the compiler believes owns
nothing. The error travels by pointer instead.

An `Arena` hands out storage by advancing a pointer and takes it all back at once, which is the right shape whenever
a program builds many small things whose lives end together. Blocks come from a backing allocator and each is twice
the size of the last, so a long-lived arena stops going back to the system. `Reset` rewinds every allocation while
keeping the largest block, so a second round of the same work asks the system for nothing.

Pass an arena to something expecting an `Allocator` through `Arena::Handle`. Coercing a move-only value to an
interface *moves* it, and an arena moved into an interface value can no longer be reset or released by its owner —
so the handle is a `Copy` borrow that implements the interface while the arena stays where it is. It must not outlive
its arena, and nothing checks that.

An arena accepts a release rather than refusing one, so it can stand in for any allocator in generic code that
releases what it takes. Ordinary releases do nothing; the most recent allocation is rewound, which is what lets a
growing collection in an arena avoid wasting every earlier size it passed through.

`SystemAllocator` is page-granular, so a small allocation costs a whole page. It is the backing store for an arena or
a pool rather than the allocator a program uses for many small values directly.

## License

Licensed under the [MIT License](LICENSE.md).
