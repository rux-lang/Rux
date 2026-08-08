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

## Documentation

<https://rux-lang.dev/docs/api/bsd>

## License

Licensed under the [MIT License](LICENSE.md).
