# Reference: Windows x64 Machine Code Opcodes

This cheat sheet maps common assembly instructions to their raw hexadecimal representations specifically for the Windows x64 environment. It focuses on instructions necessary for Windows argument passing, Shadow Space allocation, and NT-level interfacing.

## 1. Windows-Specific Register Layouts (ModR/M Cheat Sheet)

Combined with a prefix (`0x48` for standard registers, `0x4C` for extended `R8`-`R15` registers) and an opcode, these bytes map the **Source -> Destination** configurations unique to Windows.

### Common `0x89` (MOV Reg -> Reg) Mappings

*Used when shuffling registers into the Windows argument layout, or setting up hooks.*

| ModR/M Byte | Source Register | Destination Register | Full Byte Sequence (with Prefix) | Assembly Translation |
| :--- | :--- | :--- | :--- | :--- |
| `0xD1` | `RCX` | `R10` | `0x4C, 0x89, 0xD1` | `mov r10, rcx` *(Crucial before Windows syscalls)* |
| `0xC1` | `RAX` | `RCX` | `0x48, 0x89, 0xC1` | `mov rcx, rax` |
| `0xCA` | `RCX` | `RDX` | `0x48, 0x89, 0xCA` | `mov rdx, rcx` |
| `0xD0` | `RDX` | `RAX` | `0x48, 0x89, 0xD0` | `mov rax, rdx` |

### Common `0xC7` (MOV Immediate -> Reg) Target Selectors

*Prepares a Windows argument register to receive a hardcoded 32-bit constant/number.*

| ModR/M Byte | Target Register | Full Byte Sequence Prefix | Assembly Translation (Example with 0) |
| :--- | :--- | :--- | :--- |
| `0xC1` | `RCX` (Arg 0) | `0x48, 0xC7, 0xC1` | `mov rcx, 0x00000000` |
| `0xC2` | `RDX` (Arg 1) | `0x48, 0xC7, 0xC2` | `mov rdx, 0x00000000` |
| `0xC0` | `R8`  (Arg 2) | `0x4C, 0xC7, 0xC0` | `mov r8,  0x00000000` |
| `0xC1` | `R9`  (Arg 3) | `0x4C, 0xC7, 0xC1` | `mov r9,  0x00000000` |

---

## 2. Stack Allocation & Shadow Space Opcodes

Windows functions require the caller to reserve at least 32 bytes (0x20 in hex) of scratch space on the stack.

| Byte Sequence | Opcode Action | Hex Value / Size | Assembly Translation | Description |
| :--- | :--- | :--- | :--- | :--- |
| `0x48, 0x83, 0xEC, 0x20` | `SUB rsp, imm8` | `0x20` (32) | `sub rsp, 32` | **Allocate Shadow Space** (Call preparation) |
| `0x48, 0x83, 0xC4, 0x20` | `ADD rsp, imm8` | `0x20` (32) | `add rsp, 32` | **Deallocate Shadow Space** (Post-call cleanup) |
| `0x48, 0x83, 0xEC, 0x28` | `SUB rsp, imm8` | `0x28` (40) | `sub rsp, 40` | Allocates 32 bytes shadow space + 8 bytes alignment |

---

## 3. Windows Stack Parameter Spilling (4+ Arguments)

When a Windows function takes more than 4 arguments, parameters 5, 6, and upwards must be pushed to the stack or read from it.

| Byte Sequence | Base Register | Byte Offset (Hex) | Byte Offset (Dec) | Assembly Translation | Context |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x44, 0x8B, 0x44, 0x24, 0x28` | `RSP` | `0x28` | 40 | `mov r8d, [rsp + 40]` | Read 5th argument (32-bit) from stack |
| `0x4C, 0x8B, 0x4C, 0x24, 0x30` | `RSP` | `0x30` | 48 | `mov r9,  [rsp + 48]` | Read 6th argument (64-bit) from stack |
| `0x48, 0x89, 0x4C, 0x24, 0x20` | `RSP` | `0x20` | 32 | `mov [rsp + 32], rcx` | Spill 1st argument into its home space slot |

---

## 4. Control Flow & Native Environment Switching

| Byte Sequence | Mnemonic | System Target | Description |
| :--- | :--- | :--- | :--- |
| `0x0F, 0x05` | `syscall` | `ntoskrnl.exe` | Switches execution to the Windows Kernel dispatcher |
| `0xFF, 0xD0` | `call rax` | User-space API | Jumps to a dynamically resolved API pointer inside `kernel32.dll` |
| `0xCC`       | `int 3` | Debugger | Triggers a software breakpoint (highly useful for thunk debugging) |
| `0xC3`       | `ret` | Subroutine | Pops return address off the stack and returns to caller |
