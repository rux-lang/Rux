# Tests

All repository tests live below this directory and fall into five explicit categories:

| Path                         | Owner and runner                                                 |
| ---------------------------- | ---------------------------------------------------------------- |
| `Language/<Test>/`           | Black-box language/compiler behavior; `rux test`                 |
| `Packages/<Package>/<Test>/` | Black-box first-party package behavior; `rux test`               |
| `Unit/`                      | Compiler internals and `Unit/Golden/` diagnostic fixtures; CTest |
| `Policy/`                    | Source-tree invariants enforced directly by scripts or CI        |
| `Native/`                    | Target-specific runtime acceptance driven by platform scripts    |

## Rux Test Packages

Every executable Rux test contains `Rux.toml` and `Src/Main.rux`. Exit code `0` passes; any other exit code fails. Run every package from the repository root:

```sh
./Bin/rux test --release
```

To run the complete repository workflow—including policy, formatting, build, CTest, workspace checks, lint, and these packages—use `./Run.ps1 test` on Windows or `sh Run.sh test` on Linux, macOS, and FreeBSD.

The language-cutover policy under `Policy/LanguageCutover/` keeps positive first-party source on the final ownership model: parameters use `name: Type`, named ownership transfers use `<-`, destructors use `~Type`, and infallible exact-type construction uses `Type(...)`. Its fixture script verifies every guarded failure mode without compiling a package.

Test manifests are intentionally uniform:

- `[Manifest] Version = 1` opens every file.
- `Type = "Executable"` is explicit.
- `Namespace` is omitted; a test package is built in place and never published.
- Language outputs go to `Bin/Tests/Language/`.
- Package outputs go to `Bin/Tests/Packages/<Package>/`.
- Every dependency is a `{ Path = "..." }` inline table resolving below the root `Packages/` directory.
- Registry dependencies are forbidden in test manifests.

During workspace tests, transitive dependencies in publishable first-party package manifests are resolved from matching local workspace members. Registry fallback is disabled, so the suite does not require `rux install`, a populated package cache, or network access.

## Native Runtime Fixtures

Native fixtures use `Fixture.toml`, rather than `Rux.toml`, so ordinary workspace test discovery never launches platform-specific programs. The scripts under `Native/` build a named target, inspect the artifact, launch it only on compatible native hardware, and validate exact OS-visible results.

On Apple Silicon, run the macOS AArch64 executable, libSystem ABI, assertion, panic, and dynamic-library fixtures with a native compiler:

```sh
sh Tests/Native/MacOSAArch64/Verify.sh ./Bin/rux
```

An x86-64 compiler can run the same fixture set under Rosetta while its emitted ARM64 artifacts still execute directly on the underlying machine:

```sh
sh Tests/Native/MacOSAArch64/VerifyRosetta.sh /path/to/x86_64/rux
```

Both scripts reject non-Apple-Silicon hosts. Their Mach-O preflight reads the ARM64 header and ad-hoc signature bytes itself; it does not invoke an assembler, linker, signing tool, emulator, or Apple inspection utility.

On Windows AArch64, the `WindowsAArch64Assert` and `WindowsAArch64Panic` PowerShell verifiers build and launch each failure path, require non-success termination, and compare the complete LF-terminated stderr layout.

On native FreeBSD AArch64, run the freestanding, libc ABI, assertion, panic, BSD syscall, and shared-library fixtures with:

```sh
sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux
```

The script rejects other kernels and architectures. Its repository-owned ELF reader checks every generated image before execution, so generation and ELF layout failures are reported separately from loader and runtime failures.

To test bytes produced by a different compiler architecture, create a sealed payload on x86-64 FreeBSD and verify it in a separate AArch64 FreeBSD checkout:

```sh
sh Tests/Native/FreeBSDAArch64/BuildTransfer.sh ./Bin/rux /tmp/FreeBSDAArch64Payload
sh Tests/Native/FreeBSDAArch64/VerifyTransfer.sh /tmp/FreeBSDAArch64Payload
```

The first command builds executables, a shared library, and a static-library smoke artifact for `freebsd-aarch64`; the payload contains only runtime files plus their expected hashes, modes, ELF kinds, and outcomes. The second command restores modes, validates the manifest and ELF bytes, and launches the runtime set without installing a compiler, target sysroot, inspection tool, or emulator.

The native fixture set covers distinct artifact and ABI boundaries:

| Fixture                    | Acceptance boundary                                             |
| -------------------------- | --------------------------------------------------------------- |
| `ExitCode`                 | Freestanding executable entry and syscall exit                  |
| `LibC`                     | Dynamic loader, fixed calls, C variadics, stdout, and libc exit |
| `Assert` / `Panic`         | Exact stderr diagnostics and non-success termination            |
| `Packages/FreeBSD/Syscall` | FreeBSD syscall numbers, errors, clocks, mmap, and munmap       |
| `Shared`                   | Shared-library export plus loader load/call/unload behavior     |
| `Static`                   | AArch64 relocatable members and deterministic archive structure |

Ordinary `rux test --release` still runs before these fixtures in FreeBSD CI; native fixtures supplement the workspace suite rather than replacing it.

Linux and Windows can exercise the non-launching cross-build path with PowerShell:

```powershell
./Tests/Native/MacOSAArch64/VerifyCross.ps1 -Rux ./Bin/rux.exe # omit .exe on Linux
```

The cross verifier builds a signed executable and dylib twice, compares their bytes for determinism, validates their ARM64 Mach-O headers and load commands, and recomputes every SHA-256 code-slot hash in the embedded CodeDirectory. It never launches the foreign images.

## Adding Coverage

- Put syntax, semantics, code generation, and runtime language behavior in `Language/<Feature>/`.
- Put public package API behavior in `Packages/<Package>/<Feature>/`.
- Put focused compiler implementation behavior in the relevant `Unit/*Tests.cpp` file.
- Put diagnostic input/expected-output pairs in `Unit/Golden/`.
- Put repository source-layout invariants in `Policy/<Rule>/`.

The C++ manifest-policy tests in `Unit/ManifestTests.cpp` validate every checked-in `Rux.toml`: the schema header and canonical formatting repository-wide, the `Rux` namespace and registry dependency form of publishable first-party packages, and the package type, namespace-free identity, local dependency paths, source entry point and centralized output path of each test manifest.

Related C++ cases are grouped by responsibility: semantic type and binding facts, ownership consumption/lifecycle/cleanup, LIR optimization/reachability, driver target artifacts, and CLI build/package behavior. Their focused `*TestSupport.h` files share fixtures within each group. Splitting a file does not change the registered cases or their assertions.
