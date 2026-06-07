# Reference: Essential x86_64 Machine Code Opcodes

This cheat sheet maps common assembly instructions to their raw hexadecimal representations for x86_64 architectures. It is structured to help you construct custom memory thunks and compatibility layers.

## 1. Structural Modifiers (Prefixes)

Prefixes do not perform actions on their own; they modify how the processor interprets the *next* instruction byte.

| Prefix Byte | Name | Description | Example Effect |
| :--- | :--- | :--- | :--- |
| `0x48` | **REX.W** | Promotes operand size to 64-bit | Changes `mov eax` to `mov rax` |
| `0x4C` | **REX.RXB** | Accesses extended 64-bit registers | Used for registers `R8` through `R15` |
| `0xF3` | **REP / REPE**| Repeats string operations | Used in optimizations (e.g., `rep stosb`) |
| `0x66` | **Operand Size Override** | Down-grades operand size to 16-bit | Changes 32-bit register to 16-bit (`AX`) |

---

## 2. Core Operation Opcodes (Actions)

These bytes represent the fundamental operation the CPU will execute.

| Opcode Byte | Mnemonic | Description | Trailing Bytes Required? |
| :--- | :--- | :--- | :--- |
| `0x89` | **MOV (Reg to Reg/Mem)** | Copies data from a source register | No (handled entirely by ModR/M) |
| `0x8B` | **MOV (Mem/Reg to Reg)** | Loads data into a destination register | No (handled entirely by ModR/M) |
| `0xC7` | **MOV (Imm to Reg/Mem)** | Writes a hardcoded fixed value | **Yes** (Requires 4-byte immediate value) |
| `0xB8` to `0xBF` | **MOV (Imm64 to Reg)**| Writes a full 64-bit hardcoded value | **Yes** (Requires 8-byte immediate value) |
| `0x01` | **ADD** | Arithmetic Addition | No |
| `0x29` | **SUB** | Arithmetic Subtraction | No |
| `0x31` | **XOR** | Bitwise Exclusive OR (often used to zero out regs)| No |
| `0x63` | **MOVSXD** | Move with Sign-Extension (32-bit to 64-bit) | No |

---

## 3. Register-to-Register Layouts (ModR/M Cheat Sheet)

Combined with a prefix and an opcode, these bytes explicitly map the **Source -> Destination** registers.

### Common `0x89` (MOV Reg -> Reg) Mappings

*Used when shuffling arguments between Win64 and Linux layouts.*

| ModR/M Byte | Source Register | Destination Register | Full Byte Sequence (with `0x48`/`0x4C`) | Assembly Translation |
| :--- | :--- | :--- | :--- | :--- |
| `0xC8` | `RCX` | `RAX` | `0x48, 0x89, 0xC8` | `mov rax, rcx` |
| `0xD7` | `RDX` | `RDI` | `0x48, 0x89, 0xD7` | `mov rdi, rdx` |
| `0xC6` | `R8`  | `RSI` | `0x4C, 0x89, 0xC6` | `mov rsi, r8`  |
| `0xCA` | `R9`  | `RDX` | `0x4C, 0x89, 0xCA` | `mov rdx, r9`  |
| `0xCF` | `RCX` | `RDI` | `0x48, 0x89, 0xCF` | `mov rdi, rcx` |
| `0xD6` | `RDX` | `RSI` | `0x48, 0x89, 0xD6` | `mov rsi, rdx` |

### Common `0xC7` (MOV Immediate -> Reg) Target Selectors

*Prepares the register to accept a hardcoded number that follows immediately after.*

| ModR/M Byte | Target Register | Full Byte Sequence Prefix | Assembly Translation (Example with 0) |
| :--- | :--- | :--- | :--- |
| `0xC0` | `RAX` | `0x48, 0xC7, 0xC0` | `mov rax, 0x00000000` |
| `0xC7` | `RDI` | `0x48, 0xC7, 0xC7` | `mov rdi, 0x00000000` |
| `0xC6` | `RSI` | `0x48, 0xC7, 0xC6` | `mov rsi, 0x00000000` |

---

## 4. Stack Offsets (ModR/M + SIB + Displacement)

When pulling arguments from the Stack (`RSP`), the CPU requires an address calculation string.

| Byte Sequence | Base Register | Byte Offset (Hex) | Byte Offset (Dec) | Assembly Translation |
| :--- | :--- | :--- | :--- | :--- |
| `0x54, 0x24, 0x28` | `RSP` | `0x28` | 40 | `[rsp + 40]` (Win64 5th arg slot) |
| `0x54, 0x24, 0x30` | `RSP` | `0x30` | 48 | `[rsp + 48]` (Win64 6th arg slot) |
| `0x54, 0x24, 0x38` | `RSP` | `0x38` | 56 | `[rsp + 56]` (Win64 7th arg slot) |

*Example of loading a stack argument into register `R10`:* `0x4C, 0x8B, 0x54, 0x24, 0x28` -> `mov r10, [rsp + 40]`

---

## 5. Control Flow & Environment Opcodes

Instructions that change execution location or switch hardware privilege states.

| Byte Sequence | Mnemonic | Functional Context | Description |
| :--- | :--- | :--- | :--- |
| `0x0F, 0x05` | `syscall` | Kernel Interface | Switches execution to ring-0 (kernel mode) |
| `0xCD, 0x80` | `int 0x80` | Legacy Kernel Interface | Legacy 32-bit Linux system call interrupt |
| `0xC3` | `ret` | Subroutine Control | Pops the return address off stack and jumps to it |
| `0x90` | `nop` | Padding / Alignment | No Operation. Does nothing, advances Instruction Pointer |
| `0xE9` | `jmp` | Near Jump | **Yes** (Requires 4-byte relative jump offset) |
