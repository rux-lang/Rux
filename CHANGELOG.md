# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

---

## [0.3.0] - 2026-06-01

Adds broad multi-platform host support, a revamped platform abstraction layer, new language features, a macOS linker backend, Windows DLL output, and many bug fixes.

### Added

#### Language

- **Target attributes** — `#[Target(...)]` attributes to conditionally compile code per platform
- **Unicode escape sequences** — `\u{...}` escapes in string and character literals

#### Linker

- **Windows PE32+ DLL output** — emit `.dll` artifacts when `Type = "Dll"` in `Rux.toml` (export directory, optional `DllMain`)
- **macOS Mach-O linker backend** — native x86-64 Mach-O executable output on macOS

#### Platform

- **OpenBSD x86-64 host** — native compilation and execution on OpenBSD x86-64
- **NetBSD x86-64 host** — native compilation and execution on NetBSD x86-64
- **DragonFly BSD x86-64 host** — native compilation and execution on DragonFly BSD x86-64
- **Illumos x86-64 host** — native compilation and execution on Illumos/OmniOS x86-64
- **Platform abstraction layer** — revamped `Platform` implementation with platform macros and CPU feature detection at runtime
- **BSD and Illumos ELF target support** — correct ELF OSABI, `PT_NOTE`, `ET_DYN` per target
- **Target-specific platform dependencies** — `[target.<triple>.dependencies]` in `Rux.toml`

#### Syscall Thunks

- `nanosleep` and `clock_gettime` typed syscall thunks (Linux)
- `RtlCompareMemory` thunk (Windows)

#### CLI / Package Manager

- `rux check` — type-check the current package without producing a binary
- `rux info [--json]` — display installed package information; `--json` outputs machine-readable JSON
- `rux install --dev` — install a package as a dev dependency
- Inline TOML table dependency fields in `Rux.toml`

#### Build / Infrastructure

- Nix Flake derivation for reproducible builds
- Windows, Linux, macOS, FreeBSD, OpenBSD, NetBSD, DragonFly BSD, and Illumos CI runners
- macOS Homebrew formula and `install` target
- `Tests/Dll` — minimal DLL smoke test and `Tests/run_dll_test.sh` (Windows CI)

### Fixed

- Enums and type aliases not resolving inside `extern` blocks
- Integer `**` (power) operator — defines `__rux_ipow` runtime helper
- Double pointer parsing bug
- Out-of-range integer literals now report a clear diagnostic outside `let` bindings
- Platform dependency resolution via wildcard targets and robust TOML parsing
- `ReadFile`/`WriteFile` thunks: preserve R9 across syscall, guard `mov [r9]` with null check, preserve non-volatile RDI/RSI on Win64
- Entry stack alignment: pre-adjust RSP before `call Main`
- OpenBSD ELF header fixes for `execve` compatibility
- `bool` and `float` type handling regressions
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

- **Lexer** — tokenizes `.rux` source files; reports diagnostics with file, line, and column; supports token stream
  dump (`--dump-tokens`)
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
