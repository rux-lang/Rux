# Linux

Linux platform bindings: the raw syscall interface, with no libc in between.

## Installation

```sh
rux add Rux/Linux
```

## What it provides

| Module    | Covers                                                                                  |
| --------- | --------------------------------------------------------------------------------------- |
| `Types`   | the kernel ABI's own types: `ProcessId`, `FileDescriptor`, `FileOffset`, `Timespec`      |
| `Errors`  | the errno numbers, and `IsError` / `Errno` for reading a raw result                      |
| `Syscall` | `Syscall0` through `Syscall6` as inline assembly, the call numbers, and `SignExtendFd`   |
| `File`    | `Read`, `Write`, `Close`, `OpenAt`, `Lseek`, `Fsync`, `Ftruncate`, `UnlinkAt`, `MkdirAt`, `RenameAt`, `Fstat`, `Dup3`, `Pipe2`, the open flags, and the three standard descriptors |
| `Memory`  | `Mmap`, `Munmap`, `Brk`, `Mprotect`, `Madvise`, and the protection, mapping and advice flags |
| `Clock`   | `ClockGetTime`, `ClockGetResolution`, `Nanosleep`, `ClockNanosleep`, and the clock identifiers |
| `Process` | `Exit` and `GetPid`                                                                      |
| `Entropy` | `GetRandom`, the only source here fit for a key or a token                                |
| `Dynamic` | `dlopen`, `dlsym`, `dlclose`, `dlerror`, and the `RTLD_*` flags                          |

The module names describe where each declaration comes from; they are not part of an import path.

There is no `errno` here. The kernel returns the error in the result register as a small negative number, and libc is
what turns that into a positive `errno` and a `-1` return. This package calls the kernel directly, so a caller tests a
raw result with `IsError` and reads the number with `Errno`. Only `-4095` through `-1` is an error, which is what
makes `Mmap` workable: a mapping address may legitimately have its top bit set.

Only calls both architectures have are wrapped. AArch64 came after the directory-relative interface and never got
`open`, `stat`, `unlink`, `mkdir`, `rename`, `dup2` or `pipe`, so this package offers the `*at` forms alone and
`AtFdCwd` is what makes an ordinary path work through them. One wrapper is then right on both.

The errno numbers and the `RTLD_*` flags keep the kernel's own spelling, because `EINVAL` is what a man page names
and what a reader porting code will look for. The rest of the constants use Rux names, which is what they were
published under.

## Platform

Linux only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os == .Linux {
    import Linux::{ StdOut, Write };
}
```

## Documentation

<https://rux-lang.dev/docs/api/linux>

## License

Licensed under the [MIT License](LICENSE.md).
