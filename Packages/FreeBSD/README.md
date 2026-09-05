# FreeBSD

FreeBSD platform bindings: the raw syscall interface, with no libc in between.

## Installation

```sh
rux add Rux/FreeBSD
```

## What it provides

| Module    | Covers                                                                                  |
| --------- | --------------------------------------------------------------------------------------- |
| `Types`   | the kernel ABI's own types: `ProcessId`, `FileDescriptor`, `FileOffset`, `Timespec`      |
| `Errors`  | the errno numbers, and `IsError` / `Errno` for reading a raw result                      |
| `Syscall` | `Syscall0` through `Syscall6` as inline assembly, the call numbers, and `SignExtendFd`   |
| `File`    | `Read`, `Write`, `Close`, `OpenAt`, `Lseek`, `Fsync`, `Ftruncate`, `UnlinkAt`, `MkdirAt`, `RenameAt`, `Fstat`, `Dup2`, `Pipe2`, the open flags, and the three standard descriptors |
| `Memory`  | `Mmap`, `Munmap`, `Brk`, `Mprotect`, `Madvise`, and the protection, mapping and advice flags |
| `Clock`   | `ClockGetTime`, `ClockGetResolution`, `Nanosleep`, and the clock identifiers              |
| `Entropy` | `GetRandom`, the only source here fit for a key or a token                                |
| `Process` | `Exit` and `GetPid`                                                                       |
| `Dynamic` | `dlopen`, `dlsym`, `dlclose`, `dlerror`, and the `RTLD_*` flags                           |

The module names describe where each declaration comes from; they are not part of an import path.

FreeBSD reports an error by setting the carry flag and leaving a *positive* errno in the result register, so a raw
instruction result cannot be tested for sign. The assembly wrappers normalize that to a small negative number, which
is Linux's convention, so everything above them reads the same on both systems and `IsError` and `Errno` are the same
two functions.

Nothing here is interchangeable with `Rux/Linux`, and the resemblance is the hazard. The clock identifiers differ —
monotonic is 4 here and 1 there — the open flags are the BSD ones, `MADV_DONTNEED` does not discard where Linux's
does, and only the errno numbers below thirty-five agree. A value taken from one package and passed to the other
compiles and means something else.

## Values and pointers

Kernel records such as `Timespec` are structural values and copy by value. Syscall wrappers deliberately retain raw
pointers because the kernel consumes untyped integer addresses, buffers carry separate lengths, and optional outputs
may be null. Pass addresses explicitly, for example `ClockGetTime(ClockMonotonic, @time)`, and uphold each function's
safety contract. The package does not own raw addresses or descriptors; release them explicitly or use the
higher-level `Rux/Memory` and `Rux/Io` packages.

## Platform

FreeBSD only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os {
    .FreeBSD => import FreeBSD::{ StdOut, Write }
}
```

Higher-level packages such as [`Rux/Io`](../Io) and [`Rux/Memory`](../Memory) already do this, and are the interface to prefer unless you need the syscall itself.

## AArch64 ABI

The AArch64 wrappers follow the FreeBSD syscall convention: the call number
is placed in `x8`, up to six wrapper arguments are shifted into `x0` through
`x5`, and `svc #0` enters the kernel. A carry-set return is converted from a
positive `errno` to the negative result used by the package API.

Descriptor and clock identifiers are sign-extended before entering the generic
`uint64` syscall interface. `Mmap` likewise preserves a signed descriptor while
passing the FreeBSD 15.1 mapping constants and all six arguments directly. The
same source keeps a separate set of x86-64 wrappers, which reach the kernel
through `syscall` and normalize the carry flag the same way.

## Documentation

<https://rux-lang.dev/docs/api/freebsd>

## License

Licensed under the [MIT License](LICENSE.md).
