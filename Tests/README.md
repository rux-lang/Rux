# Tests

All repository tests live below this directory and fall into four explicit categories:

| Path                         | Owner and runner                                                        |
| ---------------------------- | ----------------------------------------------------------------------- |
| `Language/<Test>/`           | Black-box language/compiler behavior; `rux test`                        |
| `Packages/<Package>/<Test>/` | Black-box first-party package behavior; `rux test`                      |
| `Unit/`                      | Compiler internals and `Unit/Golden/` diagnostic fixtures; CTest        |
| `Policy/`                    | Source-tree invariants enforced directly by scripts or CI               |

## Rux Test Packages

Every executable Rux test contains `Rux.toml` and `Src/Main.rux`. Exit code `0` passes; any other exit code fails. Run every package from the repository root:

```sh
./Bin/rux test --release
```

To run the complete repository workflow—including policy, formatting, build, CTest, workspace checks, lint, and these packages—use `./Test.ps1` on Windows or `sh Test.sh` on Linux, macOS, and FreeBSD.

Test manifests are intentionally uniform:

- `Type = "bin"` is explicit.
- Language outputs go to `Bin/Tests/Language/`.
- Package outputs go to `Bin/Tests/Packages/<Package>/`.
- Every dependency uses `Path = ...` and resolves below the root `Packages/` directory.
- Registry dependencies are forbidden in test manifests.

During workspace tests, transitive dependencies in publishable first-party package manifests are resolved from matching local workspace members. Registry fallback is disabled, so the suite does not require `rux install`, a populated package cache, or network access.

## Adding Coverage

- Put syntax, semantics, code generation, and runtime language behavior in `Language/<Feature>/`.
- Put public package API behavior in `Packages/<Package>/<Feature>/`.
- Put focused compiler implementation behavior in the relevant `Unit/*Tests.cpp` file.
- Put diagnostic input/expected-output pairs in `Unit/Golden/`.
- Put repository source-layout invariants in `Policy/<Rule>/`.

The C++ manifest-policy test validates every Rux test manifest, local dependency path, executable type, source entry point, and centralized output path.
