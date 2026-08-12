# Tests

All repository tests live below this directory and fall into five explicit categories:

| Path                         | Owner and runner                                                        |
| ---------------------------- | ----------------------------------------------------------------------- |
| `Language/<Test>/`           | Black-box language/compiler behavior; `rux test`                        |
| `Packages/<Package>/<Test>/` | Black-box first-party package behavior; `rux test`                      |
| `Unit/`                      | Compiler internals and `Unit/Golden/` diagnostic fixtures; CTest        |
| `Policy/`                    | Source-tree invariants enforced directly by scripts or CI               |
| `Native/`                    | Target-specific runtime acceptance driven by platform scripts           |

## Rux Test Packages

Every executable Rux test contains `Rux.toml` and `Src/Main.rux`. Exit code `0` passes; any other exit code fails. Run every package from the repository root:

```sh
./Bin/rux test --release
```

To run the complete repository workflow—including policy, formatting, build, CTest, workspace checks, lint, and these packages—use `./Test.ps1` on Windows or `sh Test.sh` on Linux, macOS, and FreeBSD.

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

Native fixtures use `Fixture.toml`, rather than `Rux.toml`, so ordinary workspace
test discovery never launches platform-specific programs. The scripts under
`Native/` build a named target, inspect the artifact, launch it only on compatible
native hardware, and validate exact OS-visible results.

On Apple Silicon, run the macOS AArch64 executable, libSystem ABI, assertion,
panic, and dynamic-library fixtures with a native compiler:

```sh
sh Tests/Native/MacOSAArch64/Verify.sh ./Bin/rux
```

An x86-64 compiler can run the same fixture set under Rosetta while its emitted
ARM64 artifacts still execute directly on the underlying machine:

```sh
sh Tests/Native/MacOSAArch64/VerifyRosetta.sh /path/to/x86_64/rux
```

Both scripts reject non-Apple-Silicon hosts. Their Mach-O preflight reads the
ARM64 header and ad-hoc signature bytes itself; it does not invoke an assembler,
linker, signing tool, emulator, or Apple inspection utility.

## Adding Coverage

- Put syntax, semantics, code generation, and runtime language behavior in `Language/<Feature>/`.
- Put public package API behavior in `Packages/<Package>/<Feature>/`.
- Put focused compiler implementation behavior in the relevant `Unit/*Tests.cpp` file.
- Put diagnostic input/expected-output pairs in `Unit/Golden/`.
- Put repository source-layout invariants in `Policy/<Rule>/`.

The C++ manifest-policy tests in `Unit/ManifestTests.cpp` validate every checked-in `Rux.toml`: the schema header and canonical formatting repository-wide, the `Rux` namespace and registry dependency form of publishable first-party packages, and the package type, namespace-free identity, local dependency paths, source entry point and centralized output path of each test manifest.
