# Documentation: Rux Calling Convention Translation Thunks

This document details the inner workings of the internal memory thunks used by Rux when running under `RUX_IS_ELF_OS`.

## 1. The Problem: Windows vs. Linux x86_64 ABI

By default, Rux uses the **Win64 register layout** (Microsoft x64 Calling Convention) internally. When a program needs to execute a native system call (`syscall`) on an ELF-based operating system (such as Linux), the arguments must be shuffled into the registers expected by the **System V AMD64 ABI**.

| Argument Role | Win64 Calling Convention (Rux Input) | Linux x86_64 Syscall ABI (Target) |
| :--- | :--- | :--- |
| **Syscall Number** | `RCX` | **`RAX`** |
| **1st Parameter** | `RDX` | **`RDI`** |
| **2nd Parameter** | `R8`  | **`RSI`** |
| **3rd Parameter** | `R9`  | **`RDX`** |
| **4th Parameter** | Stack (`[RSP + 40]`) | **`R10`** |
| **5th Parameter** | Stack (`[RSP + 48]`) | **`R8`** |
| **6th Parameter** | Stack (`[RSP + 56]`) | **`R9`** |

---

## 2. CPU Instruction Grammar (Machine Code)

The x86_64 processor decodes bytes based on a rigid, modular schema:

1. **Prefix (`0x48`)**: Tells the CPU to execute the upcoming instruction in 64-bit mode.
2. **Opcode (Action)**: Defines the operation (e.g., `0x89` for register copy, `0xC7` for writing constants).
3. **ModR/M Byte (Target/Source)**: Specifies which registers are involved.
4. **Immediate (Filling)**: Optional trailing bytes (4 bytes for 32-bit constants) containing data.

### Pattern A: Register-to-Register Copy (No trailing bytes needed)

* **`0x48, 0x89, 0xC8`** -> `mov rax, rcx` (Set Syscall Number)
* **`0x48, 0x89, 0xD7`** -> `mov rdi, rdx` (Set 1st Parameter)
* **`0x4C, 0x89, 0xC6`** -> `mov rsi, r8` (Set 2nd Parameter)
* **`0x4C, 0x89, 0xCA`** -> `mov rdx, r9` (Set 3rd Parameter)

### Pattern B: Writing a Constant to a Register (Requires 4 bytes of filling)

* **`0x48, 0xC7, 0xC0`** -> `mov rax, [32-bit Integer]`

---

## 3. Core Instruction Reference (Cheat Sheet)

### System Instructions

* **`0x0F, 0x05`** -> `syscall` (Triggers the Linux kernel transition)
* **`0xC3`** -> `ret` (Returns from the thunk function)

### Stack Access (For 4 or more parameters)

When Windows runs out of registers, the thunks pull the remaining parameters relative to the stack pointer (`RSP`):

* **`0x4C, 0x8B, 0x54, 0x24, 0x28`** -> `mov r10, [rsp + 40]` (Fetches 4th parameter)
* **`0x4C, 0x8B, 0x44, 0x24, 0x30`** -> `mov r8,  [rsp + 48]` (Fetches 5th parameter)
* **`0x4C, 0x8B, 0x4C, 0x24, 0x38`** -> `mov r9,  [rsp + 56]` (Fetches 6th parameter)

---

## 4. Specialized Hardcoded Thunks

### `__rux_linux_nanosleep` (Syscall 35)

Suspends the execution of the calling thread. Hardcodes the number `35` (`0x23`) into `RAX`.

```text
0x48, 0xC7, 0xC0, 0x23, 0x00, 0x00, 0x00  ; mov rax, 35
0x48, 0x89, 0xCF                          ; mov rdi, rcx
0x48, 0x89, 0xD6                          ; mov rsi, rdx
0x0F, 0x05                                ; syscall
0xC3                                      ; ret

```
