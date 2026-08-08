# Windows

Windows platform bindings: Win32 declarations linked straight against `Kernel32.dll`, with no C runtime in between.

## Installation

```sh
rux add Rux/Windows
```

## What it provides

- **Files** — `CreateFileA` and `CreateFileW`, `ReadFile`, `WriteFile`, `CloseHandle`, the `CopyFileA` / `MoveFileA` / `DeleteFileA` set, `GetFileSizeEx`, `SetFilePointerEx`, and `GetFileAttributesA` / `SetFileAttributesA`.
- **Directories** — `CreateDirectoryA` and `RemoveDirectoryA`, `GetCurrentDirectoryA` and `SetCurrentDirectoryA`, and the `FindFirstFileA` / `FindNextFileA` / `FindClose` walk.
- **Console** — `AllocConsole`, `GetStdHandle`, `GetConsoleMode`, `ReadConsoleA`, `WriteConsoleA` and `WriteConsoleW`, plus `Beep`.
- **Memory** — the process heap through `GetProcessHeap`, `HeapAlloc`, `HeapReAlloc` and `HeapFree`, and the block operations `RtlCopyMemory`, `RtlZeroMemory`, `RtlFillMemory` and `RtlCompareMemory`.
- **Process, thread and time** — `ExitProcess`, `GetCurrentProcessId`, `GetCurrentThreadId`, `Sleep`, `GetTickCount64`, and `GetSystemTime` / `GetLocalTime`.
- **Dynamic loading** — `LoadLibraryA`, `GetProcAddress` and `FreeLibrary`.
- **Text encoding** — `MultiByteToWideChar` and `WideCharToMultiByte`, with code pages named by the `CodePage` enum rather than passed as bare numbers.
- **Types and constants** — `FileTime`, `SystemTime` and `Win32FindDataA`; the `CreationDisposition` enum; and the standard handles `StdInputHandle`, `StdOutputHandle` and `StdErrorHandle`.

Failure is reported the Win32 way rather than through an errno: a `bool32` or a sentinel return says that a call failed, and the reason is retrieved separately with `GetLastError`.

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
