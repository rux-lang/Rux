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

**Prefer the `W` entry points.** The `A` forms convert through the process code page, which cannot express every name a file system can hold and differs between machines — so a path that works on one system fails on another for reasons the program cannot see. A path is text a user or a file system chose, not text the program chose. The `A` forms stay declared because existing callers use them, and each is now marked.

Two failure sentinels, and they are not the same. A handle-returning call reports `InvalidHandleValue`, which is `-1` cast to a pointer; a memory-returning call reports null; and `GetFileAttributesW` reports a third value again, `INVALID_FILE_ATTRIBUTES`. Testing a handle against null, or an allocation against `InvalidHandleValue`, reads a failure as a success. `BCryptGenRandom` reports none of them — it returns an `NTSTATUS`, which `GetLastError` knows nothing about, so `GetRandom`'s boolean is the only thing a caller may read after it. A successful draw leaves whatever Win32 code was already standing exactly where it was.

**`CloseHandle(InvalidHandleValue)` succeeds.** `(HANDLE)-1` is the current process's pseudo-handle, not an invalid one, and closing a pseudo-handle is a no-op that reports success — where closing null or an arbitrary number fails with `ERROR_INVALID_HANDLE`. A cleanup path that closes the sentinel defensively is therefore not caught by its return value; it silently does nothing. This is asserted in `Tests/Packages/Windows/Failures`, because it is not what the name suggests.

Windows separates reserving address space from committing storage to it, which no other supported system does. A reserved page may not be touched; committing is what makes it usable. `MEM_RELEASE` requires a size of zero and the exact base address the reservation returned. Releasing a reservation twice **fails**, with `ERROR_INVALID_ADDRESS`, where POSIX's `munmap` on an already-unmapped range succeeds — so code that treats a double release as harmless is portable in one direction only.

## What is tested

`Tests/Packages/Windows/Failures` covers the three sentinels, the Win32 code each kind of failure leaves behind, the different codes the virtual allocator and the heap give for the same impossible size, the release rules, and the `NTSTATUS` domain staying clear of `GetLastError`. `Tests/Packages/Windows/Platform` covers the reserve-then-commit split, protection, the entropy source and the clocks; `Tests/Packages/Windows/Constants` covers the enumerated values. `Tests/Unit/PlatformBindingContractTests.cpp` additionally checks, on any host, that the sentinels differ, that every `ERROR_` constant holds its published value, and that no two of them share a number.

Unlike the three POSIX packages, all of this runs where it is developed, so every number in it was read out of a running program rather than out of documentation.

## Values and pointers

Win32 records such as `FileTime` and `SystemTime` are structural values and copy by value. Package-owned inspection helpers borrow them, so call `FileTimeTicks(stamp)` with the value itself. Imported Win32 declarations retain raw pointers for handles, optional arguments, buffers, and OS-owned addresses because null and sentinel values are part of those contracts. A raw handle or pointer never transfers ownership automatically; close or release it with the matching Win32 function.

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
