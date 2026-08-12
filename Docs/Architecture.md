# Compiler Architecture

Rux is split into focused CMake component targets whose dependencies follow the compilation pipeline. Most implementation folders map directly to a target; header-only target models are shared by the components that consume them. This keeps frontend, backend, host-system, and package-management changes reviewable without turning `RuxCore` into a monolith.

## Compilation Pipeline

```text
Source -> Lexer -> Syntax -> SemanticModel
                                |
                                v
                          AST-to-HIR -> HIR passes
                                |
                                v
                          HIR-to-LIR -> LIR
                                |
                    +-----------+-----------+
                    |                       |
                    v                       v
             x86-64 CodeGen          AArch64 CodeGen
                    |                       |
                    +----------+------------+
                               |
                               v
                          RCU Object
                               |
                               v
                            Linker -> ELF / Mach-O / PE
```

The two back ends are symmetric: each encodes instruction bytes itself, each builds an RCU object, and both hand that object to the same linker. No stage invokes an external assembler, compiler, linker, or archiver.

The driver loads the root manifest and dependencies before entering this pipeline. Diagnostics can stop the process after any frontend stage; object emission and linking only run when analysis and lowering succeed.

Which triples a back end reaches is decided by the object and image writer, not by the host: x86-64 reaches every supported operating system, while AArch64 reaches `linux-aarch64` through ELF and `windows-aarch64` through PE/COFF. The Windows writer produces executables, DLLs with import libraries, and static libraries entirely in-process. AArch64 targets without an image writer, including macOS and FreeBSD, are refused with a diagnostic naming the target rather than falling back to a host toolchain.

## Component Ownership

| Component              | Owns                                                                               | May depend on                             |
| ---------------------- | ---------------------------------------------------------------------------------- | ----------------------------------------- |
| `Diagnostics`          | Diagnostic values and rendering primitives                                         | Standard library only                     |
| `Source`               | Source loading and source locations                                                | Diagnostics                               |
| `System`               | Host OS, process, filesystem, networking, environment, and JSON                    | Standard library and host APIs            |
| `Target`               | Header-only target triples, ABI, layout, and instruction models                    | Standard library only                     |
| `Package`              | `Rux.toml`, dependency metadata, and workspace discovery                           | Standard library                          |
| `Lexer`                | Tokens and lexical analysis                                                        | Diagnostics                               |
| `Syntax`               | AST and parser                                                                     | Lexer, Diagnostics, and Target            |
| `Semantic`             | Symbols, types, conditional compilation, and validated semantic model              | Syntax and Diagnostics                    |
| `Ir/Hir`               | High-level IR and its transformations                                              | Semantic and Lexer                        |
| `Ir/Lir`               | Control-flow-explicit low-level IR                                                 | Semantic                                  |
| `Lowering`             | AST/semantic model → HIR → LIR                                                     | Frontend and IR components                |
| `CodeGen`              | Layout rules, literal decoding, register allocation, and the assembly result       | LIR                                       |
| `CodeGen/X86_64`       | x86-64 instruction encoding, inline assembly, and RCU construction                 | LIR, Object, Diagnostics                  |
| `CodeGen/AArch64`      | AArch64 instruction encoding, inline assembly, and RCU construction                | LIR, Object, Diagnostics                  |
| `Object/Rcu`           | RCU object representation, relocation kinds, and serialization                     | Target                                    |
| `Archive`              | Deterministic native archive containers and symbol indexes                         | Object                                    |
| `Linker`               | PE, ELF, Mach-O, relocatable-object, and library output                            | Object, Archive, and System               |
| `Driver`               | End-to-end compilation orchestration and build reports                             | All compiler stages                       |
| `Formatter` / `Linter` | Source formatting and lint diagnostics                                             | Syntax; the linter also uses Semantic     |

The CMake target graph enforces these dependencies. Prefer adding a dependency to the narrowest owning component rather than reaching through `RuxCore`.

The Mach-O image writer keeps file layout separate from target instruction policy. A private architecture profile owns the CPU type and subtype, VM page alignment, executable entry strategy, instruction-stub size and alignment, and the relocation patch kinds that architecture accepts. Segment, section, dyld, symbol, and link-edit layout consumes that profile and otherwise remains architecture-neutral. Unit tests inspect the resulting header and load commands with the repository's compact Mach-O reader, so structural coverage is portable and never depends on Apple tools such as `otool`.

## Host and Target Boundaries

`Target` describes the output machine. `System` describes the host running the compiler. Code generation and linking must use target data rather than host preprocessor checks. For example, emitting a Linux executable while running on Windows is a target decision; locating `%LocalAppData%` is a host decision.

Operating-system APIs are confined to `Compiler/System`; the CI isolation guard is `Tests/Policy/PlatformIsolation/Check.sh`. New uses of `getenv`, `<windows.h>`, `fork`, or similar APIs belong behind a `System` interface.

Process launches are confined the same way, and for a stronger reason: Rux encodes its own machine code and links its own artifacts, so no stage may call out to an assembler, a C compiler, a linker or an archiver. `Tests/Policy/NoExternalToolchain/Check.sh` is that guard. No file under `Compiler/` names such a program, and the only stages that launch anything at all are `Cli/CmdRun.cpp` and `Cli/CmdTest.cpp`. `run` always builds and launches the host target; `test --target` launches output only when the target OS matches and the target architecture is executable directly by the compiler process or native OS.

The language parses the same for every target, with one exception: an `asm func` body is machine text rather than Rux, so `Parser` takes the target architecture and reads the body with that architecture's register table (`Target/AsmRegisters.h`). The mnemonics are checked later — a body whose instructions belong to the other architecture is an error reported during semantic analysis, after `when` folding has dropped the branches this build never reaches. A function that must reach both machines therefore writes a body per architecture under `when #target.arch`, as the platform packages' system-call wrappers do.

## Architecture Naming

Use one spelling per context so the website, repository, CLI, and compiler APIs stay predictable:

| Context                   | x86-64                 | AArch64                 |
| ------------------------- | ---------------------- | ----------------------- |
| Website, docs, CI labels  | `x86-64`               | `AArch64`               |
| Target and artifact IDs   | `x86_64`               | `aarch64`               |
| C++ enum                  | `Target::Arch::X86_64` | `Target::Arch::AArch64` |
| Rux enum                  | `Architecture::X86_64` | `Architecture::AArch64` |
| Code-generation directory | `CodeGen/X86_64`       | `CodeGen/AArch64`       |

Canonical target names combine the lowercase OS identifier with the machine identifier, such as `linux-x86_64` and `windows-aarch64`. The CLI accepts `x64`, `amd64`, `x86-64`, and `arm64` suffixes for compatibility, but normalizes them before comparison and before exposing `#target.triple`. External APIs keep their required spellings, including Visual Studio `amd64`/`arm64`, GitHub's `windows-11-arm`, FreeBSD's `aarch64`, and WiX's `x64`.

## Namespaces and Public Boundaries

The principal ownership namespaces currently enforced at cross-platform and orchestration boundaries are `Rux::Target`, `Rux::System`, and `Rux::Driver`. New standalone tools use `Rux::Formatting` and `Rux::Linting`. Existing language model types remain in `Rux` while those large APIs are migrated incrementally; new code must not add declarations to `Misc` or recreate a generic `Utils` component.

The build exposes focused targets such as `RuxSyntax`, `RuxSemantic`, `RuxHir`, `RuxLir`, `RuxLowering`, `RuxCodeGenCommon`, `RuxCodeGenX86_64`, `RuxCodeGenAArch64`, `RuxObjectRcu`, `RuxArchive`, `RuxLinker`, and `RuxDriver`. `RuxCore` is an interface-only compatibility aggregation target.

`RuxCore` is convenient for the unit-test executable and embedders, but compiler components must link to their actual dependencies. It must not become a shortcut that introduces cycles between stages.

## CLI and Package Flows

Formatter and linter are internal compiler components exposed through the single `rux` application:

```text
rux fmt  -> RuxFormatter -> RuxSyntax
rux lint -> RuxLinter    -> RuxSyntax + RuxSemantic
rux      -> RuxDriver
```

Package commands use `Package/Manifest` for the strict versioned [`Rux.toml` contract](Manifest.md), `System/Process` and `System/Json` for registry transport and response parsing, and `Driver/Registry` for the registry's read contract: the resolver index, an exact version's checksum, and the artifact bytes. `Package/Checksum` verifies a download against the digest the registry published, and `Package/Artifact` both builds and unpacks the `.ruxpkg` archive under one contract. Manifest failures carry source-located diagnostics rather than escaping as exceptions.

Build and check resolve path dependencies directly and registry dependencies from the shared package cache, which is keyed by identity and exact version so several versions of a package coexist:

- Windows: `%LocalAppData%\Rux\Packages\<namespace>\<name>\<version>`, for example `%LocalAppData%\Rux\Packages\Rux\Io\0.1.0`
- Unix-like hosts: `~/.rux/packages/<namespace>/<name>/<version>`, for example `~/.rux/packages/Rux/Io/0.1.0`

The namespace and name directories carry the spelling the package was published under, and an existing one is found by comparing normalized names. That matters because the two sides disagree by design: an install writes the spelling the registry publishes, while a build looks the package up with the spelling the consuming manifest happens to use.

Selecting among installed versions is a local operation in `Driver/BuildTarget`, so a build never reaches the network; only `install`, `update`, `add`, `info` and the publication commands do.

An explicit `[Workspace]` manifest names its member packages. Workspace checks resolve qualified registry dependencies from matching namespaced members and resolve test-only path dependencies directly. `rux test` discovers `Program` packages below the root `Tests/` tree, requires their direct dependencies to use local path entries, resolves transitive first-party dependencies from workspace members, and disables registry fallback. Publishable package manifests can therefore retain qualified registry dependencies without making repository tests depend on the network or shared package cache.

## Failure and Diagnostic Contracts

Ordinary compiler failures are values, not exceptions. Frontend stages collect diagnostics with source locations so users can fix several problems per run. Filesystem, process, and network helpers return `bool`, `std::optional`, or a result object; the CLI owns user-facing error text and exit codes.

When adding a stage or pass:

1. Put the implementation in the component that owns the data it transforms.
2. Add the narrow CMake dependency required by that implementation.
3. Preserve diagnostics instead of printing from library code.
4. Add focused C++ unit coverage, plus a language integration test when user behavior changes.
5. Update this document if ownership or dependency direction changes.
