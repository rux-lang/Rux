# macOS

macOS platform bindings: the raw Darwin syscall interface, with no libSystem in between.

## Installation

```sh
rux add Rux/macOS
```

## What it provides

| Module      | Covers                                                                                |
| ----------- | ------------------------------------------------------------------------------------- |
| `Types`     | the Darwin ABI's own types: `ProcessId`, `FileDescriptor`, `Timespec`, `Timeval`       |
| `Errors`    | the errno numbers, `CurrentErrno`, and the `Normalize` / `IsError` / `Errno` fold      |
| `LibSystem` | every libSystem entry point this package binds, under the symbol it exports            |
| `File`      | `Read`, `Write`, `Close`, `OpenAt`, `Lseek`, `Fsync`, `Ftruncate`, `UnlinkAt`, `MkdirAt`, `RenameAt`, `Dup2`, `Pipe`, the open flags, and the three standard descriptors |
| `Memory`    | `Mmap`, `Munmap`, `Mprotect`, `Madvise`, and the protection, mapping and advice flags   |
| `Clock`     | `ClockGetTime`, `GetTimeOfDay`, `Nanosleep`, and the clock identifiers                  |
| `Entropy`   | `GetRandom`, which cannot fail and has no short count                                   |
| `Process`   | `Exit` and `GetPid`                                                                     |
| `Dynamic`   | the `RTLD_*` flags; the loader calls themselves are in `LibSystem`                      |

The module names describe where each declaration comes from; they are not part of an import path.

**Apple does not support calling the kernel directly.** The trap numbers are private, they have changed between
releases, and on Apple Silicon the trap interface is not reachable from ordinary code at all. libSystem is the ABI,
and this package binds it — which is what makes it different in kind from `Rux/Linux` and `Rux/FreeBSD`, whose
kernels publish a stable numbering.

libSystem reports failure the way C does, with `-1` and a positive `errno` reached through an accessor. `Normalize`
folds those two into the one negative-error convention the other platform packages use, so `IsError` and `Errno` read
the same on all three.

Nothing here is interchangeable with the other platform packages. `AT_FDCWD` is `-2` on Darwin and `-100` elsewhere;
monotonic is clock 6 here, 1 on Linux, 4 on FreeBSD; `RTLD_LOCAL` is 4 and `RTLD_GLOBAL` is 8, where both other
systems use 0 and 256. A value carried across compiles and means something else.

There is no `Brk`: Darwin's `brk` and `sbrk` have been unavailable to 64-bit code from the start. There is no
`Pipe2`: Darwin has only `pipe`, so close-on-exec must be set afterwards. And `Fsync` is weaker here than elsewhere —
it does not wait for the device to commit, which `F_FULLFSYNC` through `fcntl` is what does.

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
