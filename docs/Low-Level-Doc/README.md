# Rux Low-Level Documentation

Welcome to the low-level architecture documentation for the Rux compatibility layer.
This directory contains detailed technical specifications regarding how Rux translates
and interfaces with native operating system environments at the machine-code level.

## 📂 Documentation Structure

To make navigation easier, the technical reference has been split into two specialized core files:

### 🐧 Linux & 😈 BSD Subsystems (System V AMD64 ABI)

* 📄 [Thunks Architecture (Thunks.md)](./Thunks.md)
  * **What it covers:** The conceptual framework behind Rux's memory thunks when executing under `RUX_IS_ELF_OS` (Linux, BSD, SunOS).
  * **Key Content:** The structural conflict between the Win64 layout and the System V ABI, argument remapping, and specialized stubs like `__rux_linux_nanosleep`.
* 📄 [Opcodes Cheat Sheet (OPCodes.md)](./OPCodes.md)
  * **What it covers:** A pure machine-code lookup table for x86_64 architecture bytes mapping System V registers (`RDI`, `RSI`, `RDX`, `RCX`).

---

### 🪟 Windows Subsystem (Microsoft x64 Calling Convention)

* 📄 [Windows Thunks Architecture (Thunks_Windows.md)](./Thunks_Windows.md)
  * **What it covers:** The behavioral rules governing the native Windows environment and raw NT-level interfacing.
  * **Key Content:** The mandatory 32-byte Shadow Space requirement, stack management rules, and the hardware-forced `RCX` $\rightarrow$ `R10` parameter swap during a direct Windows `syscall`.
* 📄 [Windows Opcodes Cheat Sheet (OPCodes_Windows.md)](./OPCodes_Windows.md)
  * **What it covers:** A specialized machine-code reference detailing bytecode layouts for Windows registers (`RCX`, `RDX`, `R8`, `R9`), stack-spilling offsets, and stack allocation/cleanup sequences.

---

## 🛠️ Quick Workflow Reference

When adding features or debugging compatibility layers within this subsystem, your workflow should follow these steps:

1. **Identify the mapping requirements** using the respective **[Linux/BSD Thunks](./Thunks.md)** or **[Windows Thunks](./Thunks_Windows.md)**.
2. **Look up the required byte composition syntax** in **[OPCODES.md](./OPCodes.md)** or **[OPCODES_WINDOWS.md](./OPCodes_Windows.md)**.
3. Test and verify your byte logic using native tools (`nasm`, `hexdump`) or the recommended online tools linked below.

---

## 🔗 External Resources & Tools

Use these highly recommended external references and interactive tools for developing, debugging, or expanding Rux's low-level components:

### Interactive Online Tools

* 🛠️ [Defuse Online Assembler/Disassembler](https://defuse.ca/online-x86-assembler.htm) - Instant conversion between assembly text mnemonics and raw hexadecimal machine code.
* 🚀 [Compiler Explorer (godbolt.org)](https://godbolt.org/) - See exactly how modern compilers (GCC/Clang) generate x86_64 assembly from high-level C++ code in real-time.

### Official Opcodes & Architecture Specifications

* 📖 [Felix Cloutier's x86/x64 Instruction Reference](https://www.felixcloutier.com/x86/) - The gold standard database for looking up individual instruction behaviors without digging through thousands of pages of Intel manuals.
* 📄 [System V AMD64 ABI Specification (PDF)](https://gitlab.com/x86-psABIs/x86-64-ABI/-/jobs/artifacts/master/raw/x86-64-ABI/abi.pdf?job=build) - The foundational official standard document governing ELF file structures and the Linux/BSD calling layout.
* 🪟 [Microsoft x64 Calling Convention Documentation](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention) - Official documentation on register usage and stack allocation under Windows environments.

### Syscall Lookup Directories

* 📊 [Chromium OS Linux x86_64 Syscall Table](https://blog.rchapman.org/posts/Linux_System_Call_Table_for_x86_64/) - A clean, fast layout mapping Linux syscall numbers directly to their respective ABI registers.
* 😈 [FreeBSD Syscall Reference Table](https://github.com/freebsd/freebsd-src/blob/main/sys/kern/syscalls.master) - The official master file tracking native FreeBSD system call numbers (essential for `RUX_IS_BSD` logic).
* 🪟 [Windows X64 Syscall Table](https://j00ru.vexillium.org/syscalls/nt/64/) - A historical database tracking internal Windows NT system call numbers across OS versions.
* 🔍 [Linux Syscall Search Engine (syscalls.mebeim.net)](https://syscalls.mebeim.net/) - An interactive tool to quickly search syscall registers and instantly jump to their official kernel manual pages.
