# Development Workflow

The day-to-day loop for changing the Rux compiler. For where your branch should come from and where it goes, see [Branch Architecture](Branches.md).

## 1. Prerequisites

Install the current stable toolchain for your OS — see [Building from Source](../README.md#building-from-source).
The versions below were verified on September 5, 2026:

| Tool                                                                             | Current release | Repository requirement                                                 |
| -------------------------------------------------------------------------------- | --------------- | ---------------------------------------------------------------------- |
| [LLVM / Clang](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.0) | 23.1.0          | Upstream Clang 23.1+; LLVM 23 `clang-format` and `clang-tidy`          |
| [CMake](https://cmake.org/download/)                                             | 4.4.3           | 4.4.3+; CI pins 4.4.3                                                  |
| [Ninja](https://github.com/ninja-build/ninja/releases/tag/v1.13.2)               | 1.13.2          | 1.13.2+; CI pins 1.13.2                                                |
| [Git](https://git-scm.com/install/)                                              | 2.55.0          | Use the current stable release; platform packaging suffixes may differ |

Configuration rejects unsupported C++ compilers, older Clang/CMake versions, and Ninja versions below 1.13.2. Install `clang-format` and `clang-tidy` from the same LLVM 23 release.

The LLVM tools have distinct roles:

- **`clang-format`** enforces C++ layout.
- **`clang-tidy`** enforces the static-analysis baseline in [`.clang-tidy`](../.clang-tidy).
- **Editor / IDE** setup is optional. The `build` command enables `CMAKE_EXPORT_COMPILE_COMMANDS`, producing `Build/compile_commands.json` for clang-tidy, clangd, and other tools.

## 2. Get the Source and Configure

```sh
git clone https://github.com/rux-lang/Rux.git
cd Rux
cmake -S . -B Build -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DRUX_BUILD_TESTS=ON
```

With no `-DCMAKE_BUILD_TYPE`, the project defaults to a **Release** build (set in `CMakeLists.txt`). See [Debug vs. Release](#debug-vs-release-builds) below for when to pick the other.

## 3. The Inner Loop

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
   ctest --test-dir Build --output-on-failure -C Release
   ```
6. **Format** all maintained C++ and Rux sources: `sh Run.sh format` (PowerShell: `./Run.ps1 format`).

The repository provides one entry point per shell family: `./Run.ps1` on PowerShell and `sh Run.sh` on Linux, macOS, and FreeBSD. Each takes a single command — `build`, `test`, `format`, `policy`, `tidy`, `unit`, `clean`, or `help` — and prints its command and option summary when run with no arguments. `build` configures and builds the compiler and C++ test target while generating the compilation database. `format` handles maintained C++ and Rux sources. `test` runs the policy, build, formatting, CTest, check, lint, and Rux-test workflow, composing the same step functions the standalone commands use, so a step behaves identically alone and in the workflow. `policy`, `tidy`, and `unit` expose those individual steps for a fast inner loop, and `clean` removes the build directory and `Bin/`. `Scripts/RepositoryMessages.ps1` and `Scripts/RepositoryMessages.sh` keep each shell family's progress verbs, pass/fail labels, tool discovery, child-command handling, and duration formatting in one place; the two families expose the same user-facing vocabulary, file/package counts, quoted paths, and canonical duration units (`ms`, `s`, or `min` plus `s`), and both drop ANSI styling when output is redirected or `NO_COLOR` is set. A failed child command identifies the command and retains its failure status. An option a command does not accept is rejected rather than ignored. Use `./Run.ps1 test -SkipBuild` or `sh Run.sh test --skip-build` to reuse an existing build; the workflow reports the selected existing build directory. Add `-ClangTidy` or `--clang-tidy` for the slower static-analysis pass; the Code Quality workflow always runs it. `rux run` is host-only and has no `--target` option. A `-Target` / `--target` test run is allowed only for the host OS and an architecture the compiler process or native OS can execute directly. This supports an x86-64 compiler process on AArch64 Windows and under Rosetta on Apple Silicon; a physical x86-64 host should use `rux build --target` and `rux check --target`, then test the output on its native machine.

The same rule applies to FreeBSD AArch64. Every supported host can build or check `freebsd-aarch64`; Release output lands below `Bin/Release/FreeBSD/AArch64/`. Only FreeBSD with an AArch64 compiler process or native OS can use `test --target freebsd-aarch64`. For cross-runtime work, use `Tests/Native/FreeBSDAArch64/BuildTransfer.sh` on x86-64 FreeBSD and `VerifyTransfer.sh` in a separate AArch64 FreeBSD checkout so the compiler and target-runtime boundaries stay explicit.

For package profile and target selection, ordinary artifact paths, and the complete `rux build --all` contract, see [Package Builds and the Target Matrix](Builds.md).

### Ownership-facing changes

Changes to values, references, constructors, special operations, or cleanup cross several compiler boundaries. Keep the decision in the stage that owns it and test each affected boundary:

- Parser and dump tests cover the written syntax; semantic tests cover reference escape, loan conflicts, mutability, copy/move capability, definite initialization, and diagnostics.
- Lowering tests cover interface views, scratch-copy replacement, source invalidation, drop flags, partial construction, and cleanup on every control-flow exit. Backend tests cover concrete and interface-reference ABI shapes rather than re-deciding ownership.
- A user-visible behavior change gets an executable package under `Tests/Language/`; a rejected legacy spelling stays as a focused negative or golden diagnostic fixture.
- Safe call-time parameters and receivers use `&T` or `&var T`. Keep `*T` and `*var T` only for FFI, nullable values, stored addresses, pointer arithmetic, raw storage, or an intentionally unsafe API, and state that boundary in the affected package README.
- Named ownership transfers use `<-`; copyable values use `=`. Resource owners prohibit copying and provide `~Type`; infallible exact-type construction uses `Type(...)`, while fallible or descriptive factories keep their names.

When a diagnostic changes, regenerate and review every affected golden file. When a package API changes, update its tests, README, API comments, and examples in the same change.

### Debug vs. Release Builds

- **Release** (default) — optimized; this is what CI builds and tests, and what ships. Use it for normal development and before pushing.
- **Debug** — configure a _separate_ build directory with `-DCMAKE_BUILD_TYPE=Debug` (e.g. `cmake -S . -B Build-Debug -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DRUX_BUILD_TESTS=ON`). It is unoptimized and includes debug information, so breakpoints and stepping in a debugger (LLDB/GDB) behave predictably.

There are **no sanitizer presets** wired into `CMakeLists.txt`. If you want ASan/UBSan, pass the flags yourself on a throwaway build dir, e.g. `-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`.

## 4. Repository Layout

| Path                    | Purpose                                                                                |
| ----------------------- | -------------------------------------------------------------------------------------- |
| `Compiler/Cli/`         | Command-line interface and terminal presentation                                       |
| `Compiler/Diagnostics/` | Diagnostic values, severities, and rendering primitives                                |
| `Compiler/Driver/`      | Compilation orchestration, targets, and build reports                                  |
| `Compiler/SourceModel/` | Source locations and loaded-file identity values                                       |
| `Compiler/Source/`      | Source discovery and loading                                                           |
| `Compiler/Lexer/`       | Tokens and lexical analysis                                                            |
| `Compiler/Syntax/`      | AST and parser                                                                         |
| `Compiler/Semantic/`    | Types, symbols, analysis, and the persistent semantic model                            |
| `Compiler/Ir/`          | HIR/LIR models, printers, and IR-local passes                                          |
| `Compiler/Lowering/`    | Explicit AST-to-HIR and HIR-to-LIR transformations                                     |
| `Compiler/CodeGen/`     | Target-specific code generation                                                        |
| `Compiler/Object/`      | Object formats and serialization                                                       |
| `Compiler/Linker/`      | PE, ELF, Mach-O, relocatable-object, and archive writers                               |
| `Compiler/Target/`      | Compilation target, ABI, data-layout, and architecture model                           |
| `Compiler/System/`      | Host OS, hardware, filesystem, and process services                                    |
| `Compiler/Package/`     | Manifest parsing and package management                                                |
| `Compiler/Formatter/`   | Formatting engine used by `rux fmt`                                                    |
| `Compiler/Linter/`      | Lint engine and rules used by `rux lint`                                               |
| `Tests/Language/`       | Executable Rux language and compiler-behavior tests (`rux test`)                       |
| `Tests/Packages/`       | Executable first-party package tests grouped as `<Package>/<Test>` (`rux test`)        |
| `Tests/Unit/`           | C++ unit tests and their `Golden/` diagnostic fixtures (doctest + CTest)               |
| `Tests/Policy/`         | Architectural checks: host API isolation, internal toolchain, and CLI output ownership |
| `Tests/Native/`         | Target-specific end-to-end fixtures driven by platform scripts                         |
| `Packaging/`            | Linux and Windows installer scripts plus the Windows MSI project                       |
| `Bin/`                  | Compiler and centralized test executables; **git-ignored**                             |
| `CMakeLists.txt`        | Top-level build entry: project `VERSION` + `add_subdirectory(Compiler)`                |

`Bin/` contains every repository executable. The compiler builds directly to `Bin/rux` (`Bin\rux.exe` on Windows), the C++ unit runner to `Bin/Tests/Unit/rux-tests`, language tests to `Bin/Tests/Language/`, and package tests to `Bin/Tests/Packages/<Package>/`. Test executables are written directly to those corresponding directories without `Debug` or `Release` subdirectories. CMake intermediates stay in the build directory passed to `-B`. `Bin/` is listed in `.gitignore`; nothing under it is tracked.

### Compiler Pipeline

A source file flows through these stages, front to back. Each stage owns a small set of files, so this is the map for "where do I make this change?":

| Stage             | File(s)                                      | Role                                   |
| ----------------- | -------------------------------------------- | -------------------------------------- |
| Source loading    | `Source/SourceLoader.cpp`                    | Locate and read source files           |
| Lexing            | `Lexer/Lexer.cpp`                            | Source text → token stream             |
| Parsing           | `Syntax/Parser/`                             | Tokens → AST                           |
| Semantic analysis | `Semantic/` analyzer implementation files    | AST → validated `SemanticModel`        |
| HIR lowering      | `Lowering/AstToHir/`                         | Semantic model → HIR                   |
| HIR passes        | `Ir/Hir/Passes/`                             | HIR optimization                       |
| LIR lowering      | `Lowering/HirToLir/`                         | HIR → control-flow-explicit LIR        |
| Code generation   | `CodeGen/{X86_64,AArch64}/`                  | LIR → target-native representation     |
| Object emission   | `Object/Rcu/`                                | RCU serialization and diagnostics      |
| Linking           | `Linker/{Pe,Elf,MachO}/` and archive writers | Objects/LIR lowering → target artifact |

Supporting layers around the pipeline:

- **CLI & driver** — `Main.cpp`, `Cli/`, `Driver/CompilerDriver.cpp`, build reports, and target selection.
- **Packages & manifests** — `Package/Package.cpp`, `Package/Manifest.cpp` (parse `Rux.toml`, resolve dependencies).
- **Target and system layers** — `Target/` models the machine being compiled for; `System/` isolates services of the host running the compiler. Direct OS API calls (`getenv`, `<windows.h>`, `fork`, ...) are allowed only in `System/`; CI enforces this with `Tests/Policy/PlatformIsolation/Check.sh`.
- **No external toolchain** — the compiler assembles, links, and signs its own artifacts, so nothing under `Compiler/` may name or launch an assembler, C compiler, linker, archiver, or signing tool. `Tests/Policy/NoExternalToolchain/Check.sh` enforces that, with the remaining exceptions listed at the top of the script.
- **Language conventions** — parameter syntax, ownership transfer, lifecycle declarations and enum/variant behavior are verified by parser, semantic, golden and integration tests. Source conventions remain documented here and in the language guide.
- **Process-output ownership** — `Tests/Policy/OutputOwnership/Check.sh` keeps process output in `Compiler/Cli/`, with documented inspection-output exceptions. Reusable components return structured failures and diagnostics. Message grammar is covered by behavioral tests rather than source scanners.
- **Focused implementation files** — aim for roughly 800–1,000 lines and review files near 1,200 lines for useful ownership splits. This is a review guideline, with no automated line-count check or exception ceiling. Vendored files and dense reference vectors retain their natural layout.

### User-Facing Message Contract

The CLI owns user-facing toolchain messages. Reusable compiler, package, system, and runtime components return structured failures or diagnostics; they do not print. Use `CliSupport::Reporter` for semantic command output, `RenderDiagnostic` for source diagnostics, and `Reporting::FormatDuration` for elapsed time.

Canonical diagnostics use this shape:

```text
Src/Main.rux:12:9: error: cannot assign a string to 'count', which has type 'int'
  note: 'count' was declared as 'int' on line 4
  help: convert the value to 'int' or change the declared type
  docs: https://rux-lang.dev/docs/variables/
```

When adding or changing messages:

- Start with the subject and cause. Put corrective action in `help:`, supporting context in `note:`, and verified documentation in `docs:`.
- Spell `error:`, `warning:`, `note:`, `help:`, and `docs:` in lowercase; start their text in lowercase and omit a final period on a single-line message. Quote user-provided names, paths, options, targets, versions, and commands with single quotes.
- Use present participles for progress (`Compiling`, `Checking`, `Downloading`) and concise past tense for outcomes (`Built`, `Checked`, `Installed`, `Removed`). Format elapsed time as `25 ms`, `1.25 s`, or `1 min 5.2 s`.
- Name profiles and targets in display spelling in report prose — `Compiling App v0.1.0 (Debug, Windows x86-64)`, via `Driver::FormatBuildContext`. Reserve canonical IDs such as `windows-x86_64` for text naming a value the reader could pass back on the command line. See the [target ID rules](Builds.md#profiles-and-targets).
- Render a titled block of aligned rows with `Reporting::RenderSection`, or `Reporter::Summary` when writing straight to a stream. Both size the label column to the block, so no report hard-codes a column width.
- Green indicates success, yellow a warning, red an error or failure, cyan progress or links, and dim text supporting detail. Honor `--color`, `NO_COLOR`, `TERM=dumb`, and terminal detection.
- `--quiet` suppresses progress, success, tips, and summaries, but never errors. `--verbose` adds phase and path detail without repeating the normal summary.
- JSON modes write only their compatible JSON schema to stdout and never include ANSI styling. Diagnostics and warnings remain on stderr. Commands that launch user programs keep toolchain status off the program's stdout.
- Dedicated CLI pages exist below `https://rux-lang.dev/docs/cli/<command>` for `add`, `build`, `clean`, `doc`, `fmt`, `help`, `init`, `install`, `list`, `new`, `remove`, `run`, `test`, `uninstall`, `update`, and `version`. Use `https://rux-lang.dev/docs/cli` for `check`, `info`, `lint`, `login`, `logout`, `pack`, and `publish` until dedicated pages exist. Do not invent a route.
- Preserve token, AST, semantic, HIR, LIR, assembly, RCU, generated-documentation, and relayed user-program formats. Add a narrowly matched entry with a reason to `Tests/Policy/OutputOwnership/Exceptions.txt` when a stable compatibility payload needs direct output.

## 5. Testing

There are two broad suites: Rux-language/package tests (run with `rux test`) and C++ unit tests for the compiler internals (run with `ctest`). Target-specific native fixtures supplement them where a complete OS interaction needs a dedicated driver script.

`sh Run.sh test` and `./Run.ps1 test` run the three architectural boundary checks once; `policy` runs that step alone. CTest runs C++ cases and the repository-command and installer behavior checks in `Tests/Scripts/`. Language rejection behavior stays in parser, semantic, golden and integration coverage. There is no line-count, language-cutover or message-style source scanner.

### Language and Package Tests (`Tests/Language/`, `Tests/Packages/`)

Run the full suite from the repository root:

```sh
./Bin/rux test --release  # omit --release for a Debug-profile test build
```

The root `Rux.toml` is a versioned workspace manifest that declares every first-party package as a member. Every dependency in a test manifest must use a local `Path` into `Packages/`; the C++ manifest-policy test enforces this across the whole tree. While running tests, qualified transitive dependencies declared by publishable package manifests are overridden by matching namespaced workspace members, and registry fallback is disabled. The repository test suite therefore requires no package-cache setup or network access. The complete syntax is defined by the [`Rux.toml` manifest contract](Manifest.md).

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
[Manifest]
Version = 1

[Package]
Name = "Arithmetic"
Version = "0.1.0"
Type = "Executable"
Description = "Language test: Arithmetic"

[Build]
Output = "../../../Bin/Tests/Language"
```

**When you add a language feature, add a matching test package under `Tests/Language/`.**

Package tests live at `Tests/Packages/<Package>/<Test>/`. Their manifests use local path dependencies back to the package they cover and direct output to the corresponding central binary folder. For example, a Math test uses `Math = { Path = "../../../../Packages/Math" }` and `Output = "../../../../Bin/Tests/Packages/Math"`.

### Native End-to-End Fixtures (`Tests/Native/`)

Native fixtures use `Fixture.toml` so `rux test` does not discover them as ordinary packages. Their platform script builds a specific target, launches the result only on compatible native hardware, and verifies OS-visible behavior such as an exact exit code, assertion or panic output, C ABI calls, stack probing, or loading and calling an exported shared-library function. The Windows AArch64 cross job runs the exit-code and DLL fixtures with an x86-64 compiler on an AArch64 OS; the native and release jobs repeat them with the native compiler. On Apple Silicon, `Tests/Native/MacOSAArch64/Verify.sh` runs the macOS ARM64 fixture set with a native compiler, while `VerifyRosetta.sh` runs an x86-64 compiler under Rosetta and launches the produced ARM64 images directly. The scripts reject Intel Macs and inspect the Mach-O architecture and in-process ad-hoc signature before execution. Linux and Windows use `VerifyCross.ps1` for non-executing smoke coverage: it builds signed `macos-aarch64` executable and dylib fixtures twice, verifies deterministic bytes, parses their ARM64 Mach-O structures, and recomputes every CodeDirectory SHA-256 page hash.

FreeBSD also tests a transferred cross-compiler workflow. An x86-64 FreeBSD VM runs `Tests/Native/FreeBSDAArch64/BuildTransfer.sh` with the x86-64 Rux compiler. The script builds the representative AArch64 fixtures into canonical `freebsd-aarch64` output directories and creates a payload containing only the target executables, shared library, and a manifest of names, SHA-256 hashes, modes, ELF kinds, and expected outcomes. A separate AArch64 FreeBSD VM installs no compiler, restores modes from that manifest, verifies every hash and ELF identity, and launches the transferred bytes through `VerifyTransfer.sh`. Linux x86-64 separately runs `VerifyCross.sh` to build and inspect a FreeBSD AArch64 executable and static archive without a FreeBSD sysroot. QEMU belongs to the CI VM provider; Rux never discovers or launches an emulator.

### Unit Tests (`Tests/Unit/`)

C++ tests against `RuxCore` using the vendored [doctest](https://github.com/doctest/doctest) header. Build and run them with:

```sh
cmake -S . -B Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRUX_BUILD_TESTS=ON
cmake --build Build --config Release
ctest --test-dir Build --output-on-failure -C Release
```

`sh Run.sh unit` and `./Run.ps1 unit` run the same CTest invocation against an existing build. The unit-test executable is written to `Bin/Tests/Unit/rux-tests` (`rux-tests.exe` on Windows); CTest resolves that path automatically.

Run the centralized binary directly for doctest filtering, for example `Bin/Tests/Unit/rux-tests -ts="Lexer*"` (with `.exe` on Windows).

### Golden Diagnostics (`Tests/Unit/Golden/`)

Part of the unit-test binary: every `Tests/Unit/Golden/<Case>.rux` is compiled through the frontend (lex → parse → sema), the diagnostics are rendered one per line as `line:column: severity: message`, and compared against `<Case>.expected`. This stable one-line serialization deliberately omits structured `note:`, `help:`, and `docs:` context; those fields are covered by diagnostic renderer tests instead. A case opening with `// rux:target <triple>` is compiled for that target instead of the host, so a diagnostic about a foreign machine reads the same everywhere; a case compiled for AArch64 additionally has every `asm func` a clean frontend leaves behind run through the AArch64 assembler, so inline-assembly reports are golden cases too. To add a case, drop a `.rux` file in `Tests/Unit/Golden/`, run the test binary once with `RUX_UPDATE_GOLDEN=1` to generate the `.expected` file, and review it before committing. **When you change or add a diagnostic, add or regenerate the affected golden cases.**

## 6. Code Style

Formatting is enforced by [`.clang-format`](../.clang-format) for C++ (LLVM base, 4-space indent, west const, 120-column limit) and by `rux fmt` for Rux sources. The repository entry point formats all maintained sources:

```sh
sh Run.sh format
```

PowerShell equivalent:

```powershell
./Run.ps1 format
```

Use `sh Run.sh format --check` or `./Run.ps1 format -Check` to verify formatting without changing files. Vendored C++ under `Tests/Unit/ThirdParty/` and intentionally malformed Rux diagnostic fixtures under `Tests/Unit/Golden/` are excluded.

The formatters handle layout; the conventions they can't enforce, observed throughout the codebase:

- **Naming**
  - Files, types (`struct`/`class`/`enum`), and free/member functions are `PascalCase` — `Parser`, `ParseResult`, `EmitError()`.
  - Member variables, parameters, and locals are `camelCase` — `tokens`, `sourceName`, `structInitAllowed`.
  - Enum members, variant cases, and namespaced constants are `PascalCase` — `Severity::Error`, `Option::Some`, `RcuSecType::Text`. Named variant payload fields are `camelCase`.
  - Everything lives in the `Rux` namespace (no namespace indentation; closing `} // namespace Rux` comment is auto-emitted).
- **Headers** — every header opens with `#pragma once`. Includes are grouped and sorted by `clang-format`: project headers first, then `<system>` headers. Mark non-void accessors `[[nodiscard]]` and use `noexcept` where it holds, as the existing headers do.
- **Rux layout** — a guard `if` (a single `return`, `break`, `continue`, or `Panic` statement, no `else`) is written on one line when it fits in 120 columns: `if a != a { return b; }`. Braces are always required around every `if` body; there is no brace-less form.
- **Error handling** — the compiler does **not** throw for ordinary failures. Fallible operations return a result struct carrying a `diagnostics` vector (with a `HasErrors()` query) or a `std::optional` / `bool`. Diagnostics are _accumulated_ with severity + `SourceLocation` (see `EmitError`/`EmitWarning`) so the compiler can report many problems in one run and recover at statement boundaries rather than aborting on the first error.

Otherwise, match the surrounding code — consistency beats personal preference.

## 7. Commits

- One logical change per commit; keep history readable.
- Write messages as short imperative sentences: `Fix parser crash on empty block`, not `Fixed the parser`.
- Commits are **not squashed** on merge — PRs land on `dev` via a merge commit (see [Pull Request Lifecycle](PullRequest.md)), so the individual commits you push are what end up in history. Keep them atomic and self-contained.
- Link the issue a change closes with `Fixes #42` in the PR description (or a commit message). No other trailers are required.

## Build and Test Throughput

See [Compiler Build Performance](CompilerPerformance.md) for metadata stability, optional compilation launchers, standard-library PCH, PCH-disabled analysis, ThinLTO, and repeatable measurements. `-Jobs N` / `--jobs N` controls CTest, Rux-test, and tidy workers; defaults use at most four available processors. An explicit value also limits C++ build workers, while ordinary builds retain Ninja's default parallelism. Direct `rux test --jobs N` uses one worker by default.

CTest partitions the existing doctest executable by source owner. `Unit.GroupCoverage` verifies every registered case belongs to exactly one group, including included reference-vector cases. Groups with shared fixtures take a resource lock; pure numeric tests can overlap them. Script and installer checks are ordinary CTest registrations. No line-count, language-cutover, or message-style scanner runs in local verification or CI.
