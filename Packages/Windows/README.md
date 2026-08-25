# Windows

Windows platform bindings: Win32 declarations linked straight against `Kernel32.dll`, with no C runtime in between.

## Installation

```sh
rux add Rux/Windows
```

## What it provides

| Module      | Covers                                                                              |
| ----------- | ------------------------------------------------------------------------------------ |
| `Types`     | `Handle`, `ModuleHandle`, `Bool`, `Dword`, `WideChar`, `NtStatus`, `FileTime`, `SystemTime`, and `InvalidHandleValue` |
| `Constants` | the code pages and the file-creation dispositions                                      |
| `Kernel`    | the Kernel32 entry points, including the `A` forms kept for compatibility              |
| `File`      | the Unicode `W` file, directory and console entry points, with the access, share and move flags |
| `Memory`    | `VirtualAlloc`, `VirtualFree`, `VirtualProtect`, `VirtualLock`, and the state and protection flags |
| `Entropy`   | `BCryptGenRandom` behind a `GetRandom` that either fills the buffer or fails            |
| `Clock`     | `QueryPerformanceCounter`, `QueryPerformanceFrequency`, the precise wall clock, and the epoch conversion |

The module names describe where each declaration comes from; they are not part of an import path.

**Prefer the `W` entry points.** The `A` forms convert through the process code page, which cannot express every name
a file system can hold and differs between machines — so a path that works on one system fails on another for reasons
the program cannot see. A path is text a user or a file system chose, not text the program chose. The `A` forms stay
declared because existing callers use them, and each is now marked.

Two failure sentinels, and they are not the same. A handle-returning call reports `InvalidHandleValue`, which is `-1`
cast to a pointer; a memory-returning call reports null. `BCryptGenRandom` reports neither — it returns an `NTSTATUS`,
which `GetLastError` knows nothing about.

Windows separates reserving address space from committing storage to it, which no other supported system does. A
reserved page may not be touched; committing is what makes it usable. `MEM_RELEASE` requires a size of zero and the
exact base address the reservation returned.

## Values and pointers

Win32 records such as `FileTime` and `SystemTime` are structural values and copy by value. Package-owned inspection
helpers borrow them, so call `FileTimeTicks(stamp)` with the value itself. Imported Win32 declarations retain raw
pointers for handles, optional arguments, buffers, and OS-owned addresses because null and sentinel values are part
of those contracts. A raw handle or pointer never transfers ownership automatically; close or release it with the
matching Win32 function.

## Platform

Windows only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::#target;

when #target.os == .Windows {
    import Windows::{ GetStdHandle, StdOutputHandle, WriteConsoleA };
}
```

## Documentation

<https://rux-lang.dev/docs/api/windows>

## License

Licensed under the [MIT License](LICENSE.md).
