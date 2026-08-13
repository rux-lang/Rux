# Compiler Architecture

Rux is split into focused CMake component targets whose dependencies follow the compilation pipeline. Most implementation folders map directly to a target; header-only target models are shared by the components that consume them. This keeps frontend, backend, host-system, and package-management changes reviewable without turning `RuxCore` into a monolith.

## Compilation Pipeline

```text
Source loading -> SourceModel -> Lexer -> Syntax -> SemanticModel
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

The two back ends are symmetric: each encodes instruction bytes itself, each builds an RCU object, and both hand that object to the same linker. No stage invokes an external assembler, compiler, linker, archiver, or signing tool.

The driver loads the root manifest and dependencies before entering this pipeline. Diagnostics can stop the process after any frontend stage; object emission and linking only run when analysis and lowering succeed.

Rux supports four operating systems — FreeBSD, Linux, macOS and Windows — on x86-64 and AArch64. Which triples a back end reaches is decided by the object and image writer, not by the host, and both back ends reach all eight: `freebsd-*` and `linux-*` through ELF, `windows-*` through PE/COFF, and `macos-*` through Mach-O. The FreeBSD and Linux paths produce executables, shared libraries, relocatable objects, and deterministic static archives entirely in-process. The Windows writer produces executables, DLLs with import libraries, and static libraries, while the Mach-O path produces AArch64 objects and BSD archives plus signed executables — fixed-address on x86-64, position-independent on AArch64 — imported executables, and shared libraries. The public driver exposes all three native artifact kinds for each of those targets.

## Component Ownership

| Component              | Owns                                                                         | May depend on                         |
| ---------------------- | ---------------------------------------------------------------------------- | ------------------------------------- |
| `SourceModel`          | Source locations and loaded-file identity values                             | Standard library only                 |
| `BuildInfo`            | Immutable compiler identity, timestamp, and typed Debug/Release profile       | Standard library only                 |
| `Diagnostics`          | Diagnostic values and rendering primitives                                   | SourceModel                           |
| `Source`               | Source discovery and loading                                                 | SourceModel and Diagnostics           |
| `System`               | Host OS, process, filesystem, networking, environment, and JSON              | Target, standard library, host APIs   |
| `Target`               | Validated target triples, ABI, layout, and instruction models                | SourceModel                           |
| `Package`              | `Rux.toml`, dependency metadata, and workspace discovery                     | Crypto and Target                     |
| `Lexer`                | Tokens and lexical analysis                                                  | SourceModel and Diagnostics           |
| `Syntax`               | AST and parser                                                               | Lexer, Diagnostics, and Target        |
| `Semantic`             | Symbols, types, conditional compilation, and validated semantic model        | BuildInfo, Syntax, and Diagnostics    |
| `Ir/Hir`               | High-level IR and its transformations                                        | Semantic, Lexer, SourceModel, Target  |
| `Ir/Lir`               | Control-flow-explicit low-level IR                                           | Semantic                              |
| `Lowering`             | AST/semantic model → HIR → LIR                                               | Frontend and IR components            |
| `CodeGen`              | Layout rules, literal decoding, register allocation, and the assembly result | LIR                                   |
| `CodeGen/X86_64`       | x86-64 instruction encoding, inline assembly, and RCU construction           | BuildInfo, LIR, Object, Diagnostics   |
| `CodeGen/AArch64`      | AArch64 instruction encoding, inline assembly, and RCU construction          | BuildInfo, LIR, Object, Diagnostics   |
| `Object/Rcu`           | RCU object representation, relocation kinds, and serialization               | BuildInfo and Target                  |
| `Archive`              | Deterministic native archive containers and symbol indexes                   | Object                                |
| `Linker`               | PE, ELF, Mach-O, relocatable-object, and library output                      | Object, Archive, and System           |
| `Driver`               | End-to-end compilation orchestration and build reports                       | All compiler stages                   |
| `Formatter` / `Linter` | Source formatting and lint diagnostics                                       | Syntax; the linter also uses Semantic |

The CMake target graph enforces these dependencies. `RuxBuildInfo` and `RuxSourceModel` are interface-only boundaries;
`RuxTarget` also owns the compiled parsing and catalog implementation for `TargetTriple`. `BuildInfo` is populated once by
driver composition and then passed unchanged through compile-time evaluation,
lowering, and RCU emission; those stages do not include generated compiler-version data or read the clock themselves.
`BuildProfile` is the single source for profile names, build mode, optimization level, debug assertions, and debug
information. Debug selects O0/None and HIR-to-LIR lowering performs no HIR transformation. Release selects Speed and is
the only profile allowed to invoke the legacy HIR pass manager while the replacement optimization pipeline is built.
`SourceModel` and `Target` expose no source discovery or loading. Source loading reports failures as diagnostic values and
never prints them itself. Prefer adding a dependency to the narrowest owning component rather than reaching through
`RuxCore`.

The Mach-O image writer keeps file layout separate from target instruction policy. A private architecture profile owns the CPU type and subtype, VM and file alignment, executable entry strategy, instruction-stub size and alignment, thread-state shape, build-version policy, and the relocation patch kinds that architecture accepts. Segment, section, dyld, symbol, and link-edit layout consumes that profile and otherwise remains architecture-neutral. The AArch64 path uses 16 KiB segments and records the macOS 26 deployment and SDK baseline in `LC_BUILD_VERSION`. XNU rejects a static arm64 executable and rejects a dyld-linked one without `MH_PIE`, so every AArch64 executable is dynamic and position-independent: it enters through `LC_MAIN`, preserves dyld's frame across the call to Rux `Main`, names `libSystem` even with no imports, and routes imported calls through 12-byte X16 ADRP/LDR/BR stubs backed by eagerly bound non-lazy pointers. A slid image cannot carry absolute pointers in read-only memory, so constant data moves from `__TEXT,__const` into a writable `__DATA_CONST` segment whose absolute pointers are rebased; the segment carries `SG_READ_ONLY` — current dyld refuses it otherwise — so dyld re-protects it read-only once those fixups are applied, and an absolute relocation in code is reported rather than emitted. The x86-64 freestanding path keeps its fixed-address layout, `LC_UNIXTHREAD` entry, and in-image exit syscall stub. AArch64 dylibs omit all executable entry state, publish their non-local code and data through the export trie and symbol table, resolve cross-object definitions directly, and reuse the architecture-owned stubs for external calls. Dyld rebase opcodes cover absolute pointers to image-local definitions, while eager bind opcodes populate imported function slots in the dylib's `__DATA` segment. Defined pointers, relative fields, branches, ADRP/ADD pairs, and scaled low-12 loads are patched through the same AArch64 relocation helper used by the ELF and PE image writers. Imported data remains rejected until code generation provides GOT-aware lowering. Unit tests inspect the resulting header, load commands, instructions, relocations, rebase/bind metadata, exports, and signature hashes with the repository's compact Mach-O reader, so structural coverage is portable and never depends on Apple tools such as `otool`.

The ELF64 image writer similarly consumes a target-owned profile selected by the requested OS and architecture. The profile owns ELF OSABI and machine identity, interpreter and default libc names, image and maximum load alignment, freestanding exit syscall, dynamic relocation numbers, BSD process-global policy, and the architecture's PLT instruction shape. The writer owns generic ELF header, segment, dynamic-table, symbol, relocation, and byte layout; it rejects any OS/architecture pair without an explicit profile instead of inheriting Linux defaults. FreeBSD AArch64 supports freestanding and imported executables, shared libraries, relocatable objects, and static archives through both the direct linker and public compiler driver. Its shared objects use zero-based `ET_DYN` values, export public code and data through the dynamic symbol table, bind image-local definitions directly, and leave load-bias and imported-function address fixups to FreeBSD `rtld`. Portable unit coverage drives all three manifest artifact kinds and reads ELF headers, mapped segments, interpreter, dynamic metadata, symbols, RELA records, and archive-member identities through repository-owned readers rather than invoking host tools.

Foreign `freebsd-aarch64` builds use the canonical target-separated output directory, while `run` remains host-only and target tests require a FreeBSD host whose compiler process or native OS is AArch64. CI covers both ownership boundaries: a native compiler executes the full suite and runtime fixtures, and a separate x86-64 compiler produces a sealed payload that a fresh AArch64 FreeBSD VM validates and launches. Release publication depends on both paths. The relocatable Mach-O object path maps AArch64 pointers, branches, ADRP page references, and ADD/load/store page offsets to Apple's ARM64 relocation records. Instruction relocations carry non-zero addends in a preceding signed 24-bit `ARM64_RELOC_ADDEND` record; local definitions use a section ordinal and section-relative offset, while external definitions use the reordered symbol-table index. The writer decodes each instruction before emitting a relocation so an invalid opcode, shifted ADD, or misaligned scaled access is diagnosed rather than archived. `RuxArchive` then places those objects in the same deterministic BSD archive and sorted symbol index used by macOS x86-64.

The AArch64 RCU emitter similarly selects procedure-call layout from the target OS rather than the compiler host. Generic AAPCS64 and Windows retain doubleword stack argument slots and the standard even-register rule for 16-byte-aligned arguments. Apple fixed arguments use naturally aligned, exact-width stack slots, may start a 16-byte-aligned value in an odd-numbered X register, and extend sub-32-bit integers in the caller. For C variadic calls, LIR records the fixed-parameter count: Apple classifies that prefix normally, promotes narrow integer and `float32` arguments in the anonymous tail, and places the entire tail in eight-byte stack slots. Windows keeps its general-register duplication behavior, while generic AAPCS64 keeps its register-save-area convention. Every variant finishes with a 16-byte-aligned stack pointer, reserves X18, and maintains the X29/X30 frame chain.

## Host and Target Boundaries

`Target` describes the output machine. `System` describes the host running the compiler. Code generation and linking must use target data rather than host preprocessor checks. For example, emitting a Linux executable while running on Windows is a target decision; locating `%LocalAppData%` is a host decision.

Operating-system APIs are confined to `Compiler/System`; the CI isolation guard is `Tests/Policy/PlatformIsolation/Check.sh`. New uses of `getenv`, `<windows.h>`, `fork`, or similar APIs belong behind a `System` interface.

Process launches are confined the same way, and for a stronger reason: Rux encodes its own machine code, links its own artifacts, and signs Mach-O images, so no stage may call out to an assembler, a C compiler, a linker, an archiver, or a signing tool. `Tests/Policy/NoExternalToolchain/Check.sh` is that guard. No file under `Compiler/` names such a program, and the only stages that launch anything at all are `Cli/CmdRun.cpp` and `Cli/CmdTest.cpp`. `run` always builds and launches the host target; `test --target` launches output only when the target OS matches and the target architecture is executable directly by the compiler process or native OS.

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

The build exposes focused targets such as `RuxBuildInfo`, `RuxSourceModel`, `RuxTarget`, `RuxCrypto`, `RuxSyntax`, `RuxSemantic`, `RuxHir`, `RuxLir`, `RuxLowering`, `RuxCodeGenCommon`, `RuxCodeGenX86_64`, `RuxCodeGenAArch64`, `RuxObjectRcu`, `RuxArchive`, `RuxLinker`, and `RuxDriver`. `RuxBuildInfo` carries immutable per-compilation identity and time values without orchestration APIs. `RuxSourceModel` owns immutable source identity values shared by diagnostics and target assembly models without exposing loading APIs. `RuxCrypto` owns narrow byte-oriented cryptographic primitives shared across otherwise unrelated stages; package checksum formatting remains in `RuxPackage`, while Mach-O signing remains in `RuxLinker`. `RuxCore` is an interface-only compatibility aggregation target.

`RuxCore` is convenient for the unit-test executable and embedders, but compiler components must link to their actual dependencies. It must not become a shortcut that introduces cycles between stages.

## CLI and Package Flows

Formatter and linter are internal compiler components exposed through the single `rux` application:

```text
rux fmt  -> RuxFormatter -> RuxSyntax
rux lint -> RuxLinter    -> RuxSyntax + RuxSemantic
rux      -> RuxDriver
```

Package commands use `Package/Manifest` for the strict versioned [`Rux.toml` contract](Manifest.md), `System/Process` and `System/Json` for registry transport and response parsing, and `Driver/Registry` for the registry's read contract: the resolver index, an exact version's checksum, and the artifact bytes. `Package/Checksum` verifies a download against the digest the registry published, and `Package/Artifact` both builds and unpacks the `.ruxpkg` archive under one contract. Manifest failures carry source-located diagnostics rather than escaping as exceptions.

The Mach-O image writer builds ad-hoc signatures entirely inside `RuxLinker`. It reserves `LC_CODE_SIGNATURE` during layout, hashes the final signed prefix in 4 KiB pages with `RuxCrypto` SHA-256, emits a version 0x20400 CodeDirectory and embedded-signature superblob in big-endian form, and includes the result in `__LINKEDIT`. The same target bytes are therefore produced on every compiler host without invoking an Apple signing tool.

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
