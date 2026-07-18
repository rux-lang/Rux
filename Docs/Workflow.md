# Development Workflow

The day-to-day loop for changing the Rux compiler. For where your branch should come from and where it goes, see [Branch Architecture](Branches.md).

## 1. Prerequisites

Install the toolchain (Clang 22.1+, CMake 3.31+, Ninja 1.13+, and a recent Git) for your OS — see [Building from Source](../README.md#building-from-source).

Beyond that build toolchain you need **`clang-format`** (ships with LLVM/Clang) to format your changes — CI and reviewers expect formatted diffs. No other tool is required:

- **`clang-tidy`** is _not_ used; there is no `.clang-tidy` in the repo.
- **Editor / IDE** setup is optional. Pass `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` when configuring (as shown below) to generate `build/compile_commands.json` for `clangd` and other LSP clients.

## 2. Get the source and configure

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRUX_BUILD_TESTS=ON
```

With no `-DCMAKE_BUILD_TYPE`, the project defaults to a **Release** build (set in `CMakeLists.txt`). See [Debug vs. Release](#debug-vs-release-builds) below for when to pick the other.

## 3. The inner loop

1. **Branch** off `dev` (see [Branch Architecture](Branches.md)).
2. **Edit** sources under `Compiler/<Component>/` (e.g. `Compiler/Syntax/`, `Compiler/Semantic/`, or `Compiler/CodeGen/`); headers live next to their `.cpp` files.
3. **Build** the incremental build:
   ```sh
   cmake --build build --config Release
   ```
4. **Run / debug** the compiler you just built:
   ```sh
   ./Bin/Release/rux help        # Windows: .\Bin\Release\rux.exe help
   ```
5. **Install test dependencies and test** (see below):
   ```sh
   ./Bin/Release/rux install
   ./Bin/Release/rux test --release
   ctest --test-dir build --output-on-failure
   ```
6. **Format** touched files: `clang-format -i <files>`.

### Debug vs. Release builds

- **Release** (default) — optimized; this is what CI builds and tests, and what ships. Use it for normal development and before pushing.
- **Debug** — configure a _separate_ build directory with `-DCMAKE_BUILD_TYPE=Debug` (e.g. `cmake -S . -B build-debug -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DRUX_BUILD_TESTS=ON`). It is unoptimized and includes debug information, so breakpoints and stepping in a debugger (LLDB/GDB) behave predictably.

There are **no sanitizer presets** wired into `CMakeLists.txt`. If you want ASan/UBSan, pass the flags yourself on a throwaway build dir, e.g. `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`.

## 4. Repository layout

| Path                  | Purpose                                                                              |
| --------------------- | ------------------------------------------------------------------------------------ |
| `Compiler/Cli/`       | Command-line interface and terminal presentation                                     |
| `Compiler/Driver/`    | Compilation orchestration, targets, and build reports                                |
| `Compiler/Source/`    | Source discovery, loading, and locations                                             |
| `Compiler/Lexer/`     | Tokens and lexical analysis                                                          |
| `Compiler/Syntax/`    | AST and parser                                                                       |
| `Compiler/Semantic/`  | Types, symbols, analysis, and the persistent semantic model                          |
| `Compiler/Ir/`        | HIR/LIR models, printers, and IR-local passes                                        |
| `Compiler/Lowering/`  | Explicit AST-to-HIR and HIR-to-LIR transformations                                   |
| `Compiler/CodeGen/`   | Target-specific code generation                                                      |
| `Compiler/Object/`    | Object formats and serialization                                                     |
| `Compiler/Linker/`    | PE, ELF, and Mach-O executable writers                                               |
| `Compiler/Target/`    | Compilation target, ABI, data-layout, and architecture model                         |
| `Compiler/System/`    | Host OS, hardware, filesystem, and process services                                  |
| `Compiler/Package/`   | Manifest parsing and package management                                              |
| `Compiler/Formatter/` | Formatting engine used by `rux fmt`                                                  |
| `Compiler/Linter/`    | Lint engine and rules used by `rux lint`                                             |
| `Tests/Integration/`  | Rux language test packages, one per subdirectory (`rux test`)                        |
| `Tests/Unit/`         | C++ unit tests for compiler internals (doctest + CTest)                              |
| `Tests/Golden/`       | Golden diagnostics cases: `<Case>.rux` + `<Case>.expected`                           |
| `Tools/`              | Repo maintenance scripts (e.g. the platform-isolation guard)                         |
| `Packaging/`          | Linux and Windows installer scripts plus the Windows MSI project                     |
| `Bin/`                | Output for the compiler (`Bin/<Config>/`) and compiled Rux packages; **git-ignored** |
| `CMakeLists.txt`      | Top-level build entry: project `VERSION` + `add_subdirectory(Compiler)`              |

`Bin/` is where `rux` drops the binaries it produces. Test packages set `[Build] Output = "../Bin"` in their manifest, so `rux test` writes their binaries to `Tests/Integration/Bin/` (git-ignored, like every `Bin/` directory). The Rux compiler itself builds to `Bin/<Config>/` (e.g. `Bin/Release/rux`); the CMake intermediates stay in the `build/` directory you pass to `-B`. `Bin/` is listed in `.gitignore`; nothing under it is tracked.

### Compiler pipeline

A source file flows through these stages, front to back. Each stage owns a small set of files, so this is the map for "where do I make this change?":

| Stage             | File(s)                         | Role                              |
| ----------------- | ------------------------------- | --------------------------------- |
| Source loading    | `Source/SourceLoader.cpp`       | Locate and read source files      |
| Lexing            | `Lexer/Lexer.cpp`               | Source text → token stream        |
| Parsing           | `Syntax/Parser/`                | Tokens → AST                      |
| Semantic analysis | `Semantic/SemanticAnalyzer.cpp` | AST → validated `SemanticModel`   |
| HIR lowering      | `Lowering/AstToHir/`            | Semantic model → HIR              |
| HIR passes        | `Ir/Hir/Passes/`                | HIR optimization                  |
| LIR lowering      | `Lowering/HirToLir/`            | HIR → control-flow-explicit LIR   |
| Code generation   | `CodeGen/X86_64/`               | LIR → x86-64 RCU object data      |
| Object emission   | `Object/Rcu/`                   | RCU serialization and diagnostics |
| Linking           | `Linker/{Pe,Elf,MachO}/`        | Objects → target executable       |

Supporting layers around the pipeline:

- **CLI & driver** — `Main.cpp`, `Cli/`, `Driver/CompilerDriver.cpp`, build reports, and target selection.
- **Packages & manifests** — `Package/Package.cpp`, `Package/Manifest.cpp` (parse `Rux.toml`, resolve dependencies).
- **Target and system layers** — `Target/` models the machine being compiled for; `System/` isolates services of the host running the compiler. Direct OS
  API calls (`getenv`, `<windows.h>`, `fork`, ...) are allowed only in `System/`; CI enforces this with `Tools/PlatformIsolation/Check.sh`.

## 5. Testing

There are two suites: Rux-language test packages (run with `rux test`) and C++ unit tests for the compiler internals (run with `ctest`).

### Language tests (`Tests/Integration/`)

Run the full suite from the repo root. Installation is idempotent, so it is safe to run after pulling changes or editing a test manifest:

```sh
./Bin/Release/rux install         # discover and install test dependencies
./Bin/Release/rux test --release  # omit --release for a Debug-profile test build
```

Because this repository intentionally has no root `Rux.toml`, bare `rux install` uses manifest-less workspace discovery. It scans package manifests below the root `Tests/`, immediate member directories, and each member's `Tests/`; deduplicates their registry dependencies; and installs the complete transitive graph. Path dependencies are local and are not downloaded. On Windows the shared package cache is `%LocalAppData%\Rux\Packages`; on Unix-like hosts it is `~/.rux/packages`.

`rux test` walks `Tests/` recursively: a directory with a `Rux.toml` is a test package; a directory without one (like `Tests/Integration/`) is a group and is searched further. Each package is built and run, and per-package results plus a summary are reported.

Run from a package directory, it tests that package alone (its own `Tests/`). Run from a workspace root — a directory with no `Rux.toml` of its own — it discovers the root `Tests/` **and** each member package's `Tests/`, so tests may
live centrally or beside the code they cover. A test under a member is labeled with the member's name, so `Text/Tests/Compare` reports as `Text/Compare`.

A test package is a normal binary package whose **exit code is the only signal: `0` = pass, anything else = fail** — no framework required:

```rux
func Main() -> int {
    if 2 * 10 != 20 {
        return 1;
    }
    return 0;
}
```

Each package lives at `Tests/Integration/<Name>/` with a `Rux.toml` manifest and `Src/Main.rux`:

```toml
[Package]
Name = "Arithmetic"
Version = "0.1.0"
Description = "Test for Rux compiler"

[Build]
Output = "../Bin"
```

**When you add a language feature, add a matching test package under `Tests/Integration/`.**

### Unit tests (`Tests/Unit/`)

C++ tests against `RuxCore` using the vendored [doctest](https://github.com/doctest/doctest) header. Build and run them with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUX_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

(The test binary is `build/Tests/Unit/rux-tests`; run it directly for doctest's filtering options, e.g. `rux-tests -ts="Lexer*"`. On Windows it has an `.exe` suffix.)

### Golden diagnostics (`Tests/Golden/`)

Part of the unit-test binary: every `Tests/Golden/<Case>.rux` is compiled through the frontend (lex → parse → sema), the diagnostics are rendered one per line as `line:column: severity: message`, and compared against `<Case>.expected`. To add a case, drop a `.rux` file in `Tests/Golden/`, run the test binary once with `RUX_UPDATE_GOLDEN=1` to generate the `.expected` file, and review it before committing. **When you change or add a diagnostic, add or regenerate the affected golden cases.**

## 6. Code style

Formatting is enforced by [`.clang-format`](../.clang-format) (LLVM base, 4-space indent, west const, 120-column limit).

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

PowerShell equivalent:

```powershell
git ls-files '*.cpp' '*.h' | ForEach-Object { clang-format -i $_ }
```

`clang-format` handles layout; the conventions it can't enforce, observed
throughout the codebase:

- **Naming**
  - Files, types (`struct`/`class`/`enum`), and free/member functions are `PascalCase` — `Parser`, `ParseResult`, `EmitError()`.
  - Member variables, parameters, and locals are `camelCase` — `tokens`, `sourceName`, `structInitAllowed`.
  - Enumerators and namespaced constants are `PascalCase` — `Severity::Error`, `RcuSecType::Text`.
  - Everything lives in the `Rux` namespace (no namespace indentation; closing `} // namespace Rux` comment is auto-emitted).
- **Headers** — every header opens with `#pragma once`. Includes are grouped and sorted by `clang-format`: project headers first, then `<system>` headers. Mark non-void accessors `[[nodiscard]]` and use `noexcept` where it holds, as the existing headers do.
- **Error handling** — the compiler does **not** throw for ordinary failures. Fallible operations return a result struct carrying a `diagnostics` vector (with a `HasErrors()` query) or a `std::optional` / `bool`. Diagnostics are _accumulated_ with severity + `SourceLocation` (see `EmitError`/`EmitWarning`) so the compiler can report many problems in one run and recover at statement boundaries rather than aborting on the first error.

Otherwise, match the surrounding code — consistency beats personal preference.

## 7. Commits

- One logical change per commit; keep history readable.
- Write messages as short imperative sentences: `Fix parser crash on empty block`, not `Fixed the parser`.
- Commits are **not squashed** on merge — PRs land on `dev` via a merge commit (see [Pull Request Lifecycle](PullRequest.md)), so the individual commits you push are what end up in history. Keep them atomic and self-contained.
- Link the issue a change closes with `Fixes #42` in the PR description (or a commit message). No other trailers are required.
