# Rux Project Documentation Portal

Welcome to the official documentation portal for the **Rux Programming Language** ecosystem. This directory serves as the centralized knowledge base for developers, maintainers, and contributors working on the Rux compiler, runtime, tooling, and platform compatibility layers.

---

## 📂 Documentation Directory Map

The documentation is organized into modular sections covering different abstraction layers of the project—from low-level binary translation up to IDE integrations:

### 🦾 1. Low-Level Core & OS Compatibility Layers

This subsystem documents how the Rux runtime interfaces directly with operating system kernels and translates binary structures across different environments.

* 📂 **[Low-Level Documentation Root](./Low-Level-Doc/README.md)** (Sub-Root Index)
  * 📄 **[Linux & BSD Thunks (THUNKS.md)](././Low-Level-Doc/Thunks.md):** Architectural deep-dive into bridging the Win64 calling convention with the System V AMD64 ABI for `RUX_IS_ELF_OS`.
  * 📄 **[Linux/BSD Instruction Reference (OPCODES.md)](./Low-Level-Doc/OPCodes.md):** Raw x86_64 machine code and ModR/M bytecode cheat sheet for ELF platforms.
  * 📄 **[Windows Thunks (THUNKS_WINDOWS.md)](./Low-Level-Doc/Thunks_Windows.md):** Details regarding native Windows calling convention execution, mandatory Shadow Space, and kernel `R10` swapping rules.
  * 📄 **[Windows Instruction Reference (OPCODES_WINDOWS.md)](./Low-Level-Doc/OPCodes_Windows.md):** Raw bytecode structures mapped specifically to Microsoft x64 target registers.

---

### 🚀 2. Language Reference & Specification

Learn how to write Rux code and track the implementation progress of the compiler pipeline.

* 📄 **[Rux Code Examples (rux_code_examples.md)](./rux/eng/rux_code_examples.md):** The syntax showroom. Contains official code snippets demonstrating how to declare data structures, manage memory, and design applications in Rux.
* 📄 **[Rux Gap Analysis (rux_gap_analysis.md)](./rux/eng/rux_gap_analysis.md):** The compiler engine roadmap. A detailed gap analysis tracking feature completeness, mapping implemented frontend/backend components against the ultimate language specification.
