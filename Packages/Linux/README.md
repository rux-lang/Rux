# Linux

Linux platform bindings: the raw syscall interface, with no libc in between.

## Installation

```sh
rux add Rux/Linux
```

## What it provides

- **Syscall entry points** — `Syscall0` through `Syscall6`, written as inline assembly.
- **Wrappers** — `Read`, `Write`, `Close`, `Exit`, `GetPid`, `Brk`, `Mmap`, `Munmap`, `Nanosleep`, and `ClockGetTime`.
- **Constants** — the standard descriptors `StdIn`, `StdOut`, `StdErr`; the `mmap` protection and mapping flags; and `ClockRealtime` / `ClockMonotonic`.
- **Types and helpers** — `Timespec`, plus `Errno`, `IsError`, and `SignExtendFd` for turning a raw return value into a result you can test.

## Platform

Linux only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Rux::#target;

when #target.os == .Linux {
    import Linux::{ StdOut, Write };
}
```

Higher-level packages such as [`Rux/Io`](../Io) and [`Rux/Memory`](../Memory) already do this, and are the interface to prefer unless you need the syscall itself.

## Documentation

<https://rux-lang.dev/docs/api/linux>

## License

Licensed under the [MIT License](https://github.com/rux-lang/Rux/blob/main/LICENSE.md).
