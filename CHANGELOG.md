# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

<<<<<<< HEAD
=======
## [0.3.0] - 2026-05-31

Major language feature release adding modern ergonomic syntax for lambdas, error handling, optional types, string interpolation, and functional-style pipelines.

### Added

#### Language

- **Lambda expressions** — `|params| expr` and `|params| { body }` syntax for inline closures; zero-parameter lambdas with `|| { body }`; optional return type annotations (`|x: int| -> int32 { ... }`)
- **String interpolation** — `f"Hello, {name}!"` syntax for embedding expressions inside string literals; supports arbitrary expressions (`f"Sum: {a + b}"`) and escape sequences (`\n`, `\t`, `\{`, `\}`)
- **Optional chaining** — `?.` operator for safe member/method access on nullable values (`obj?.field`, `obj?.method()`)
- **Null-coalescing** — `??` operator providing default values for nullable expressions (`value ?? fallback`); right-associative for chaining (`a ?? b ?? c`)
- **Pipeline operator** — `|>` operator for clean function composition chains (`x |> f |> g` desugars to `g(f(x))`); left-associative
- **`try`/`catch`** — structured error handling with `try { } catch e { }` blocks; catch variable captures the error value
- **`defer` statement** — `defer { }` schedules a block to execute when the enclosing scope exits
- **Optional type syntax** — `T?` as sugar for optional/nullable types (e.g., `int32?`, `string?`)

#### Lexer

- New tokens: `?.` (QuestionDot), `??` (QuestionQuestion), `|>` (PipeArrow)
- New keywords: `try`, `catch`, `defer`
- `f"..."` interpolated string literal scanning with `{expr}` placeholder support
- `?.` and `??` are scanned from `?` followed by `.` or `?` respectively

#### Parser

- Pratt parsing extended with new precedence levels for `|>` (pipeline) and `??` (null-coalescing)
- Lambda parsing: `|params| expr` and `|params| { body }` in primary expressions
- Interpolated string parsing: `f"..."` with `{expr}` extraction
- Optional chaining: `?.` in postfix position
- `try`/`catch` and `defer` statement parsing

#### Semantic Analysis

- Type checking for lambda expressions with parameter type inference
- Optional chaining returns `Option<T>` type
- Null-coalescing type unification
- Pipeline operator type resolution (returns function return type)
- Interpolated string returns `Slice<char8>` type
- `try`/`catch` and `defer` block analysis

#### HIR

- Pipeline desugaring: `expr |> func` becomes `func(expr)`
- Lambda lowering to function reference types
- Optional chaining and null-coalescing type propagation
- Try/catch and defer block lowering

#### Tests

- `Tests/Features` — comprehensive test exercising all new v0.3.0 features together
- `Tests/Lambda` — exercises single-expression, block-body, zero-param, multi-param, and higher-order lambdas
- `Tests/Interpolation` — exercises variable interpolation, expression interpolation, and escape sequences
- `Tests/Pipeline` — exercises single, double, and triple pipeline chains
- `Tests/Optional` — exercises `T?` optional types, `?.` optional chaining, and `??` null-coalescing

---

## [0.3.0] - 2026-05-31

Major language feature release adding modern ergonomic syntax for lambdas, error handling, optional types, string interpolation, and functional-style pipelines.

### Added

#### Lambda Expressions
- Closure syntax: `|params| expr` for single-expression lambdas
- Block body lambdas: `|params| { body }`
- Zero-parameter lambdas: `|| { body }`
- Explicit return type annotations: `|x: int32| -> string { ... }`
- Higher-order function support (passing lambdas as arguments)

#### String Interpolation
- f-string syntax: `f"Hello, {name}!"`
- Expression interpolation: `f"Sum: {a + b}"`
- Escape sequences inside interpolated strings: `\n`, `\t`, `\{`, `\}`

#### Optional Chaining
- `?.` operator for safe field access: `obj?.field`
- `?.` operator for safe method calls: `obj?.method()`
- Works on nullable types, returns null if the receiver is null

#### Null-Coalescing
- `??` operator for default values: `value ?? fallback`
- Right-associative chaining: `a ?? b ?? c`

#### Pipeline Operator
- `|>` operator for function composition: `x |> f |> g`
- Desugars to nested calls: `g(f(x))`
- Left-associative, supports arbitrary chain length

#### Try/Catch
- `try { } catch e { }` blocks for structured error handling
- Catch variable receives the error value

#### Defer
- `defer { }` statement for scope-exit cleanup
- Defers execute in LIFO order when scope exits

#### Optional Types
- `T?` syntax sugar for nullable/optional types (e.g., `int32?`, `string?`)

### Changed

- Extended AST with new node types: LambdaExpr, OptionalChainExpr, NullCoalescingExpr, PipelineExpr, InterpolatedStringExpr, TryCatchStmt, DeferStmt, OptionalTypeExpr
- Extended parser with new expression precedence levels for `|>` and `??`
- Extended semantic analysis with type checking for all new features
- Extended HIR with lowering support for all new features
- Lexer now scans `?.`, `??`, `|>` operators and `f"..."` string prefix

### Tests

- `Tests/Lambda` — lambda/closure expression tests
- `Tests/Interpolation` — f-string interpolation tests
- `Tests/Pipeline` — pipeline operator tests
- `Tests/Optional` — optional chaining and null-coalescing tests
- `Tests/TryCatch` — try/catch error handling tests
- `Tests/Defer` — defer statement tests
- `Tests/Features` — comprehensive test combining all new features

---

>>>>>>> 3f33986 (feat: Rux v0.3.0 ΓÇö lambdas, string interpolation, optional chaining, pipeline, try/catch, defer, optional types)
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
