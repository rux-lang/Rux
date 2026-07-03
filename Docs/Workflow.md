# Development Workflow

The day-to-day loop for changing the Rux compiler. For where your branch should
come from and where it goes, see [Branch Architecture](Branches.md).

## 1. Prerequisites

Install the toolchain (Clang 22.1+, CMake 4.3+, Ninja 1.13+, Git 2.54+) per your
OS — see [Building from Source](../README.md#building-from-source).

Beyond that build toolchain you need **`clang-format`** (ships with LLVM/Clang)
to format your changes — CI and reviewers expect formatted diffs. No other tool
is required:

- **`clang-tidy`** is *not* used; there is no `.clang-tidy` in the repo.
- **Editor / IDE** setup is optional. CMake writes a `compile_commands.json`
  into the build directory, so `clangd` (and any editor that speaks LSP) gets
  full navigation and diagnostics with no extra configuration.

## 2. Get the source and configure

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++   # adjust compiler name per OS
```

With no `-DCMAKE_BUILD_TYPE`, the project defaults to a **Release** build (set in
`CMakeLists.txt`). See [Debug vs. Release](#debug-vs-release-builds) below for
when to pick the other.

## 3. The inner loop

1. **Branch** off `dev` (see [Branch Architecture](Branches.md)).
2. **Edit** sources under `Compiler/<Component>/` (e.g. `Compiler/Frontend/`,
   `Compiler/Backend/`); headers live next to their `.cpp` files.
3. **Build** the incremental build:
   ```sh
   cmake --build build --config Release
   ```
4. **Run / debug** the compiler you just built:
   ```sh
   ./Bin/Release/rux help        # Windows: .\Bin\Release\rux.exe help
   ```
5. **Test** (see below).
6. **Format** touched files: `clang-format -i <files>`.

### Debug vs. Release builds

- **Release** (default) — optimized; this is what CI builds and tests, and what
  ships. Use it for normal development and before pushing.
- **Debug** — configure a *separate* build directory with
  `-DCMAKE_BUILD_TYPE=Debug` (e.g. `cmake -S . -B build-debug -G Ninja
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug`). Unoptimized with debug
  info, so breakpoints and stepping in a debugger (lldb/gdb) behave predictably.

There are **no sanitizer presets** wired into `CMakeLists.txt`. If you want ASan/
UBSan, pass the flags yourself on a throwaway build dir, e.g.
`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`.

## 4. Repository layout

| Path                    | Purpose                                                       |
|-------------------------|---------------------------------------------------------------|
| `Compiler/Cli/`         | Command-line interface (argument parsing, command handlers)   |
| `Compiler/Driver/`      | Build orchestration support (targets, build reports)          |
| `Compiler/Frontend/`    | Lexer, parser (`Parser/`), AST (`Ast/`), semantic analysis (`Sema/`) |
| `Compiler/Ir/`          | Intermediate representations (`Hir/`, `Lir/`) and optimizer   |
| `Compiler/Backend/`     | Code generation (`X64/`), object files (`Rcu/`), linking (`Link/`) |
| `Compiler/Package/`     | Manifest (`Rux.toml`) parsing and package scaffolding         |
| `Compiler/Platform/`    | OS/architecture detection and OS-specific code (the only place for OS `#ifdef`s) |
| `Compiler/Support/`     | Shared utilities (e.g. the generated `Version.h`)             |
| `Compiler/CMakeLists.txt` | The `RuxCore` library + `rux` executable targets            |
| `Tests/Lang/`           | Rux language test packages, one per subdirectory (`rux test`) |
| `Tests/Unit/`           | C++ unit tests for compiler internals (doctest + CTest)       |
| `Tests/Golden/`         | Golden diagnostics cases: `<Case>.rux` + `<Case>.expected`    |
| `Tools/`                | Repo maintenance scripts (e.g. the platform-isolation guard)  |
| `Installers/`           | Platform installer projects (e.g. `Installers/Windows`: MSI + PowerShell) |
| `Bin/`                  | Output for the compiler (`Bin/<Config>/`) and compiled Rux packages; **git-ignored** |
| `CMakeLists.txt`        | Top-level build entry: project `VERSION` + `add_subdirectory(Compiler)` |

`Bin/` is where `rux` drops the binaries it produces. Test packages set
`[Build] Output = "../Bin"` in their manifest, so `rux test` writes their
binaries to `Tests/Lang/Bin/` (git-ignored, like every `Bin/` directory).
The Rux compiler itself builds to `Bin/<Config>/` (e.g.
`Bin/Release/rux`); the CMake intermediates stay in the `build/` directory you
pass to `-B`. `Bin/` is listed in `.gitignore`; nothing under it is tracked.

### Compiler pipeline

A source file flows through these stages, front to back. Each stage owns a small
set of files, so this is the map for "where do I make this change?":

| Stage             | File(s)                  | Role                                                     |
|-------------------|--------------------------|----------------------------------------------------------|
| Source loading    | `SourceLoader.cpp`       | Locate and read source files                             |
| Lexing            | `Token.cpp`, `Lexer.cpp` | Source text → token stream                               |
| Parsing           | `Parser.cpp` (`Ast.h`)   | Tokens → AST (Pratt / precedence-climbing expressions)   |
| Semantic analysis | `Sema.cpp` (`Type.h`)    | Name resolution, type checking, diagnostics              |
| HIR lowering      | `Hir.cpp`                | AST → high-level IR                                      |
| LIR lowering      | `Lir.cpp`                | HIR → low-level IR                                       |
| Optimization      | `Optimizer.cpp`          | Optimize the LIR                                         |
| Codegen           | `Asm.cpp`                | LIR → x86-64 assembly (Intel syntax, System V AMD64 ABI) |
| Object emission   | `Rcu.cpp`                | Emit the native object file in Rux's RCU format          |
| Linking           | `Linker.cpp`             | Link objects into the final executable                   |

Supporting layers around the pipeline:

- **CLI & driver** — `Main.cpp`, `Cli.cpp`, the `*Cmd.cpp` files
  (`BuildCmd`, `CheckCmd`, `InstallCmd`, `PackageCmd`, `HelpCmd`, `UtilityCmd`),
  plus `BuildReport.cpp`, `BuildTarget.cpp`, `Terminal.cpp`, `Process.cpp`.
- **Packages & manifests** — `Package.cpp`, `Manifest.cpp` (parse `Rux.toml`,
  resolve dependencies).
- **Platform layer** — `Platform/` isolates OS differences: `Platform.h`
  (compile-time detection), `Host.cpp` (runtime hardware queries), `Os.h`
  (environment, console, temp/system directories, memory stats, output file
  naming), `Process.h` (subprocess running), `Terminal.h`, `WinApi.h`. Direct
  OS API calls (`getenv`, `<windows.h>`, `fork`, ...) are allowed **only**
  here — CI enforces this with `Tools/CheckPlatformIsolation.sh`.

## 5. Testing

There are two suites: Rux-language test packages (run with `rux test`) and C++
unit tests for the compiler internals (run with `ctest`).

### Language tests (`Tests/Lang/`)

Run the full suite from the repo root:

```sh
./Bin/Release/rux install Std     # test packages depend on Std
./Bin/Release/rux test            # add --release to test the optimized build, --verbose for paths
```

`rux test` walks `Tests/` recursively: a directory with a `Rux.toml` is a test
package; a directory without one (like `Tests/Lang/`) is a group and is
searched further. Each package is built and run, and per-package results plus a
summary are reported. A test package is a normal binary package whose **exit
code is the only signal: `0` = pass, anything else = fail** — no framework
required:

```rux
func Main() -> int {
    if 2 * 10 != 20 {
        return 1;
    }
    return 0;
}
```

Each package lives at `Tests/Lang/<Name>/` with a `Rux.toml` manifest and
`Src/Main.rux`:

```toml
[Package]
Name = "Arithmetic"
Version = "0.1.0"
Description = "Test for Rux compiler"

[Build]
Output = "../Bin"
```

**When you add a language feature, add a matching test package under
`Tests/Lang/`.**

### Unit tests (`Tests/Unit/`)

C++ tests against `RuxCore` using the vendored
[doctest](https://github.com/doctest/doctest) header. Build and run them with:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

(The test binary is `build/Tests/Unit/rux-tests`; run it directly for doctest's
filtering options, e.g. `rux-tests -ts="Lexer*"`.)

### Golden diagnostics (`Tests/Golden/`)

Part of the unit-test binary: every `Tests/Golden/<Case>.rux` is compiled
through the frontend (lex → parse → sema), the diagnostics are rendered one per
line as `line:column: severity: message`, and compared against
`<Case>.expected`. To add a case, drop a `.rux` file in `Tests/Golden/`,
run the test binary once with `RUX_UPDATE_GOLDEN=1` to generate the
`.expected` file, and review it before committing. **When you change or add a
diagnostic, add or regenerate the affected golden cases.**

## 6. Code style

Formatting is enforced by [`.clang-format`](../.clang-format) (LLVM base,
4-space indent, west const, 120-column limit).

```sh
clang-format -i $(git ls-files '*.cpp' '*.h')
```

`clang-format` handles layout; the conventions it can't enforce, observed
throughout the codebase:

- **Naming**
    - Files, types (`struct`/`class`/`enum`), and free/member functions are
      `PascalCase` — `Parser`, `ParseResult`, `EmitError()`.
    - Member variables, parameters, and locals are `camelCase` — `tokens`,
      `sourceName`, `structInitAllowed`.
    - Enumerators and namespaced constants are `PascalCase` — `Severity::Error`,
      `RcuSecType::Text`.
    - Everything lives in the `Rux` namespace (no namespace indentation; closing
      `} // namespace Rux` comment is auto-emitted).
- **Headers** — every header opens with `#pragma once`. Includes are grouped and
  sorted by `clang-format`: project `"Rux/…"` headers first, then `<system>`
  headers. Mark non-void accessors `[[nodiscard]]` and use `noexcept` where it
  holds, as the existing headers do.
- **Error handling** — the compiler does **not** throw for ordinary failures.
  Fallible operations return a result struct carrying a `diagnostics` vector
  (with a `HasErrors()` query) or a `std::optional` / `bool`. Diagnostics are
  *accumulated* with severity + `SourceLocation` (see `EmitError`/`EmitWarning`)
  so the compiler can report many problems in one run and recover at statement
  boundaries rather than aborting on the first error.

Otherwise, match the surrounding code — consistency beats personal preference.

## 7. Commits

- One logical change per commit; keep history readable.
- Write messages as short imperative sentences:
  `Fix parser crash on empty block`, not `Fixed the parser`.
- Commits are **not squashed** on merge — PRs land on `dev` via a merge commit
  (see [Pull Request Lifecycle](PullRequest.md)), so the individual commits you
  push are what end up in history. Keep them atomic and self-contained.
- Link the issue a change closes with `Fixes #42` in the PR description (or a
  commit message). No other trailers are required.
