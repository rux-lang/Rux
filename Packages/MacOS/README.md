# MacOS

macOS platform bindings: the raw Darwin syscall interface, with no libSystem in between.

## Installation

```sh
rux add Rux/MacOS
```

## What it provides

- **Syscall entry points** — `Syscall0` through `Syscall6`, written as inline assembly.
- **Wrappers** — `Read`, `Write`, `Close`, `Exit`, `GetPid`, `Mmap`, `Munmap`, and `GetTimeOfDay`.
- **Constants** — the standard descriptors `StdIn`, `StdOut`, `StdErr`; the `mmap` protection and mapping flags; and the class-qualified syscall numbers.
- **Types and helpers** — `Timeval`, plus `Errno`, `IsError`, and `SignExtendFd` for turning a raw return value into a result you can test.

Darwin partitions syscall numbers by class, with the Unix/BSD calls in class 2. The `Sys*` constants here are complete, class-qualified values, so they are passed to `Syscall0` through `Syscall6` as they are rather than combined with `UnixSyscallClass` at the call site.

## Platform

macOS only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os == .MacOS {
    import MacOS::{ StdOut, Write };
}
```

## Documentation

<https://rux-lang.dev/docs/api/macos>

## License

Licensed under the [MIT License](LICENSE.md).
