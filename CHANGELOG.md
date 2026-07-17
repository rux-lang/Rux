# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

#### Language

- **Intrinsic assertions** — importing `Assert` or `DebugAssert` from the Rux package enables compiler-backed assertions that report the message, function, file, line, and column before trapping. Release builds remove `DebugAssert` checks without evaluating their arguments.
- **Intrinsic panic and no-return functions** — importing `Panic` from the Rux package enables compiler-backed panic reporting and termination. `#NoReturn()` marks user or extern functions that never return and makes calls terminate their control-flow path without adding a new type or keyword.
- **Primitive associated constants** — Boolean, character, integer, and floating-point types expose their storage width through `Bits` and `Bytes`. Integers and characters also expose `Min` and `Max`; floating-point types expose `Lowest`, `Max`, `MinPositive`, `Epsilon`, `Infinity`, and `NaN`. The `bool`, `char`, and `float` aliases forward to their canonical widths, while `int` and `uint` follow the compilation target.
- **Future primitive type reservation** — documented primitive names whose representations are not implemented yet, such as `int128`, `float16`, `bool64`, and `char128`, are reserved and produce a compiler-version diagnostic instead of being reported as unknown types.
- **`intrinsic`** — marks a declaration the compiler supplies rather than Rux source: `intrinsic const target: Target;` and `intrinsic func Assert(condition: bool, message: char8[]);`. The declaration names the intrinsic, so nothing is written twice: a constant takes its type (`Target`), a free function its own name (`Assert`), and a method the type it extends (`extend Config { intrinsic func Get(...); }` is `Config.Get`). An intrinsic constant has no initializer and an intrinsic function has no body. This replaces the `#Intrinsic("Name")` attribute and the `$` sigil, which together marked the same thing three ways and had to be kept in agreement.
- **`when` — conditional compilation** — `when cond { ... } else when cond { ... } else { ... }` evaluates its condition while compiling and keeps only the taken branch; the others are never resolved or type-checked, so they may reference symbols that do not exist on the current build. `if` is the run-time conditional and `when` the compile-time one, and a chain keeps the keyword it opened with: `else if` inside a `when` chain is an error. Conditions are built from `const` declarations, enums, literals, imported compiler parameters such as `target.os`, and the usual operators (`!`, `&&`, `||`, comparisons, arithmetic). It works both between declarations and inside a function body, where it introduces no scope of its own — a binding it makes is visible to the statements that follow. A condition that is not a compile-time constant, or not a `bool`, is an error.
- **`target.os` and the `OperatingSystem` enum** — `target.os` is a compile-time value of the built-in `OperatingSystem` enum, so a build selects its platform code with `when target.os == OperatingSystem::Windows { ... }`. Each variant is a system, not a family: `Windows`, `Linux`, `MacOS`, `FreeBSD`, `OpenBSD`, `NetBSD`, `DragonFlyBSD`, `Illumos`, `Solaris`, `Android`, `iOS`, `AIX`, `Haiku`, `Fuchsia`, `QNX`, `Redox` — so the BSDs are told apart. Enum variants in a `when` condition are always written in full as `Enum::Variant`, including for the program's own enums (`when Build == Mode::Fast`); the bare `.Variant` shorthand is not accepted. A variant the enum does not have is an error, and one no build target can currently produce (`OperatingSystem::Haiku`) is a warning, rather than a branch that quietly never runs. A condition may only name a Rux build intrinsic (`target`, `build`, …) or built-in enum (`OperatingSystem`, `Architecture`, …) that the file has brought into scope — `import Rux::{ target, OperatingSystem };` or a local `intrinsic` declaration; otherwise it is an `unknown identifier` error.
- **Compile-time build context** — importing the Rux package exposes ordinary values for the target (`target.os`, `.arch`, `.abi`, `.endian`, `.pointerBits`, `.dataModel`, `.objectFormat`, `.triple`, `.HasFeature(...)`), build (`build.profile`, `.mode`, `.optimization`, `.debugAssertions`, `.debugInfo`, `.isTest`, `.outputKind`, `.timestamp`, `.date`, `.time`), compiler (`compiler.version`, `.HasFeature(...)`), and source location (`source.line`, `.column`, `.file`, `.fileName`, `.filePath`, `.function`, `.module`). `config.Get("name")` and `config.Has("name")` read user-defined build values. Package authors declare these with the `intrinsic` keyword — `intrinsic const target: Target;` — and the compiler supplies the value. The old compiler-parameter sigils and flat aliases have been removed.

- **Whole-module import** — `import Pkg;` binds a dependency's eponymous `module Pkg` as a namespace, so its members are used through `Pkg::Name` (e.g. `import Windows;` then `Windows::HeapAlloc(...)`). A package that exposes no same-named module still requires naming an item (`import Pkg::Name`), and `import Pkg::Module` / `import Pkg::{ A, B }` / `import Pkg::*` are unchanged.

- **Inline assembly functions** — `asm func Name(...) -> T { ... }` bodies are written directly in x86-64 (Intel syntax) and assembled to machine code, bypassing the normal HIR/LIR pipeline. Supports the common instruction subset (ALU ops, `mov`/`lea`, `movzx`/`movsx`, the multiply/divide group, shifts, `push`/`pop`, `call`/`jmp`, the full `jcc`/`setcc` family, `ret`/`leave`/`nop`/`syscall`), register/immediate/memory operands, local labels, and calls to other functions.

#### CLI / Package Manager

- **Compile-time configuration** — `[Build.Defines]` supplies string values to `config`, and `--define NAME[=VALUE]` overrides them for `build`, `check`, `run`, and `test`. Date/time compiler parameters share one UTC build timestamp and honor `SOURCE_DATE_EPOCH` for reproducible builds.

- `rux uninstall --global` — remove every package from the global cache, whether or not the current `Rux.toml` declares it. Completes the `--global` set alongside `rux list --global` and `rux update --global`.

### Changed

- **Fully-qualified item imports** — importing an item now requires its complete `Package::Module::…::Item` path; the shortcut that let a bare `import Package::Item` reach into a package's same-named module is gone. `import Memory::Alloc` is an error (with a `did you mean 'import Memory::Memory::Alloc'?` hint) — the package name is always the first segment and the containing module must be named. Items declared at a package's root (`import Rux::{ target }`) are unaffected.
- **Extern import syntax** — `#Link("Kernel32.dll")` imports an extern function under its Rux declaration name, while an optional second argument such as `#Link("Kernel32.dll", "Beep")` names a different exported DLL symbol. The one-argument form also applies a library to an extern block. `#Library` and `#Symbol` remain compatibility spellings but cannot be combined with `#Link`.
- **Metadata blocks removed** — every `#{...}` declaration metadata block is now an error. Calling conventions use `#Abi(.C)`, `#Abi(.SysV)`, or `#Abi(.Win64)`; platform selection uses conditional compilation such as `when target.os == OperatingSystem::Windows { ... }`.

- **Workspace test discovery** — `rux test` at a workspace root now discovers each member package's `Tests/` directory in addition to the root `Tests/`, so test packages can live beside the code they cover. Tests found under a member are labeled with the member's name (`Text/Tests/Compare` reports as `Text/Compare`), and a root `Tests/` keeps working as before.
- **Native Linux calling convention** — ordinary and inline-assembly Rux functions now use the System V AMD64 ABI on Linux, including `rdi`/`rsi`/`rdx`/`rcx`/`r8`/`r9` integer arguments, 16-byte call-site stack alignment, and stack-passed overflow arguments. `@[Call(.Win64)]` remains available as an explicit override.

### Removed

- **Target-specific dependency sections** — `Rux.toml` no longer recognizes `[Dependencies.Target.<OS>]` (or the legacy `[Target.<OS>.Dependencies]`); only a single `[Dependencies]` table is allowed. Platform selection belongs in source with conditional compilation such as `when target.os == OperatingSystem::Windows { ... }`.

### Fixed

- **Duplicate function signatures** — declaring the same function or method signature twice now reports the second declaration as an error. Functions with distinct parameter types remain valid overloads, and same-named functions in separate modules remain independent.
- **Call-site diagnostics on extern functions** — `#Error("...")` and `#Warn("...")` now emit their diagnostics when an extern function is called, including through module-qualified names, instead of being discarded after parsing.
- **Dependency compilation** — `rux check` and `rux build` now share the same dependency-loading pipeline, so installed and path-based dependencies are available during semantic analysis and unresolved dependencies stop before code generation.
- **Zero-operand assembly parsing** — instructions such as `syscall` followed by `ret` are now parsed as two instructions instead of treating the second mnemonic as an operand.
- **Attributes on extend methods** — attribute calls such as `#Abi(...)` are accepted on methods inside an `extend` block instead of failing with "expected 'func' in extend body" (#197).
- **Module scope in call resolution** — a call now binds to a function from its own module or from a module the file imports, in preference to a same-named function declared elsewhere in the program. Functions in different modules are no longer treated as overloads of one another, and two modules that would emit the same symbol are disambiguated by their module path, so importing one package could silently redirect a call into another.
- **Constants of slice type** — `const Name: T[N] = [...]` and `const Name: char8[] = "..."` are now emitted into read-only data with a `{data, length}` header, instead of an eight-byte placeholder that was never filled in. Reading, indexing, iterating or taking the `.length` of such a constant crashed at runtime. Elements may be literals, negated literals, or other named constants; anything the backend cannot lay out is now a compile error rather than a corrupt read.

## [0.3.0] - 2026-06-23

Adds broad multi-platform host support, a revamped platform abstraction layer, new language features, a macOS linker backend, and Windows DLL output, along with correctness fixes, broader literal and constant-expression support, improved overload resolution, expanded runtime support, better test coverage, cleaner CLI/build tooling, and many bug fixes.

### Added

#### Language

- **Target attributes** — `@[Target(...)]` attributes to conditionally compile code per platform
- **Unicode escape sequences** — `\u{...}` escapes in string and character literals
- **Constant integer expression coercion** — compile-time folded integer expressions now coerce to sized integer targets when the value fits
- **Typed non-decimal integer suffixes** — `0xFFu`, `0b1010u`, `0o17i`, and underscore separators in non-decimal literals
- **Constant character cast validation** — compile-time validation for `as char8`, `as char16`, and `as char32`
- **Boolean bitwise operators** — `&`, `|`, `^`, and `~` on `bool` types
- **Attribute handling improvements** — `@[Target(...)]` import filtering by platform, plus warning and error attribute support (`@[Warn(...)]`, `@[Error(...)]`)

#### Runtime / Linker

- **Windows PE32+ DLL output** — emit `.dll` artifacts when `Type = "Dll"` in `Rux.toml` (export directory, optional `DllMain`)
- **macOS Mach-O linker backend** — native x86-64 Mach-O executable output on macOS
- **macOS `munmap` thunk** — adds `munmap` support to the Mach-O linker so `Std::Memory::Free` can release mmap-backed allocations
- **Floating-point remainder support** — adds FP `%` handling
- **Floating-point comparison fixes** — correct FP comparison behavior

#### Platform

- **OpenBSD x86-64 host** — native compilation and execution on OpenBSD x86-64
- **NetBSD x86-64 host** — native compilation and execution on NetBSD x86-64
- **DragonFly BSD x86-64 host** — native compilation and execution on DragonFly BSD x86-64
- **Illumos x86-64 host** — native compilation and execution on Illumos/OmniOS x86-64
- **Platform abstraction layer** — revamped `Platform` implementation with platform macros and CPU feature detection at runtime
- **BSD and Illumos ELF target support** — correct ELF OSABI, `PT_NOTE`, `ET_DYN` per target
- **Target-specific platform dependencies** — `[Target.<Platform>.Dependencies]` in `Rux.toml`

#### Syscall Thunks

- `nanosleep` and `clock_gettime` typed syscall thunks (Linux)
- `RtlCompareMemory` thunk (Windows)

#### CLI / Package Manager

- `rux check` — type-check the current package without producing a binary
- `rux info [--json]` — display installed package information; `--json` outputs machine-readable JSON
- `rux install --dev` — install a package as a dev dependency
- Inline TOML table dependency fields in `Rux.toml`

### Fixed

#### Semantics / Type Checking

- **Overload resolution** now hard-errors on unresolved overloads instead of silently falling back
- **Bare integer literals** now work in single-overload resolution
- **Binary expression operands** are now type-checked on both sides
- **`is` folding** now produces a compile-time boolean instead of emitting a fake call
- **Tuple size / field offset layout** is now aligned consistently across backends
- **Pointer arithmetic** now scales by element size
- **Slice-of-slice assignment** no longer corrupts the slice length field
- **Compatibility checks** for mixed character and integer types are relaxed where appropriate
- **Platform-conditional imports** under `@[Target(...)]` are filtered correctly during dependency collection
- Enums and type aliases not resolving inside `extern` blocks
- `bool` and `float` type handling regressions
- Double pointer parsing bug

#### Language Diagnostics

- **Constant casts** now reject out-of-range character values and Unicode surrogate code points
- **Out-of-range integer literals** now produce clearer diagnostics outside `let` bindings
- **Unsigned/sized integer literal handling** is now consistent across decimal, hex, binary, and octal forms

#### Codegen / Runtime

- Integer `**` (power) operator — defines `__rux_ipow` runtime helper
- **SysV stack argument passing** is restored in codegen
- Entry stack alignment: pre-adjust RSP before `call Main`
- `ReadFile`/`WriteFile` thunks: preserve R9 across syscall, guard `mov [r9]` with null check, preserve non-volatile RDI/RSI on Win64

#### Platform / Build

- **Windows CMake linking** issues are fixed
- **GCC/MinGW** terminal-link workaround is added
- **Install/uninstall** now work outside project directories
- **Help command handling** is cleaner, more robust, and terminal-width aware
- **Type dependency resolution** and wildcard target handling are fixed
- Platform dependency resolution via wildcard targets and robust TOML parsing
- OpenBSD ELF header fixes for `execve` compatibility
- **OpenBSD test cases** are removed where they no longer apply
- Windows `std::max` macro conflict with compiler internals
- UB in `gitclone` due to missing `return`

---

## [0.2.2] - 2026-05-28

Expands the package manager CLI, adds Linux and FreeBSD host support, and fixes several compiler bugs.

### Added

#### CLI

- `rux install [package][@version]` — install a package into the current project
- `rux uninstall [package]` — remove a package from the current project
- `rux list [--global]` — list installed packages
- `rux update [--global]` — update packages to their latest versions
- `rux add --path <path>` — add a local package dependency by path

#### Platform

- **Linux x86-64 host** — native compilation and execution on Linux x86-64
- **FreeBSD x86-64 host** — native compilation and execution on FreeBSD x86-64
- Linux syscall thunks for I/O (`ReadFile`, stdin support via `GetStdHandle`)

### Fixed

- Parsing bugs with the `as` keyword
- Compiler bugs with `const` declarations, `import` statements, and calling conventions
- Incorrect handling of integer literals with suffixes (`10i`, `10u`) and range expressions (`0..10u`)
- `rux add` crash when specifying an unknown package name

---

## [0.2.0] - 2026-05-10

Expands the compiler with control flow, composite types, modules, and a richer type system.

### Added

#### Language

- **Control flow** — `if`, `for`, `while`, `do-while` statements
- **`sizeof` operator** — returns the byte size of a type
- **Slices** — variable-length views over contiguous memory
- **Tuples** — fixed-size anonymous product types
- **Enums** — named sum types
- **Interfaces** — structural contracts for types
- **`extend` blocks** — method implementations for types
- **`module` keyword** — declares the module a source file belongs to
- **Function overloading** — multiple functions with the same name and different signatures
- **Function imports** — call functions from other modules
- **Packages** — multi-file compilation units with dependency resolution

#### CLI

- `rux build` now prints build statistics (files, lines, time) after a successful build

### Fixed

- Dependency resolution error when packages referenced each other
- Incorrect code generation for `if` conditions
- Type checking regressions in slices, function calls, and pointer arithmetic

---

## [0.1.0] - 2026-04-30

Initial release of the Rux compiler and package manager.

> **Note:** This release supports compiling simple `Main` functions with arithmetic return expressions only. Full language features are not yet implemented.

```rux
func Main() -> int32 {
    return 10 + 2 * (5 - 3);
}
```

### Compiler pipeline

- **Lexer** — tokenizes `.rux` source files; reports diagnostics with file, line, and column; supports token stream dump (`--dump-tokens`)
- **Parser** — produces an AST from the token stream; supports AST dump (`--dump-ast`)
- **Semantic analysis** — type checking and name resolution; supports analysis dump (`--dump-sema`)
- **HIR** — high-level intermediate representation lowered from the AST
- **LIR** — low-level intermediate representation; supports dump (`--dump-lir`)
- **ASM** — x86-64 assembly emitter; supports dump (`--dump-asm`)
- **RCU** — native object file emitter; supports dump (`--dump-rcu`)
- **Linker** — links RCU object files into a native executable

### CLI commands

- `rux build` — compile the current package; supports only `--dump-*` flags
- `rux run` — build and execute the package binary
- `rux new <name>` — scaffold a new package in a new directory only (`--bin` / `--lib`)
- `rux version` — print compiler version and build timestamp
- `rux help [command]` — show help for a command

### Global flags

- `-V` / `--version` — print version
