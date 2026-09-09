# First-Party Packages

First-party Rux packages live under `Packages/` in the repository root. Return to the [main README](../README.md) for the complete documentation index.

Core is an optional declaration provider. Import primitive APIs explicitly, for example `import Core::int8;` before using `int8::Min`. A dependency's imports do not expose those APIs to its consumers. Replacement providers can declare the same intrinsic types and context values without using Core's package name or registry identity.

## The v0.1.0 Package Set

The v0.1.0 release ships **25 packages**. This table is the target catalog: it is the authority on which package identities exist, what each one owns, and what it depends on.

| Package       | Exclusive responsibility                                             | Direct dependencies                                      |
| ------------- | -------------------------------------------------------------------- | -------------------------------------------------------- |
| `Algorithms`  | Generic algorithms over caller-owned slices                          | Core                                                     |
| `Allocator`   | Allocation policies, validated layouts and allocation owners         | Core, Memory                                             |
| `C`           | Explicit interoperability with C runtime APIs                        | Core                                                     |
| `Collections` | Owning containers and their traversal/storage invariants             | Core, Memory, Allocator, Algorithms, Hash, Entropy, Text |
| `Core`        | Optional intrinsic declarations and fundamental value protocols      | None                                                     |
| `Crypto`      | Cryptographic digest, MAC, KDF and secret-buffer operations          | Core                                                     |
| `Entropy`     | OS entropy acquisition and failure handling                          | Core, Memory, OS                                         |
| `FileSystem`  | Files, directories, metadata and filesystem transactions             | Core, Allocator, Text, Path, Io, Time, Entropy, OS       |
| `Format`      | Placeholder rendering and primitive text conversion                  | Core, Allocator, Text                                    |
| `FreeBSD`     | FreeBSD ABI declarations and thin binding helpers                    | Core                                                     |
| `Hash`        | Named non-cryptographic hash and checksum engines                    | Core                                                     |
| `Io`          | Stream contracts, buffering and console I/O                          | Core, Memory, Allocator, Text, Format, OS                |
| `Json`        | JSON grammar, values and serialization                               | Core, Allocator, Collections, Text, Format, Io           |
| `Linux`       | Linux ABI declarations and thin binding helpers                      | Core                                                     |
| `macOS`       | Darwin/libSystem ABI declarations and thin binding helpers           | Core                                                     |
| `Math`        | Mathematical operations and numerical classification                 | Core                                                     |
| `Memory`      | Untyped blocks, page mappings and byte-level operations              | Core, OS                                                 |
| `Path`        | Lossless native strings and lexical path operations                  | Core, Memory, Allocator, Text                            |
| `Random`      | Seeded generators, distributions and sampling                        | Core, Math, Entropy                                      |
| `Text`        | Text storage, encoding, formatting protocols and shared text writers | Core, Memory, Allocator                                  |
| `Time`        | Clock, duration, calendar and temporal text semantics                | Core, OS, Text                                           |
| `Toml`        | TOML grammar, values and serialization                               | Core, Allocator, Collections, Text, Format, Math, Time   |
| `Unicode`     | Unicode database properties and Unicode algorithms                   | Core                                                     |
| `Uuid`        | UUID representation, generation and textual forms                    | Core, Entropy, Time, Text                                |
| `Windows`     | Windows ABI declarations and thin binding helpers                    | None                                                     |

`OS` denotes the conditional dependency on `Windows`, `Linux`, `macOS` and `FreeBSD` selected by the manifest's `TargetOS` conditions. Every package keeps the `Rux` namespace, the `SourceLibrary` type and `Version = "0.1.0"`. Manifest schema `Version = 1` and `MinRux = "0.4.0"` are separate fields and are not changed by the package release version.

### Layering

Arrows point toward dependencies. This is a dependency ordering, not a requirement that every package depend on every lower layer.

```text
Layer 7  FileSystem, Json, Toml
             |
Layer 6  Io, Uuid
             |
Layer 5  Format, Time, Path, Collections
             |
Layer 4  Text, Random
             |
Layer 3  Allocator, Entropy
             |
Layer 2  Memory
             |
Layer 1  Linux, macOS, FreeBSD, C,
         Algorithms, Hash, Crypto, Math, Unicode
             |
Layer 0  Core, Windows
```

`Linux`, `macOS` and `FreeBSD` import Core intrinsics, so they sit in Layer 1. `Windows` imports nothing and sits in Layer 0. All four have the same platform-binding responsibility; uniform naming and ABI documentation do not require an unused manifest edge.

## Retired Identities

The workspace matches the catalog above: 25 packages, and none of the five identities below resolves any more. The table is kept so that a reader who meets one of these names in older documentation, a branch or an issue can see what happened to it.

| Former identity | What happened               | Status                                  |
| --------------- | --------------------------- | --------------------------------------- |
| `Rux/Storage`   | Renamed to `Rux/FileSystem` | Migrated; the identity no longer exists |
| `Rux/Sync`      | Withdrawn from the release  | Deleted; the identity no longer exists  |
| `Rux/Thread`    | Withdrawn from the release  | Deleted; the identity no longer exists  |
| `Rux/Benchmark` | Withdrawn from the release  | Deleted; the identity no longer exists  |
| `Rux/Simd`      | Withdrawn from the release  | Deleted; the identity no longer exists  |

The `Storage` rename covered the registry identity and dependency names, the `Packages/` and `Tests/Packages/` trees, imports and qualified references, test package names and output paths, the `/storage/` to `/filesystem/` API URL prefix, and the root workspace, manifests, READMEs and examples. The separate website repository retires or redirects the `/storage/` documentation routes on its own schedule; that route migration is recorded here but not published from this repository.

`Storage` survives only in historical discussion. The last file carrying the name, `Packages/Collections/Src/Storage.rux`, is now `ContainerStorage.rux`, and its helpers are package-private: they are how a container talks to an allocator in elements rather than bytes, which is a container's business and not a caller's. That was a filename and helper-visibility concern rather than a second public package identity, and it is closed.

## Not in v0.1.0

These packages are deliberately absent from the release set. Their sources and executable tests were deleted rather than excluded: keeping an unreleased but compiling subset would be a maintenance commitment this release does not make. Git history retains every implementation.

| Package     | Why it is not shipping                                                                                                                                                     |
| ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Benchmark` | Configuration and result arithmetic with no measurement loop, and a `BlackBox` that is an ordinary local store and load. Measurement methodology is not a `Time` concern.   |
| `Simd`      | Four scalar records performing field-by-field arithmetic. Scalar records do not establish the accelerated SIMD contract the package advertised.                             |
| `Sync`      | AArch64 updates use ordinary loads and stores, guards expose raw fields and permit copying, and `Once` cannot initialize. The whole package goes, including x86-64 atomics. |
| `Thread`    | Non-Windows sleep and yield share one spin hint, and `CurrentId` always returns 1.                                                                                          |

Removing `Sync` is not a claim that the inspected x86-64 atomics are incorrect; it is a decision not to release a partial concurrency package. A future concurrency package must be complete and independently validated. `Time::SleepFor` is the supported waiting API for v0.1.0. No thread creation, thread identifier, yield, lock or channel is promised.

## Naming and Ownership Decisions

These are settled for v0.1.0. Revisit the decision before changing anything that depends on it.

- **`Format` keeps its name** and owns primitive conversion in both directions alongside placeholder rendering, because parsing and rendering share numeric machinery. The allocating entry point is `Format::Render`.
- **`Text` owns the presentation contracts** — `Display`, `Debug`, `TextWriter`, `FormatSpec`, `FormatError` and the specification parser. Core gains no formatting contract, so value packages do not depend on the conversion engine. `Text` must not depend on `Format`.
- **`Path` owns native strings.** `OsString`, `OsStringView` and `OsUnit` stay in `Path` as a native-string contract usable by future environment and process packages without `FileSystem` or `Io`. `Text` concerns validated text and encoding rather than OS-dependent native-unit storage. `Path` stays independent of `FileSystem` and `Io`.
- **`macOS` keeps its name.** It binds the Darwin/libSystem ABI rather than issuing raw syscalls. A libSystem dependency alone does not establish iOS support, so the package retains `#target.os == .macOS`.
- **`Memory` and `Allocator` stay separate.** Memory owns addresses and page and block operations; Allocator owns layouts, policies and lifetimes.
- **`FileSystem` owns `File`** and implements Io's interfaces. `Io` does not acquire filesystem resources.
- **`Entropy` has no presentation dependency.** `EntropyError` implements neither `Display` nor `Debug` in v0.1.0; callers handle its cases or wrap it in their own diagnostic type. `Format` must not acquire an `Entropy` dependency in either direction, and neither must `Text`. The edge is what makes the decision reversible by accident — adding the implementation would look local to whoever added it — so `Tests/Unit/ManifestTests.cpp` checks the transitive dependency closure in both directions rather than the implementation. `Text` presents the low-level `AllocError` and `PageError` it already depends on, since they surface through `TextError`.
- **`Entropy`, `Uuid` and `Hash` stay separate identities.** Unpredictable OS input with its retries and zeroization, UUID layout and version semantics, and named non-cryptographic hash engines are each a coherent domain.

A new concern belongs with the package that owns its invariants. Shared implementation detail moves downward only when it has an independent contract and introduces no upward dependency. Add a package dependency only for actual use.

## Platform Manifest Audit

One rule applies to all four platform packages: declare `Core` when the sources import its APIs or intrinsics, and not otherwise. Manifest edges are not added to align diagram levels.

| Package   | Imports `Core` in `Src/`  | Declares the `Core` dependency | Result  |
| --------- | ------------------------- | ------------------------------ | ------- |
| `Windows` | No                        | No                             | Correct |
| `Linux`   | Yes (`#target`, `#Error`) | Yes                            | Correct |
| `macOS`   | Yes (`#target`)           | Yes                            | Correct |
| `FreeBSD` | Yes (`#target`, `#Error`) | Yes                            | Correct |

All four agree with their sources at the current commit. Do not add a `Windows` to `Core` edge for symmetry. `SourceLibrary` packages conditionally import these four according to the compilation target.

## POSIX Binding Contract

`Linux`, `macOS` and `FreeBSD` bind the same interface to three kernels. Their **values** are each system's own and must never be unified: `AtFdCwd` is `-2` on Darwin and `-100` elsewhere, monotonic is clock 1, 6 and 4 respectively, `EAGAIN` is 11 on Linux and 35 on both BSDs, and `RTLD_LOCAL` is 0, 4 and 0. Their **vocabulary** is shared and must not diverge: an equivalent call carries the same wrapper name, the same parameter names and the same parameter shapes in all three packages, so a consumer writes one call site rather than a `when #target.os` ladder around three spellings of one idea.

The rule applies to what the three actually have in common. `Brk` is absent on Darwin, `Dup2` on Linux and `Pipe2` on Darwin, and those absences are real; an emulating wrapper that papers over one would be a worse answer than the branch it saved.

| Aspect                     | Rule                                                                                                      |
| -------------------------- | --------------------------------------------------------------------------------------------------------- |
| Wrapper names              | Identical for an equivalent call. Linux's `FstatAt` keeps the kernel's `newfstatat` numbering under `SysNewFstatAt`, but the wrapper is spelled as the BSDs spell theirs. |
| Parameter names and shapes | Identical, including the pointer qualifiers. Kernel records pass by address on all three, never by reference. |
| ABI type names             | `ProcessId`, `FileDescriptor`, `FileOffset`, `FileMode`, `UserId`, `GroupId` and `Timespec` are declared by all three; each width is that system's own. |
| Errno names                | The shared POSIX set is declared by all three, in the kernel's own spelling. Each number is that system's own. |
| Constants and flags        | Never shared. A value carried between two of these packages compiles and means something else.               |

`Tests/Unit/PlatformBindingContractTests.cpp` enforces every row of that table over the parsed sources, and also asserts that the values a reader might carry across really do differ. The executable tests under `Tests/Packages/{Linux,macOS,FreeBSD}/Syscall` carry a block that is byte-identical across the three, which is the same claim made from the other direction: if the contract diverges, only one of the three still compiles.

## Publication Readiness

No first-party package is published, and none is marked publishable. A package becomes publishable only when every documentation check, archive check and applicable review gate is complete; the rule was applied, and the answer today is that one criterion blocks all of them.

**What passes for all 25 packages.** Canonical `SourceLibrary` manifests with no path dependency, package tests on local paths only, `rux check` clean on all eight target cells, `rux doc` generating with no missing declaration, no duplicate route and no unsafe link, and `rux publish --dry-run` validating a deterministic archive — two packs of the same tree agree byte for byte, which the CLI tests assert.

**What blocks all of them.** Native behavior must be executed on Windows, Linux, macOS and FreeBSD across both architectures. Only the host cell runs locally: the compiler refuses to run a foreign target's test programs by design, reporting that the host can build and check the target but not execute it. The other seven cells are compiled and never run, so the syscall numbers, flag values, errno constants and `struct stat` offsets of the four platform binding packages rest on published documentation rather than on a passing test. Closing this needs the [CI matrix](CI-CD.md) or VMs; marking anything publishable before then would treat "it compiles for that target" as the same claim as "it works on that target".

The tests those cells need are written and waiting. `Tests/Packages/{Linux,macOS,FreeBSD}/Descriptors` covers descriptor numbering, short reads and writes, end of file, size boundaries and the native error for each way of misusing a descriptor or a name; `Tests/Packages/{Linux,macOS,FreeBSD}/Mapping` covers anonymous mappings, protection changes, advice, the requests each kernel refuses, and both edges of the reserved error-result window that keeps a high mapping address from reading as a failure. All six compile for both architectures and none has ever been executed, because no host runs another system's programs. They assert a specific errno only where POSIX names it and all three systems agree — `EBADF`, `ENOENT`, `EEXIST`, `EINVAL` — and assert the refusal alone everywhere else, so a passing run means the behavior matched rather than that a guessed number happened to be right.

**What blocks `Rux/Crypto` twice over.** It additionally awaits the independent review described in the [cryptographic review checklist](CryptoReview.md), and says so in its own README and digest contract.

### Verifying a Cell Locally

Cross-compilation covers everything except execution, and is worth running whenever the back end changes — it catches what `rux check` cannot, since the frontend accepts programs that code generation then refuses:

```sh
rux check --target linux-aarch64
rux doc --target linux-aarch64 --output Temp/Docs
rux --manifest Tests/Language/Arithmetic/Rux.toml build --release --target linux-aarch64
```

## Package Layout and Tests

Each package has a versioned [`Rux.toml` manifest](Manifest.md) and source directory. `Executable` package tests are centralized under `Tests/Packages/` and use local first-party path dependencies:

```text
Packages/Format/
├── Rux.toml
└── Src/

Tests/Packages/Format/Int/
├── Rux.toml
└── Src/
```

A test passes by returning exit code `0`; any other exit code fails. See the [development workflow](Workflow.md) for the complete repository layout and testing model, and [`Tests/README.md`](../Tests/README.md) for test ownership and authoring rules.

From the repository root, operate on the whole local workspace:

```sh
rux check
rux lint
rux test
```

Run these commands from the repository root. Package tests are centralized below `Tests/Packages/`; running `rux test` from an individual `Packages/<Name>/` directory does not discover them.
