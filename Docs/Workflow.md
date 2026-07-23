# Development Workflow

The day-to-day loop for changing the Rux compiler. For where your branch should come from and where it goes, see [Branch Architecture](Branches.md).

## 1. Prerequisites

Install the toolchain (Clang 22.1+ including `clang-format`, CMake 3.30+, Ninja 1.11+, and a recent Git) for your OS — see [Building from Source](../README.md#building-from-source). Install `clang-tidy` from the same LLVM 22 release when you want to run static analysis locally.

The LLVM tools have distinct roles:

- **`clang-format`** enforces C++ layout.
- **`clang-tidy`** enforces the static-analysis baseline in [`.clang-tidy`](../.clang-tidy).
- **Editor / IDE** setup is optional. The build scripts enable `CMAKE_EXPORT_COMPILE_COMMANDS`, producing `Build/compile_commands.json` for clang-tidy, clangd, and other tools.

## 2. Get the source and configure

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
cmake -S . -B Build -G Ninja \
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
   cmake --build Build --config Release
   ```
4. **Run / debug** the compiler you just built:
   ```sh
   ./Bin/rux help        # Windows: .\Bin\rux.exe help
   ```
5. **Test** against the local workspace packages (see below):
   ```sh
   ./Bin/rux test --release
   ctest --test-dir Build --output-on-failure
   ```
6. **Format** all maintained C++ and Rux sources: `sh Format.sh` (PowerShell: `./Format.ps1`).

The repository provides matching platform entry points. PowerShell users can run `./Build.ps1`, `./Format.ps1`, and `./Test.ps1`; Linux, macOS, and FreeBSD users can run `sh Build.sh`, `sh Format.sh`, and `sh Test.sh`. The build scripts configure and build the compiler and C++ test target while generating the compilation database. The format scripts handle maintained C++ and Rux sources. The test scripts run the policy, formatting, build, CTest, lint, and Rux-test workflow. Use `./Test.ps1 -SkipBuild` or `sh Test.sh --skip-build` to reuse an existing build. Add `-ClangTidy` or `--clang-tidy` for the slower static-analysis pass; the Code Quality workflow always runs it.

### Debug vs. Release builds

- **Release** (default) — optimized; this is what CI builds and tests, and what ships. Use it for normal development and before pushing.
- **Debug** — configure a _separate_ build directory with `-DCMAKE_BUILD_TYPE=Debug` (e.g. `cmake -S . -B Build-Debug -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DRUX_BUILD_TESTS=ON`). It is unoptimized and includes debug information, so breakpoints and stepping in a debugger (LLDB/GDB) behave predictably.

There are **no sanitizer presets** wired into `CMakeLists.txt`. If you want ASan/UBSan, pass the flags yourself on a throwaway build dir, e.g. `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`.

## 4. Repository layout

| Path                        | Purpose                                                                              |
| --------------------------- | ------------------------------------------------------------------------------------ |
| `Compiler/Cli/`             | Command-line interface and terminal presentation                                     |
| `Compiler/Driver/`          | Compilation orchestration, targets, and build reports                                |
| `Compiler/Source/`          | Source discovery, loading, and locations                                             |
| `Compiler/Lexer/`           | Tokens and lexical analysis                                                          |
| `Compiler/Syntax/`          | AST and parser                                                                       |
| `Compiler/Semantic/`        | Types, symbols, analysis, and the persistent semantic model                          |
| `Compiler/Ir/`              | HIR/LIR models, printers, and IR-local passes                                        |
| `Compiler/Lowering/`        | Explicit AST-to-HIR and HIR-to-LIR transformations                                   |
| `Compiler/CodeGen/`         | Target-specific code generation                                                      |
| `Compiler/Object/`          | Object formats and serialization                                                     |
| `Compiler/Linker/`          | PE, ELF, and Mach-O executable writers                                               |
| `Compiler/Target/`          | Compilation target, ABI, data-layout, and architecture model                         |
| `Compiler/System/`          | Host OS, hardware, filesystem, and process services                                  |
| `Compiler/Package/`         | Manifest parsing and package management                                              |
| `Compiler/Formatter/`       | Formatting engine used by `rux fmt`                                                  |
| `Compiler/Linter/`          | Lint engine and rules used by `rux lint`                                             |
| `Tests/Language/`           | Executable Rux language and compiler-behavior tests (`rux test`)                     |
| `Tests/Packages/`           | Executable first-party package tests grouped as `<Package>/<Test>` (`rux test`)      |
| `Tests/Unit/`               | C++ unit tests and their `Golden/` diagnostic fixtures (doctest + CTest)             |
| `Tests/Policy/`             | Repository-policy checks, including platform isolation                              |
| `Packaging/`                | Linux and Windows installer scripts plus the Windows MSI project                     |
| `Bin/`                      | Compiler and centralized test executables; **git-ignored**                           |
| `CMakeLists.txt`            | Top-level build entry: project `VERSION` + `add_subdirectory(Compiler)`              |

`Bin/` contains every repository executable. The compiler builds directly to `Bin/rux` (`Bin\rux.exe` on Windows), the C++ unit runner to `Bin/Tests/Unit/rux-tests`, language tests to `Bin/Tests/Language/`, and package tests to `Bin/Tests/Packages/<Package>/`. Test executables are written directly to those corresponding directories without `Debug` or `Release` subdirectories. CMake intermediates stay in the build directory passed to `-B`. `Bin/` is listed in `.gitignore`; nothing under it is tracked.

### Compiler pipeline

A source file flows through these stages, front to back. Each stage owns a small set of files, so this is the map for "where do I make this change?":

| Stage             | File(s)                             | Role                                      |
| ----------------- | ----------------------------------- | ----------------------------------------- |
| Source loading    | `Source/SourceLoader.cpp`           | Locate and read source files              |
| Lexing            | `Lexer/Lexer.cpp`                   | Source text → token stream                |
| Parsing           | `Syntax/Parser/`                    | Tokens → AST                              |
| Semantic analysis | `Semantic/SemanticAnalyzer.cpp`     | AST → validated `SemanticModel`           |
| HIR lowering      | `Lowering/AstToHir/`                | Semantic model → HIR                      |
| HIR passes        | `Ir/Hir/Passes/`                    | HIR optimization                          |
| LIR lowering      | `Lowering/HirToLir/`                | HIR → control-flow-explicit LIR           |
| Code generation   | `CodeGen/{X86_64,AArch64}/`         | LIR → target-native representation        |
| Object emission   | `Object/Rcu/`                       | x86-64 RCU serialization and diagnostics  |
| Linking           | `Linker/{Pe,Elf,MachO}/` or Clang   | Objects/LIR lowering → target executable  |

Supporting layers around the pipeline:

- **CLI & driver** — `Main.cpp`, `Cli/`, `Driver/CompilerDriver.cpp`, build reports, and target selection.
- **Packages & manifests** — `Package/Package.cpp`, `Package/Manifest.cpp` (parse `Rux.toml`, resolve dependencies).
- **Target and system layers** — `Target/` models the machine being compiled for; `System/` isolates services of the host running the compiler. Direct OS
  API calls (`getenv`, `<windows.h>`, `fork`, ...) are allowed only in `System/`; CI enforces this with `Tests/Policy/PlatformIsolation/Check.sh`.

## 5. Testing

There are two suites: Rux-language test packages (run with `rux test`) and C++ unit tests for the compiler internals (run with `ctest`).

### Language and package tests (`Tests/Language/`, `Tests/Packages/`)

Run the full suite from the repository root:

```sh
./Bin/rux test --release  # omit --release for a Debug-profile test build
```

The root `Rux.toml` declares every first-party package as a workspace member. Every dependency in a test manifest must use a local `Path` into `Packages/`; the C++ manifest-policy test enforces this across the whole tree. While running tests, transitive dependencies declared by publishable package manifests are overridden by matching local workspace members, and registry fallback is disabled. The repository test suite therefore requires no package-cache setup or network access.

`rux test` walks `Tests/` recursively: a directory with a `Rux.toml` is a test package; a directory without one (like `Tests/Language/` or `Tests/Packages/`) is a group and is searched further. Each package is built and run, and per-package results plus a summary are reported.

All repository tests are centralized under the root `Tests/` tree. Language tests are labeled `Language/<Test>`; package tests are labeled `Packages/<Package>/<Test>`. See [`Tests/README.md`](../Tests/README.md) for the concise ownership and authoring rules.

A test package is a normal binary package whose **exit code is the only signal: `0` = pass, anything else = fail** — no framework required:

```rux
func Main() -> int {
    if 2 * 10 != 20 {
        return 1;
    }
    return 0;
}
```

Each language test lives at `Tests/Language/<Name>/` with a `Rux.toml` manifest and `Src/Main.rux`:

```toml
[Package]
Name = "Arithmetic"
Version = "0.1.0"
Type = "bin"
Description = "Language test: Arithmetic"

[Build]
Output = "../../../Bin/Tests/Language"
```

**When you add a language feature, add a matching test package under `Tests/Language/`.**

Package tests live at `Tests/Packages/<Package>/<Test>/`. Their manifests use local path dependencies back to the package they cover and direct output to the corresponding central binary folder. For example, a Math test uses `Math = { Path = "../../../../Packages/Math" }` and `Output = "../../../../Bin/Tests/Packages/Math"`.

### Unit tests (`Tests/Unit/`)

C++ tests against `RuxCore` using the vendored [doctest](https://github.com/doctest/doctest) header. Build and run them with:

```sh
cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUX_BUILD_TESTS=ON
cmake --build Build --config Release
ctest --test-dir Build --output-on-failure -C Release
```

The unit-test executable is written to `Bin/Tests/Unit/rux-tests` (`rux-tests.exe` on Windows); CTest resolves that path automatically.

Run the centralized binary directly for doctest filtering, for example `Bin/Tests/Unit/rux-tests -ts="Lexer*"` (with `.exe` on Windows).

### Golden diagnostics (`Tests/Unit/Golden/`)

Part of the unit-test binary: every `Tests/Unit/Golden/<Case>.rux` is compiled through the frontend (lex → parse → sema), the diagnostics are rendered one per line as `line:column: severity: message`, and compared against `<Case>.expected`. To add a case, drop a `.rux` file in `Tests/Unit/Golden/`, run the test binary once with `RUX_UPDATE_GOLDEN=1` to generate the `.expected` file, and review it before committing. **When you change or add a diagnostic, add or regenerate the affected golden cases.**

## 6. Code style

Formatting is enforced by [`.clang-format`](../.clang-format) for C++ (LLVM base, 4-space indent, west const, 120-column limit) and by `rux fmt` for Rux sources. The repository scripts format all maintained sources:

```sh
sh Format.sh
```

PowerShell equivalent:

```powershell
./Format.ps1
```

Use `sh Format.sh --check` or `./Format.ps1 -Check` to verify formatting without changing files. Vendored C++ under `Tests/Unit/ThirdParty/` and intentionally malformed Rux diagnostic fixtures under `Tests/Unit/Golden/` are excluded.

The formatters handle layout; the conventions they can't enforce, observed
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
