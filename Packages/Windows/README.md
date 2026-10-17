# Windows

Windows platform bindings: Win32 declarations linked straight against the system's own libraries — `Kernel32.dll`, `User32.dll`, `Gdi32.dll`, `Advapi32.dll`, `ntdll.dll` and the rest — with no C runtime in between.

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
| `Shell`     | `CommandLineToArgvW`, which splits the command line into arguments the way the C runtime does |
| `Synch`     | `WaitOnAddress`, `WakeByAddressSingle` and `WakeByAddressAll`, from the synchronization API set |
| `ProcessControl` | `CreateProcessW` and its structures, `CreatePipe`, handle inheritance, `GetExitCodeProcess`, `TerminateProcess` and `SetEnvironmentVariableW` |
| `TimeZone`  | `GetTimeZoneInformation`, `TimeZoneInformation` and `TimeZoneRuleDate`                 |
| `Winsock`   | `WSAStartup`, `WSAGetLastError`, the Ws2_32 socket calls, `ioctlsocket` with `FIONBIO`, `WSAPoll` with `WsaPollFd` and the `POLL*` flags, `getaddrinfo` and `freeaddrinfo`, `WsaData`, `AddressInfo` and the socket constants |
| `Window`    | `RegisterClassExW`, `CreateWindowExW`, `DefWindowProcW`, the `GetMessageW` loop, `BeginPaint`/`EndPaint`, `MessageBoxW`, per-monitor DPI, `WindowClassEx`, `Message`, `PaintStruct`, `Rect` and the `WS_`, `WM_` and `SW_` values |
| `Gdi`       | `CreateFontW`, `SelectObject`, `SetBkMode`, `SetTextColor`, `TextOutW` and `DeleteObject`, with the font weights and quality values |
| `Registry`  | `RegGetValueW`, the predefined keys and the `RRF_RT_` type flags |
| `Version`   | `RtlGetVersion` and `OsVersionInfoEx`, the true version of the running system |
| `SystemInformation` | `GlobalMemoryStatusEx` with `MemoryStatusEx`, `GetPhysicallyInstalledSystemMemory`, `GetNativeSystemInfo`, and `GetLogicalProcessorInformationEx` for counting cores |

The module names describe where each declaration comes from; they are not part of an import path.

**Prefer the `W` entry points.** The `A` forms convert through the process code page, which cannot express every name a file system can hold and differs between machines — so a path that works on one system fails on another for reasons the program cannot see. A path is text a user or a file system chose, not text the program chose. The `A` forms stay declared because existing callers use them, and each is now marked.

Two failure sentinels, and they are not the same. A handle-returning call reports `InvalidHandleValue`, which is `-1` cast to a pointer; a memory-returning call reports null; and `GetFileAttributesW` reports a third value again, `INVALID_FILE_ATTRIBUTES`. Testing a handle against null, or an allocation against `InvalidHandleValue`, reads a failure as a success. `BCryptGenRandom` reports none of them — it returns an `NTSTATUS`, which `GetLastError` knows nothing about, so `GetRandom`'s boolean is the only thing a caller may read after it. A successful draw leaves whatever Win32 code was already standing exactly where it was.

**`CloseHandle(InvalidHandleValue)` succeeds.** `(HANDLE)-1` is the current process's pseudo-handle, not an invalid one, and closing a pseudo-handle is a no-op that reports success — where closing null or an arbitrary number fails with `ERROR_INVALID_HANDLE`. A cleanup path that closes the sentinel defensively is therefore not caught by its return value; it silently does nothing. This is asserted in `Tests/Packages/Windows/Failures`, because it is not what the name suggests.

Windows separates reserving address space from committing storage to it, which no other supported system does. A reserved page may not be touched; committing is what makes it usable. `MEM_RELEASE` requires a size of zero and the exact base address the reservation returned. Releasing a reservation twice **fails**, with `ERROR_INVALID_ADDRESS`, where POSIX's `munmap` on an already-unmapped range succeeds — so code that treats a double release as harmless is portable in one direction only.

**Ask `RtlGetVersion`, not `GetVersionExW`, which version is running.** Since Windows 8.1, `GetVersionExW` reports the version a program's manifest says it supports, and a program with no manifest — every Rux program today — is told 6.2 on every later release. `RtlGetVersion` reports the truth. Windows 11 still reports major version 10; its build numbers start at 22000, and that is how it is told apart. The registry's `ProductName` still says "Windows 10" on Windows 11 too.

**A registry call returns its error.** `RegGetValueW` gives back a Win32 code as its result and leaves `GetLastError` alone, so reading `GetLastError` after a failed registry read reports whatever failed before it. A missing key or value is `ERROR_FILE_NOT_FOUND`, and a buffer too small is `ERROR_MORE_DATA`, where most of Win32 says `ERROR_INSUFFICIENT_BUFFER`. The predefined keys are 32-bit values sign-extended to a pointer, so `HKEY_LOCAL_MACHINE` is `0xFFFFFFFF80000002`.

**A window procedure is an ordinary Rux function.** Its type is `WindowProcedure`, and it is called on the thread that created the window, with the platform's C convention, which is what a Rux function uses on Windows. A Rux program is built as a console program, so a program that only shows windows calls `FreeConsole` first.

**UTF-16 strings must be aligned.** The kernel refuses a UTF-16 string that is not on a two-byte boundary, with `ERROR_NOACCESS`. A `c16"..."` literal is always aligned to its code unit, whether it is a `const` or written at the call, so `name.data` can be passed directly; a buffer of bytes reinterpreted as `char16` must be aligned by its owner.

## What is tested

`Tests/Packages/Windows/Failures` covers the three sentinels, the Win32 code each kind of failure leaves behind, the different codes the virtual allocator and the heap give for the same impossible size, the release rules, and the `NTSTATUS` domain staying clear of `GetLastError`. `Tests/Packages/Windows/Platform` covers the reserve-then-commit split, protection, the entropy source and the clocks; `Tests/Packages/Windows/Constants` covers the enumerated values. `Tests/Packages/Windows/SystemInformation` checks the true version against the registry's build number, the registry's typed reads and its two failure codes, memory against what is fitted, and the core count against the logical processors. `Tests/Packages/Windows/Window` registers a class, creates a message-only window whose Rux procedure receives `WM_CREATE`, a sent message with both parameters intact and `WM_DESTROY`, and makes and deletes a font, so nothing is ever shown. `Tests/Unit/PlatformBindingContractTests.cpp` additionally checks, on any host, that the sentinels differ, that every `ERROR_` constant holds its published value, and that no two of them share a number.

Unlike the three POSIX packages, all of this runs where it is developed, so every number in it was read out of a running program rather than out of documentation.

## Values and pointers

Win32 records such as `FileTime` and `SystemTime` are structural values and copy by value. Package-owned inspection helpers borrow them, so call `FileTimeTicks(stamp)` with the value itself. Imported Win32 declarations retain raw pointers for handles, optional arguments, buffers, and OS-owned addresses because null and sentinel values are part of those contracts. A raw handle or pointer never transfers ownership automatically; close or release it with the matching Win32 function.

## Platform

Windows only. Guard use behind a compile-time check, so a build for another target never resolves these declarations:

```rux
import Core::{ #target };

when #target.os == .Windows {
    import Windows::{ GetStdHandle, StdOutputHandle, WriteConsoleA };
}
```

## Documentation

<https://rux-lang.dev/docs/api/windows>

## License

Licensed under the [MIT License](LICENSE.md).
