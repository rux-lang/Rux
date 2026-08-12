# Bsd

BSD platform bindings: the raw syscall interface shared by FreeBSD, OpenBSD, NetBSD, and DragonFly BSD.

## Installation

```sh
rux add Rux/Bsd
```

## What it provides

- **Syscall entry points** — `Syscall0` through `Syscall6`, written as inline assembly.
- **Wrappers** — `Read`, `Write`, `Close`, `Exit`, `GetPid`, `Brk`, `Mmap`, `Munmap`, `Nanosleep`, and `ClockGetTime`.
- **Constants** — the standard descriptors `StdIn`, `StdOut`, `StdErr`; the `mmap` protection and mapping flags; and the syscall numbers themselves (`SysRead`, `SysWrite`, `SysClose`, `SysExit`, `SysGetPid`, `SysBrk`).
- **Types and helpers** — `Timespec`, plus `Errno`, `IsError`, and `SignExtendFd` for turning a raw return value into a result you can test.

## Platform

The four BSDs only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os {
    .DragonFlyBSD, .FreeBSD, .NetBSD, .OpenBSD => import Bsd::{ StdOut, Write }
}
```

Higher-level packages such as [`Rux/Io`](../Io) and [`Rux/Memory`](../Memory) already do this, and are the interface to prefer unless you need the syscall itself.

## AArch64 ABI

The AArch64 wrappers follow the FreeBSD 14 syscall convention: the call number
is placed in `x8`, up to six wrapper arguments are shifted into `x0` through
`x5`, and `svc #0` enters the kernel. A carry-set return is converted from a
positive `errno` to the negative result used by the package API.

Descriptor and clock identifiers are sign-extended before entering the generic
`uint64` syscall interface. `Mmap` likewise preserves a signed descriptor while
passing the FreeBSD 14 mapping constants and all six arguments directly. The
same source keeps separate x86-64 wrappers and target-selected constants for the
other supported BSD systems.

## Documentation

<https://rux-lang.dev/docs/api/bsd>

## License

Licensed under the [MIT License](LICENSE.md).
