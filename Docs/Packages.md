# First-Party Packages

First-party Rux packages live under `Packages/` in the repository root. Return to the [main README](../README.md) for the complete documentation index.

## Package Status

Every package below exposes an API and is exercised by tests under `Tests/Packages/`. None is published yet; see [Publication Readiness](#publication-readiness) for what each is waiting on.

| Package       | Description                                                           |
| ------------- | --------------------------------------------------------------------- |
| `Algorithms`  | Generic algorithms over slices and mutable slices                     |
| `Allocator`   | Allocation contracts and the allocators that meet them                |
| `C`           | C standard library bindings                                           |
| `Collections` | Generic data structures                                               |
| `Core`        | Core language intrinsics                                              |
| `Crypto`      | Cryptographic hashes, message authentication codes and key derivation |
| `Entropy`     | Unpredictable bytes from the operating system                         |
| `Format`      | String conversion and formatting                                      |
| `Hash`        | Named non-cryptographic hashes and checksums                          |
| `Io`          | Streams, console I/O, readers and writers                             |
| `Json`        | JSON parsing and serialization                                        |
| `Math`        | Mathematical constants and functions                                  |
| `Memory`      | Memory management functions                                           |
| `Path`        | Native operating-system strings and filesystem paths                  |
| `Random`      | Pseudorandom number generators and distributions                      |
| `Storage`     | Files, directories and filesystem operations                          |
| `Text`        | Strings and fundamental text manipulation                             |
| `Time`        | Durations, monotonic clocks and wall clocks                           |
| `Toml`        | TOML parsing, serialization and streaming                             |
| `Unicode`     | Unicode character properties, case, normalization and segmentation    |
| `Uuid`        | UUID representation, parsing, formatting and generation               |

## Target-Specific Packages

The `FreeBSD`, `Linux`, `macOS`, and `Windows` packages provide operating-system bindings. SourceLibrary packages can conditionally import these packages according to the compilation target.

## Publication Readiness

No first-party package is published, and none is marked publishable. A package becomes publishable only when every documentation check, archive check and applicable review gate is complete; the rule was applied, and the answer today is that one criterion blocks all of them.

**What passes for all 25 packages.** Canonical `SourceLibrary` manifests with no path dependency, package tests on local paths only, `rux check` clean on all eight target cells, `rux doc` generating with no missing declaration, no duplicate route and no unsafe link, and `rux publish --dry-run` validating a deterministic archive — two packs of the same tree agree byte for byte, which the CLI tests assert.

**What blocks all of them.** Native behavior must be executed on Windows, Linux, macOS and FreeBSD across both architectures. Only the host cell runs locally: the compiler refuses to run a foreign target's test programs by design, reporting that the host can build and check the target but not execute it. The other seven cells are compiled and never run, so the syscall numbers, flag values, errno constants and `struct stat` offsets of the four platform binding packages rest on published documentation rather than on a passing test. Closing this needs the [CI matrix](CI-CD.md) or VMs; marking anything publishable before then would treat "it compiles for that target" as the same claim as "it works on that target".

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
