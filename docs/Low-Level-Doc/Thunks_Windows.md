# Documentation: Rux Windows x64 Calling Convention & Thunks

This document details the mechanics of the x86_64 calling convention used natively by Windows (the Microsoft x64 Calling Convention) and how memory thunks interact with it.

## 1. The Win64 Architecture

Unlike Linux and BSD, which use the System V AMD64 ABI, Windows utilizes its own unique layout for passing arguments to functions and native system calls (`syscall`).

### Core Rules of Windows x64

1. **The Register Quartet:** The first 4 integer or pointer arguments are passed strictly in **`RCX`**, **`RDX`**, **`R8`**, and **`R9`**.
2. **Shadow Space (Home Space):** The caller **must** allocate 32 bytes of temporary storage on the stack right before calling a function. Even if a function takes 0 or 2 arguments, these 32 bytes must be allocated (`sub rsp, 32`).
3. **Stack Cleanup:** The caller is responsible for cleaning up the stack (`add rsp, 32`) after the function returns.

### Windows x64 Register Layout

| Argument Slot | Data Type / Role | Register |
| :--- | :--- | :--- |
| **Return Value** | Integer / Pointer | **`RAX`** |
| **1st Parameter** | Integer / Pointer / Handle | **`RCX`** |
| **2nd Parameter** | Integer / Pointer | **`RDX`** |
| **3rd Parameter** | Integer / Pointer | **`R8`** |
| **4th Parameter** | Integer / Pointer | **`R9`** |
| **5th Parameter and beyond** | Memory Spills | **Stack** (`[RSP + 40]`, `[RSP + 48]`, etc.) |

---

## 2. Machine Code Translation Patterns

Because Rux standardizes on the Win64 register layout internally, invoking standard Win64 functions doesn't require shuffling registers. However, if you write raw system hooks or direct NT-level kernel `syscall` thunks under Windows, you must load the System Service Identifier (SSID) into `EAX`/`RAX` and utilize the `EDX`/`R10` swap.

### The `syscall` R10 Override Pattern

On Windows, when executing a direct kernel `syscall`, the processor automatically destroys the content of `RCX` (it is used to store the return instruction pointer `RIP`). Because of this hardware behavior, **Windows redirects the 1st parameter from `RCX` into `R10`** right before firing the syscall.

* **`0x4C, 0x89, 0xD1`** -> `mov r10, rcx` (Preserves 1st argument into `R10`)
* **`0x48, 0xC7, 0xC0`** -> `mov rax, [32-bit Windows Syscall Number]`

---

## 3. Core Windows Instruction Reference

### Native Windows Control Flow

* **`0x0F, 0x05`** -> `syscall` (Triggers ring-0 transit into `ntoskrnl.exe`)
* **`0x48, 0x83, 0xEC, 0x20`** -> `sub rsp, 32` (Allocates mandatory 32-byte Shadow Space)
* **`0x48, 0x83, 0xC4, 0x20`** -> `add rsp, 32` (Cleans up 32-byte Shadow Space)

### Common Parameter Shuffling

If you ever need to map a System V layout (Linux style input) *back* into a Windows function, use these mappings:

* `0x48, 0x89, 0xF9` -> `mov rcx, rdi` (System V Arg 0 -> Windows Arg 0)
* `0x48, 0x89, 0xF2` -> `mov rdx, rsi` (System V Arg 1 -> Windows Arg 1)

---

## 4. Example: Hardcoded NT Syscall Thunk

Direct NT syscall numbers change between Windows builds (e.g., Windows 10 vs Windows 11). However, the boilerplate pattern for a raw environment wrapper always looks like this:

```text
0x4C, 0x89, 0xD1                          ; mov r10, rcx      <- Copy 1st arg because syscall overwrites RCX
0x48, 0xC7, 0xC0, 0x1E, 0x00, 0x00, 0x00  ; mov rax, 0x1E     <- Example SSID (e.g., NtWriteFile)
0x0F, 0x05                                ; syscall           <- Jump to Windows Kernel
0xC3                                      ; ret               <- Return to Rux

```
