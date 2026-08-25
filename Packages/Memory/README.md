# Memory

Memory management functions: raw allocation and the block operations that go with it.

Every function dispatches on `#target.os` at compile time, so a build carries exactly one platform's implementation — `mmap`/`munmap` on Linux, macOS, and the BSDs, and the process heap on Windows.

## Installation

```sh
rux add Rux/Memory
```

## What it provides

| Function         | Purpose                                                          |
| ---------------- | ---------------------------------------------------------------- |
| `Alloc`          | Allocate an uninitialized block                                  |
| `Realloc`        | Resize a block, preserving its contents                          |
| `Free`           | Release a block                                                  |
| `Copy`           | Copy bytes between blocks that do not overlap                    |
| `Move`           | Copy bytes between blocks that may overlap in any way            |
| `Set`            | Fill a block with a byte value                                   |
| `Zero`           | Fill a block with zero                                           |
| `Compare`        | Order two blocks lexicographically, returning an `Ordering`      |
| `Equal`          | Whether two blocks hold the same bytes                           |
| `MismatchOffset` | Where two blocks first differ, or their length when they do not  |
| `Find`           | Where a byte first occurs, or the length when it does not        |

| `PageSize` / `PageAlign` | The system page size, and rounding a length to it                        |
| `PageAllocate` / `PageRelease` / `PageProtect` | Address space at the granularity the system hands it out in |

**Zero length is always legal, and null is legal with it.** `Copy(null, null, 0)` is defined and does nothing. That
is deliberately not C's rule, where passing null to `memcpy` is undefined even for a zero length — a rule that turns
an ordinary empty case into a trap and makes every caller guard.

`Copy` may be read as copying byte zero first, so a destination overlapping the source ahead of it reads bytes the
copy has already overwritten. `Move` is defined for every overlap and chooses its direction accordingly.

The page layer is what sits under an allocator, not an allocator itself. Nothing there tracks what it gave out or
remembers a length; a caller releases exactly what it took. Failure is a `PageError` rather than an errno, a
`GetLastError` code or a sentinel address — one set of reasons across four systems that report three different ways.

Windows differs in kind: it separates reserving address space from committing storage to it, and `PageAllocate`
always does both, which is what every other system does and what an allocator expects. A caller wanting a large
reservation cheaply has to reach for `Rux/Windows` directly.

`PageSize` asks the system where the system will answer — `GetSystemInfo` on Windows, `getpagesize` on Darwin, which
is not 4 KiB on Apple Silicon. Linux and FreeBSD keep the answer in the auxiliary vector rather than behind a system
call and this package calls no libc there, so those report the standard 4 KiB and a kernel configured for larger
pages is under-reported. That cannot make an allocation wrong, since every call rounds the length up in the system
rather than here; it can only make a caller ask for more than it needed.

None of the comparisons is constant-time: each stops at the first difference, so none may compare a secret.

## Value and ownership model

`PageError` is a structural `Copy` value. Every address remains a raw pointer because this package exposes byte
ranges, nullable allocation failure, stored addresses, pointer arithmetic, and explicit release—the operations for
which references cannot prove validity or ownership. `Alloc` and `PageAllocate` transfer ownership to the caller;
`Free` and `PageRelease` end it. No destructor runs automatically, so never copy an owning address with the intent of
freeing both copies. Scalar output slots are also raw: pass writable addresses such as `@result` and obey the
function's non-null contract.

> **Changed in this release.** `Compare` now returns an `Ordering`, which is what this table always claimed it did;
> the old first-difference offset is `MismatchOffset`, and `Equal` is what most callers of the old `Compare` wanted.
> `Set` takes its arguments in `memset` order — destination, value, length — where it previously took the length
> before the value, and its value is a `byte` rather than an `int32`, so a mistaken call fails to compile.

## Example

```rux
import Memory::{ Alloc, Free, Zero };

var buffer = Alloc(1024);
if buffer != null {
    Zero(buffer, 1024);
    Free(buffer);
}
```

## Documentation

<https://rux-lang.dev/docs/api/memory>

## License

Licensed under the [MIT License](LICENSE.md).
