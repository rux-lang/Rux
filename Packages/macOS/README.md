# macOS

macOS platform bindings: the Darwin ABI as libSystem publishes it, which on this system is the only interface
Apple supports.

## Installation

```sh
rux add Rux/macOS
```

## What it provides

| Module      | Covers                                                                                |
| ----------- | ------------------------------------------------------------------------------------- |
| `Types`     | the Darwin ABI's own types: `ProcessId`, `FileDescriptor`, `FileMode`, `UserId`, `Timespec`, `Timeval` |
| `Errors`    | the errno numbers, `CurrentErrno`, and the `Normalize` / `IsError` / `Errno` fold      |
| `LibSystem` | every libSystem entry point this package binds, under the symbol it exports            |
| `File`      | `Read`, `Write`, `Close`, `OpenAt`, `Lseek`, `Fsync`, `Ftruncate`, `UnlinkAt`, `MkdirAt`, `RenameAt`, `Dup2`, `Pipe`, the open flags, and the three standard descriptors |
| `Memory`    | `Mmap`, `Munmap`, `Mprotect`, `Madvise`, `QueryPageSize`, and the protection, mapping and advice flags |
| `Clock`     | `ClockGetTime`, `ClockGetResolution`, `GetTimeOfDay`, `Nanosleep`, and the clock identifiers |
| `Entropy`   | `GetRandom`, which cannot fail and has no short count                                   |
| `Process`   | `Exit` and `GetPid`                                                                     |
| `Dynamic`   | the `RTLD_*` flags; the loader calls themselves are in `LibSystem`                      |

The module names describe where each declaration comes from; they are not part of an import path.

**Apple does not support calling the kernel directly.** The trap numbers are private, they have changed between releases, and on Apple Silicon the trap interface is not reachable from ordinary code at all. libSystem is the ABI, and this package binds it — which is what makes it different in kind from `Rux/Linux` and `Rux/FreeBSD`, whose kernels publish a stable numbering.

libSystem reports failure the way C does, with `-1` and a positive `errno` reached through an accessor. `Normalize` folds those two into the one negative-error convention the other platform packages use, so `IsError` and `Errno` read the same on all three.

Nothing here is interchangeable with the other platform packages. `AT_FDCWD` is `-2` on Darwin and `-100` elsewhere; monotonic is clock 6 here, 1 on Linux, 4 on FreeBSD; `RTLD_LOCAL` is 4 and `RTLD_GLOBAL` is 8, where both other systems use 0 and 256. A value carried across compiles and means something else.

There is no `Brk`: Darwin's `brk` and `sbrk` have been unavailable to 64-bit code from the start. There is no `Pipe2`: Darwin has only `pipe`, so close-on-exec must be set afterwards. And `Fsync` is weaker here than elsewhere — it does not wait for the device to commit, which `F_FULLFSYNC` through `fcntl` is what does.

## What is tested

`Tests/Packages/macOS/Descriptors` covers descriptor numbering, short reads and writes, end of file, size boundaries and the native error for each way of misusing a descriptor or a name. `Tests/Packages/macOS/Mapping` covers anonymous mappings, protection changes, advice, the requests the kernel refuses, and both edges of the reserved error-result window. `Tests/Packages/macOS/Syscall` covers the wrapper surface and the shared POSIX contract.

All of it compiles for both architectures, and none of it has ever run on a host that is not macOS. Until it does, the syscall numbers, flag values and errno constants here rest on published documentation rather than on a passing test.

## The shared POSIX contract

Nothing here is interchangeable with `Rux/Linux` or `Rux/FreeBSD`, but the three are *spelled* the same, which is a different claim and a deliberate one. An equivalent call has the same wrapper name, the same parameter names and the same parameter shapes on all three systems, so a consumer such as [`Rux/FileSystem`](../FileSystem) or [`Rux/Time`](../Time) writes one call site instead of a `when #target.os` ladder around three names for one idea. `Tests/Unit/PlatformBindingContractTests.cpp` compares the three packages' parsed sources and fails when they drift apart.

The values are the opposite: every constant is this system's own, and the same file asserts that the ones a reader might carry across really do differ.

## Values and pointers

Darwin records such as `Timespec` and `Timeval` are structural values and copy by value, and every call that reads or fills one takes its address: `GetTimeOfDay(@time)`, `ClockGetTime(ClockMonotonic, @time)`. That is the convention on all three POSIX packages. It is not the prettiest form available here — libSystem's C declarations would accept a reference, and this package used them until the divergence showed up as target branches in `Rux/Time` — but one shape across three systems is worth more than a borrow on one of them.

libSystem declarations retain raw pointers for nullable parameters, byte buffers, loader handles, and OS-owned storage. These pointers do not express ownership; use the documented close or unmap operation, or prefer `Rux/Io` and `Rux/Memory` for managed interfaces.

## Platform

macOS only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os == .macOS {
    import macOS::{ StdOut, Write };
}
```

## Documentation

<https://rux-lang.dev/docs/api/macos>

## License

Licensed under the [MIT License](LICENSE.md).
