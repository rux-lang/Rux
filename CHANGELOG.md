# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Each release groups its entries under **Added**, **Changed**, **Removed**, and **Fixed**, subdivided by area (Language, CLI / Package Manager, Tooling, Platform, Runtime / Linker) when a section is long enough to need it.

## [Unreleased]

Introduces compile-time programming (`when`, `intrinsic`, `#`-prefixed compiler context), native AArch64 host support on every platform, an explicit `let`/`var` mutability model, and a consolidated, hermetic test tree.

### Added

#### Language

- **`when` — conditional compilation** — `when cond { ... } else when cond { ... } else { ... }` evaluates its condition while compiling and keeps only the taken branch; untaken branches are never resolved or type-checked, so they may reference symbols that do not exist on the current build. `if` remains the run-time conditional, and a chain keeps the keyword it opened with — `else if` inside a `when` chain is an error. Conditions are built from `const` declarations, enums, literals, imported compile-time intrinsics, and the usual operators. `when` works between declarations and inside a function body, where it introduces no scope of its own. A condition that is not a compile-time constant, or not a `bool`, is an error.
- **`when` match over a compile-time value** — `when <subject> { pattern => body, ... else => body }` keeps only the matching arm: `when #target.os { .Windows => PrintLine("win"), .Linux => PrintLine("linux"), else => #Error("Unsupported OS") }`. Arm patterns are enum variants or literals, an arm may list several comma-separated patterns, and `else` is the default. An arm body is an expression, a `{ ... }` block, or — between declarations — a declaration or an `#Error`/`#Warn` directive. If no arm matches and there is no `else`, the build is an error.
- **`intrinsic`** — marks a declaration the compiler supplies rather than Rux source: `intrinsic #target: Target;` and `intrinsic func Assert(condition: bool, message: char8[]);`. A `#`-prefixed name marks a compile-time value or function (`#target`, `#Error`); ordinary runtime intrinsics (`Assert`, `Panic`) and methods keep their plain names. An intrinsic value has no initializer and an intrinsic function has no body. Replaces the `#Intrinsic("Name")` attribute and the `$` sigil.
- **Compile-time build context** — importing the Rux package exposes ordinary values for the target (`#target.os`, `.arch`, `.abi`, `.endian`, `.pointerBits`, `.dataModel`, `.objectFormat`, `.triple`, `.HasFeature(...)`), build (`#build.profile`, `.mode`, `.optimization`, `.debugAssertions`, `.debugInfo`, `.isTest`, `.outputKind`, `.timestamp`, `.date`, `.time`), compiler (`#compiler.version`, `.HasFeature(...)`), and source location (`#source.line`, `.column`, `.file`, `.fileName`, `.filePath`, `.function`, `.module`). `#config.Get("name")` and `#config.Has("name")` read user-defined build values.
- **`#target.os` and the `OperatingSystem` enum** — `#target.os` is a compile-time value of the built-in `OperatingSystem` enum, so a build selects platform code with `when #target.os == .Windows { ... }`. Each variant is a system rather than a family — `Windows`, `Linux`, `MacOS`, `FreeBSD`, `OpenBSD`, `NetBSD`, `DragonFlyBSD`, `Illumos`, `Solaris`, `Android`, `IOS`, `AIX`, `Haiku`, `Fuchsia`, `QNX`, `Redox` — so the BSDs are told apart. A variant may be written as `Enum::Variant` or as the `.Variant` shorthand when the other side fixes the enum. A variant the enum does not have is an error; one no build target can currently produce is a warning. A condition may only name an intrinsic or built-in enum the file has brought into scope.
- **`#Error` and `#Warn` compile-time directives** — `#Error("message")` and `#Warn("message")` emit a diagnostic at the call site and produce no runtime code, so a build can reject an unsupported configuration: `when #target.os == .Windows { #Error("Windows is not supported yet"); }`. They run after `when` folds, so a directive in an untaken branch never fires. The message must be a string literal.
- **Intrinsic assertions** — importing `Assert` or `DebugAssert` from the Rux package enables compiler-backed assertions that report the message, function, file, line, and column before trapping. Release builds remove `DebugAssert` checks without evaluating their arguments.
- **Intrinsic panic and no-return functions** — importing `Panic` from the Rux package enables compiler-backed panic reporting and termination. `#NoReturn()` marks user or extern functions that never return and makes calls terminate their control-flow path without adding a new type or keyword.
- **`byte` primitive alias** — `byte` is the raw-storage spelling of `uint8`, with identical representation, ABI, constants, conversions, and overload identity. Memory operations use it for byte-addressable views while numeric code can retain the explicit `uint8` spelling.
- **Primitive associated constants** — Boolean, character, integer, and floating-point types expose their storage width through `Bits` and `Bytes`. Integers and characters also expose `Min` and `Max`; floating-point types expose `Lowest`, `Max`, `MinPositive`, `Epsilon`, `Infinity`, and `NaN`. The `bool`, `char`, and `float` aliases forward to their canonical widths, while `int` and `uint` follow the compilation target.
- **Future primitive type reservation** — documented primitive names whose representations are not implemented yet, such as `int128`, `float16`, `bool64`, and `char128`, are reserved and produce a compiler-version diagnostic instead of being reported as unknown types.
- **Logical right shift** — signed integers support `>>>` and `>>>=` to shift in zero bits while preserving the left operand's type and fixed width; `>>` remains arithmetic for signed integers and logical for unsigned integers.
- **Whole-module import** — `import Pkg;` binds a dependency's eponymous `module Pkg` as a namespace, so its members are used through `Pkg::Name` (e.g. `import Windows;` then `Windows::HeapAlloc(...)`). A package that exposes no same-named module still requires naming an item, and `import Pkg::Module` / `import Pkg::{ A, B }` / `import Pkg::*` are unchanged.
- **Inline assembly functions** — `asm func Name(...) -> T { ... }` bodies are written directly in x86-64 (Intel syntax) and assembled to machine code, bypassing the normal HIR/LIR pipeline. Supports the common instruction subset (ALU ops, `mov`/`lea`, `movzx`/`movsx`, the multiply/divide group, shifts, `push`/`pop`, `call`/`jmp`, the full `jcc`/`setcc` family, `ret`/`leave`/`nop`/`syscall`), register/immediate/memory operands, local labels, and calls to other functions. Only x86-64 targets can build an `asm func`; compiling one for AArch64 is a diagnostic.
- **Targeted lint allowances** — `#Allow("naming.type")` preserves intentional foreign type, field, and variant spellings on a single type declaration without disabling naming checks elsewhere.

#### CLI / Package Manager

- **Compile-time configuration** — `[Build.Defines]` supplies string values to `config`, and `--define NAME[=VALUE]` overrides them for `build`, `check`, `run`, and `test`. Date/time compiler parameters share one UTC build timestamp and honor `SOURCE_DATE_EPOCH` for reproducible builds.
- **Manifest-less workspace installation** — bare `rux install` now discovers package manifests below the root `Tests/` tree, immediate member packages, and member `Tests/` trees when no root `Rux.toml` exists. Registry dependencies are deduplicated and installed through the existing transitive resolver, so repository test setup no longer needs a hard-coded package list.
- **`rux uninstall --global`** — remove every package from the global cache, whether or not the current `Rux.toml` declares it. Completes the `--global` set alongside `rux list --global` and `rux update --global`.

#### Tooling

- **AArch64 host support** — FreeBSD, Linux, macOS, and Windows now build and run the compiler natively on AArch64, with architecture-correct target selection, platform-aware lowering, and full CI build/test coverage alongside x86-64.
- **C++ static analysis** — `.clang-tidy` defines an enforced, high-signal Clang 22 baseline for compiler and unit-test translation units. The portable test workflows run the analysis in parallel from CMake's compilation database when passed `-ClangTidy` or `--clang-tidy`, and the lint CI workflow always rejects new findings.
- **Portable repository scripts** — `Build.sh` and `Test.sh` provide the same build and complete verification entry points as their PowerShell counterparts on Linux, macOS, and FreeBSD.
- **Source formatting scripts** — `Format.ps1` and `Format.sh` format or check all maintained C++ and Rux sources while excluding vendored code and malformed diagnostic fixtures.

### Changed

#### Language

- **Explicit binding and pointee mutability** — `let` bindings and parameters are immutable by default, while `var` declares a mutable binding or parameter. Pointer pointee mutability is written `*T` (read-only) or `*var T` (writable), and `@place` infers the pointer form from the addressed place. This replaces `let mut`, `*mut T`, and `@mut`.
- **Fully-qualified item imports** — importing an item now requires its complete `Package::Module::…::Item` path; the shortcut that let a bare `import Package::Item` reach into a package's same-named module is gone. `import Memory::Alloc` is an error (with a `did you mean 'import Memory::Memory::Alloc'?` hint) — the package name is always the first segment and the containing module must be named. Items declared at a package's root (`import Rux::{ #target }`) are unaffected.
- **Extern import syntax** — `#Link("Kernel32.dll")` imports an extern function under its Rux declaration name, while an optional second argument such as `#Link("Kernel32.dll", "Beep")` names a different exported DLL symbol. The one-argument form also applies a library to an extern block. `#Library` and `#Symbol` remain compatibility spellings but cannot be combined with `#Link`.
- **`#`-prefixed compiler context and PascalCase target variants** — compiler-provided values are now `#target`, `#build`, `#compiler`, `#config`, and `#source`. Target enum variants use unambiguous spellings such as `IOS`, `X86_64`, and `RiscvLp64`; Windows ISO code pages use names such as `Iso8859Part1`.
- **Native Linux calling convention** — ordinary and inline-assembly Rux functions now use the System V AMD64 ABI on Linux, including `rdi`/`rsi`/`rdx`/`rcx`/`r8`/`r9` integer arguments, 16-byte call-site stack alignment, and stack-passed overflow arguments. `#Abi(.Win64)` remains available as an explicit override.

#### CLI / Package Manager

- **Workspace test discovery** — `rux test` at a workspace root now discovers each member package's `Tests/` directory in addition to the root `Tests/`, so test packages can live beside the code they cover. Tests found under a member are labeled with the member's name (`Text/Tests/Compare` reports as `Text/Compare`), and a root `Tests/` keeps working as before.
- **Flat test executable output** — `rux test` writes each executable directly to its configured `[Build].Output` directory instead of adding a `Debug` or `Release` subdirectory. Ordinary `rux build` and `rux run` outputs remain profile-specific.

#### Tooling

- **Hardened CI and releases** — all workflows use current action versions, least-privilege permissions, credential-free checkouts, bounded timeouts, consistent `x86_64` artifact names, and seven-day intermediate artifact retention. Every platform now runs C++ unit tests plus Rux check, lint, and test verification; release assets include SHA-256 checksums.
- **CMake 3.30 baseline** — Rux now accepts CMake 3.30 or newer while retaining policy behavior through CMake 4.3. FreeBSD CI uses the packaged CMake 3.31 release instead of spending more than 20 minutes building CMake from source.
- **Canonical architecture names** — documentation and CI labels use `x86-64` and `AArch64`; machine-readable target triples and artifacts use `x86_64` and `aarch64`; C++ and Rux architecture enums use `X86_64` and `AArch64`. The CLI still accepts `x64`, `amd64`, and `arm64` target suffixes as compatibility aliases and normalizes them before compilation.
- **Hermetic consolidated tests** — repository tests are organized as language, package, unit/golden, and policy suites below `Tests/`. Every Rux test dependency is an explicit local path, while workspace-member overrides resolve transitive first-party dependencies without registry access.

### Removed

- **Metadata blocks** — every `#{...}` declaration metadata block is now an error. Calling conventions use `#Abi(.C)`, `#Abi(.SysV)`, or `#Abi(.Win64)`; platform selection uses conditional compilation such as `when #target.os == .Windows { ... }`.
- **Compiler-parameter sigils** — the `$`-sigil compiler parameters, their flat aliases, and the `#Intrinsic("Name")` attribute are gone. Compile-time values are `#`-prefixed and declared with the `intrinsic` keyword.
- **Target-specific dependency sections** — `Rux.toml` no longer recognizes `[Dependencies.Target.<OS>]` (or the legacy `[Target.<OS>.Dependencies]`); only a single `[Dependencies]` table is allowed. Platform selection belongs in source with conditional compilation.

### Fixed

- **Constants of slice type** — `const Name: T[N] = [...]` and `const Name: char8[] = "..."` are now emitted into read-only data with a `{data, length}` header, instead of an eight-byte placeholder that was never filled in. Reading, indexing, iterating, or taking the `.length` of such a constant crashed at runtime. Elements may be literals, negated literals, or other named constants; anything the backend cannot lay out is now a compile error rather than a corrupt read.
- **Module scope in call resolution** — a call now binds to a function from its own module or from a module the file imports, in preference to a same-named function declared elsewhere in the program. Functions in different modules are no longer treated as overloads of one another, and two modules that would emit the same symbol are disambiguated by their module path, so importing one package could silently redirect a call into another.
- **Duplicate function signatures** — declaring the same function or method signature twice now reports the second declaration as an error. Functions with distinct parameter types remain valid overloads, and same-named functions in separate modules remain independent.
- **Dependency compilation** — `rux check` and `rux build` now share the same dependency-loading pipeline, so installed and path-based dependencies are available during semantic analysis and unresolved dependencies stop before code generation.
- **Call-site diagnostics on extern functions** — `#Error("...")` and `#Warn("...")` now emit their diagnostics when an extern function is called, including through module-qualified names, instead of being discarded after parsing.
- **Attributes on extend methods** — attribute calls such as `#Abi(...)` are accepted on methods inside an `extend` block instead of failing with "expected 'func' in extend body" (#197).
- **Zero-operand assembly parsing** — instructions such as `syscall` followed by `ret` are now parsed as two instructions instead of treating the second mnemonic as an operand.
- **Windows MSI license packaging** — the installer now packages the canonical `LICENSE.md` file and displays the same copyright notice as the repository license.
- **Repository integration consistency** — FreeBSD package metadata now declares its `Rux` dependency, macOS scripts locate Homebrew's versioned LLVM 22 tools, CI enforces Rux formatting, Nix runs CTest from the correct build directory, and documentation consistently uses the `Build/` directory and centralized test workflow.

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
- **Platform abstraction layer** — revamped `Platform` implementation with platform macros and CPU feature detection at runtime
- **BSD ELF target support** — correct ELF OSABI, `PT_NOTE`, `ET_DYN` per target
- **Target-specific platform dependencies** — `[Target.<Platform>.Dependencies]` in `Rux.toml`
- **Syscall thunks** — `nanosleep` and `clock_gettime` typed thunks (Linux); `RtlCompareMemory` thunk (Windows)

#### CLI / Package Manager

- `rux check` — type-check the current package without producing a binary
- `rux info [--json]` — display installed package information; `--json` outputs machine-readable JSON
- `rux install --dev` — install a package as a dev dependency
- Inline TOML table dependency fields in `Rux.toml`

### Fixed

#### Language

- **Overload resolution** now hard-errors on unresolved overloads instead of silently falling back
- **Bare integer literals** now work in single-overload resolution
- **Binary expression operands** are now type-checked on both sides
- **`is` folding** now produces a compile-time boolean instead of emitting a fake call
- **Tuple size / field offset layout** is now aligned consistently across backends
- **Pointer arithmetic** now scales by element size
- **Slice-of-slice assignment** no longer corrupts the slice length field
- **Compatibility checks** for mixed character and integer types are relaxed where appropriate
- **Platform-conditional imports** under `@[Target(...)]` are filtered correctly during dependency collection
- **Constant casts** now reject out-of-range character values and Unicode surrogate code points
- **Out-of-range integer literals** now produce clearer diagnostics outside `let` bindings
- **Unsigned/sized integer literal handling** is now consistent across decimal, hex, binary, and octal forms
- Enums and type aliases not resolving inside `extern` blocks
- `bool` and `float` type handling regressions
- Double pointer parsing bug

#### Runtime / Linker

- **Integer `**` (power) operator** — defines the `__rux_ipow` runtime helper
- **SysV stack argument passing** is restored in codegen
- **Entry stack alignment** — pre-adjust RSP before `call Main`
- **`ReadFile`/`WriteFile` thunks** — preserve R9 across syscall, guard `mov [r9]` with a null check, preserve non-volatile RDI/RSI on Win64

#### Platform / Tooling

- **Windows CMake linking** issues are fixed
- **GCC/MinGW** terminal-link workaround is added
- **Install/uninstall** now work outside project directories
- **Help command handling** is cleaner, more robust, and terminal-width aware
- **Type dependency resolution** and wildcard target handling are fixed
- Platform dependency resolution via wildcard targets and robust TOML parsing
- OpenBSD ELF header fixes for `execve` compatibility
- OpenBSD test cases are removed where they no longer apply
- Windows `std::max` macro conflict with compiler internals
- UB in `gitclone` due to a missing `return`

## 0.2.2 - 2026-05-28

Expands the package manager CLI, adds Linux and FreeBSD host support, and fixes several compiler bugs.

### Added

#### CLI / Package Manager

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

## 0.2.0 - 2026-05-10

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

#### CLI / Package Manager

- `rux build` now prints build statistics (files, lines, time) after a successful build

### Fixed

- Dependency resolution error when packages referenced each other
- Incorrect code generation for `if` conditions
- Type checking regressions in slices, function calls, and pointer arithmetic

## 0.1.0 - 2026-04-30

Initial release of the Rux compiler and package manager.

> **Note:** This release supports compiling simple `Main` functions with arithmetic return expressions only. Full language features are not yet implemented.

```rux
func Main() -> int32 {
    return 10 + 2 * (5 - 3);
}
```

### Added

#### Compiler Pipeline

- **Lexer** — tokenizes `.rux` source files; reports diagnostics with file, line, and column; supports token stream dump (`--dump-tokens`)
- **Parser** — produces an AST from the token stream; supports AST dump (`--dump-ast`)
- **Semantic analysis** — type checking and name resolution; supports analysis dump (`--dump-sema`)
- **HIR** — high-level intermediate representation lowered from the AST
- **LIR** — low-level intermediate representation; supports dump (`--dump-lir`)
- **ASM** — x86-64 assembly emitter; supports dump (`--dump-asm`)
- **RCU** — native object file emitter; supports dump (`--dump-rcu`)
- **Linker** — links RCU object files into a native executable

#### CLI / Package Manager

- `rux build` — compile the current package; supports only `--dump-*` flags
- `rux run` — build and execute the package binary
- `rux new <name>` — scaffold a new package in a new directory only (`--bin` / `--lib`)
- `rux version` — print compiler version and build timestamp
- `rux help [command]` — show help for a command
- `-V` / `--version` — global flag that prints the version

[Unreleased]: https://github.com/rux-lang/Rux/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/rux-lang/Rux/releases/tag/v0.3.0
