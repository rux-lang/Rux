# Rux.toml Manifest

`Rux.toml` describes one Rux package or a workspace of packages. It is a case-sensitive TOML document whose schema-owned section names, field names and enum values are PascalCase. Every manifest starts with an explicit schema version:

```toml
[Manifest]
Version = 1
```

Version 1 is a hard cutover. Unversioned files, unsupported version numbers and legacy field spellings are errors; tools do not infer a version or silently upgrade files.

`[Manifest].Version` is the manifest schema version. `[Package].Version` is a separate TOML path containing the package's SemVer string.

The compiler and the [package registry](https://github.com/rux-lang/Server) implement one schema. Where this page and the registry's `docs/manifest.md` describe the same field, they describe the same rules, and both sides carry the same positive and negative conformance cases.

## Package manifests

```toml
[Manifest]
Version = 1
MinRux = "0.4.0"

[Package]
Namespace = "Rux"
Name = "Math"
Version = "0.1.0"
Type = "SourceLibrary"
Description = "Mathematical constants and functions"
Authors = ["Rux Contributors <info@rux-lang.dev>"]
Keywords = ["Math", "Numeric"]
License = "MIT"
LicenseFile = "LICENSE.md"
Repository = "https://github.com/rux-lang/Rux"
Homepage = "https://rux-lang.dev"
ReadmeFile = "README.md"

[Dependencies]
Memory = { Namespace = "Rux", Version = "^0.1.0" }

[Build]
Output = "Dist"

[Build.Defines]
CheckedArithmetic = "true"
```

`[Manifest]` contains:

| Field     | Presence                              | Contract                                                     |
| --------- | ------------------------------------- | ------------------------------------------------------------ |
| `Version` | Required                              | Integer schema version; Version 1 is the only accepted value |
| `MinRux`  | Optional locally; required to publish | Strict SemVer with precedence at least `0.4.0`               |

`MinRux` is the oldest compiler release that can build the package. A compiler older than the declared minimum refuses to build, check, run, test or install the package; manifest editing stays available. Omitting it locally keeps `rux new`, `rux init` and repository test packages free of a field only publication needs.

`[Package]` contains:

| Field         | Presence                              | Contract                                                                  |
| ------------- | ------------------------------------- | ------------------------------------------------------------------------- |
| `Namespace`   | Optional locally; required to publish | One identity segment                                                      |
| `Name`        | Required                              | One identity segment                                                      |
| `Version`     | Required                              | Strict Semantic Versioning 2.0.0 without a leading `v`                    |
| `Type`        | Required                              | Exactly `Executable`, `SharedLibrary`, `StaticLibrary` or `SourceLibrary` |
| `Description` | Optional                              | String                                                                    |
| `Authors`     | Optional                              | Array of strings                                                          |
| `Keywords`    | Optional                              | Array of identity segments, unique after normalization                    |
| `License`     | Optional                              | SPDX expression                                                           |
| `LicenseFile` | Optional                              | Package-relative path                                                     |
| `Repository`  | Optional                              | Absolute `http`/`https` URL with a host and no credentials                |
| `Homepage`    | Optional                              | Absolute `http`/`https` URL with a host and no credentials                |
| `ReadmeFile`  | Optional                              | Package-relative path                                                     |

The scalar legacy form of `Authors` is invalid.

`License` names the terms and `LicenseFile` carries their text inside the package. They are independent, so a package may declare either, both, or neither, and setting both is the norm: the expression is what a dependency-tree license audit reads, while the file holds the copyright holder and year an SPDX identifier cannot express. A package whose terms are not a well-known SPDX expression should publish a `LicenseFile` alongside SPDX's own `LicenseRef-` form.

Package types determine top-level command behavior:

- `Executable` builds a runnable program with a `Main` entry point.
- `SharedLibrary` builds `Name.dll` plus `Name.lib` on Windows, `libName.so` on ELF targets, or `libName.dylib` on macOS. It cannot run.
- `StaticLibrary` builds `Name.lib` on Windows or `libName.a` on ELF targets and macOS. It cannot run.
- `SourceLibrary` is compiled directly into dependent packages. It may be checked and consumed as a dependency, but cannot build or run as a top-level target.

The retired `Program`, `Library`, and `Source` spellings are invalid and have no aliases. Dependency consumption remains source-based in 0.4.0 even when a local dependency declares `SharedLibrary` or `StaticLibrary`.

`rux new` and `rux init` select mutually exclusive `--executable`, `--shared`, `--static`, and `--source` modes. Executable is the default; `--bin` and `--lib` are invalid.

`[Build]` is optional. `Output` is a package-relative path and defaults to `Bin`. `[Build.Defines]` is an optional table whose values are exposed to compile-time configuration.

An ordinary artifact always writes to `<Output>/<Profile>/<OS>/<Arch>/`, including when the target is the host. For example, `rux build --release` on a Windows x86-64 host produces `Dist/Release/Windows/x86-64/Name.exe`; `--target macos-arm64` canonicalizes to `Dist/Release/macOS/AArch64/Name`. Profile and target directories therefore never collide. Artifact names follow the target rather than the host: `Name.exe` for Windows, `libName.dylib` for a macOS shared library, `libName.a` for a macOS static library, and `libName.so` on ELF targets.

`rux build --all` applies this layout to all eight canonical targets in both profiles, producing 16 distinct directories below the configured root. The [package build guide](Builds.md) lists their exact order and documents the matrix flag and reporting rules.

Outputs that are not ordinary machine artifacts use the configured root explicitly. Native `rux test` artifacts go directly below the test manifest's `Output`; selecting another executable architecture adds the same OS and architecture components but no profile. `rux doc` defaults to `<Output>/Docs`, and `rux pack` writes its target-independent `.ruxpkg` directly to `<Output>`. `rux clean` removes exactly the configured output root and the package's `Temp` tree.

| Output kind                        | Layout                                  |
| ---------------------------------- | --------------------------------------- |
| Normal Debug artifact              | `<Output>/Debug/<OS>/<Arch>/`           |
| Normal Release artifact            | `<Output>/Release/<OS>/<Arch>/`         |
| Native test artifact               | `<Output>/`                             |
| Explicit non-host test artifact    | `<Output>/<OS>/<Arch>/`                 |
| Generated documentation            | `<Output>/Docs/`                        |
| Published or locally packed source | `<Output>/<Name>-<Version>.ruxpkg`      |

## Workspace manifests

```toml
[Manifest]
Version = 1

[Workspace]
Packages = [
    "Packages/Math",
    "Packages/Memory",
]
```

A workspace manifest contains a non-empty, duplicate-free array of explicit relative paths named `Packages`. Glob patterns and parent traversal are not supported. `[Workspace]` and `[Package]` are mutually exclusive; a manifest contains exactly one of them. A workspace cannot declare dependencies or build settings and cannot be published.

## Package identity

`Namespace` and `Name` are separate identity segments. A segment is 1–64 bytes of ASCII alphanumeric characters separated by single `-` or `_` characters. It cannot start or end with a separator and cannot contain two adjacent separators:

```text
Rux        My_Pkg        my-pkg        7zip
-Pkg       Pkg-          My__Pkg       My.Pkg      (all invalid)
```

Tools preserve spelling for display. Registry lookup and uniqueness lowercase ASCII letters and fold `_` to `-`, so `Rux/My_Pkg` and `rux/my-pkg` identify the same package. `Keywords` use the same grammar and must stay unique after normalization.

A namespace-free package is local-only. `rux new` and `rux init` therefore retain their simple local workflow and accept an optional `--namespace <Namespace>` when a qualified identity is wanted.

## Dependencies

Each `[Dependencies]` key is the local import name and each value is an inline table:

```toml
[Dependencies]
Io = { Namespace = "Rux", Version = "^1.0.0" }
Json = { Namespace = "Acme", Package = "FastJson", Version = ">=2.0.0, <3.0.0" }
Util = { Path = "../Util" }
Windows = { Namespace = "Rux", Version = "0.1.0", TargetOS = ["Windows"] }
```

A registry dependency requires `Namespace` and `Version`. A path dependency requires `Path` and cannot carry `Namespace` or `Version`. Either form may set `Package` to name a package whose spelling differs from the import name; `Package` defaults to the import name.

Either form may also declare a non-empty `TargetOS` allow-list. The dependency participates in builds and package resolution only when the selected target operating system appears in the list; omitting `TargetOS` makes it unconditional. The accepted values are `FreeBSD`, `Linux`, `macOS` and `Windows`. Values are exact and cannot repeat. A dependency used only on macOS therefore declares `TargetOS = ["macOS"]`.

The import name is an identity segment, and two dependencies cannot produce the same import name after normalization. Path dependencies are valid for local builds but make a manifest unpublishable.

`rux add Namespace/Name@<requirement>` writes a registry dependency. Omitting the requirement writes `*`. `rux add Name --path <path>` writes a local path dependency.

Workspace overrides match registry dependencies by normalized qualified identity. A namespace-free workspace member cannot override a qualified registry dependency.

## Version requirements

A requirement is a comma-separated intersection of comparators; surrounding whitespace is insignificant:

- `*`, `x` or `X` alone matches any stable version. A wildcard cannot be combined with another comparator.
- Comparators `=`, `>`, `>=`, `<` and `<=` compare against their operand, for example `>=1.2.0, <2.0.0`.
- `^1.2.3`, `^0.2.3` and `^0.0.3` have exclusive upper bounds `2.0.0`, `0.3.0` and `0.0.4`.
- `~1.2.3` has the exclusive upper bound `1.3.0`.
- An operand with no operator is a caret requirement, so `1.2.3` means `^1.2.3`. Write `=1.2.3` for an exact match.

Operands may be partial: `^1.0` and `2` omit trailing components, and a trailing `*`, `x` or `X` is a component wildcard, as in `1.2.*`. A numeric component cannot follow a wildcard, and a prerelease or build suffix requires a complete `major.minor.patch` operand. Build metadata in an operand is accepted and ignored, because it never affects matching. Requirements reject OR expressions, hyphen ranges, space-separated comparators, more than three numeric components and more than 32 comparators.

A prerelease is eligible only when some comparator names the same `major.minor.patch` and carries a prerelease operand of its own.

Package versions may contain build metadata. The complete version text forms publication identity, while SemVer precedence ignores build metadata.

## Supported TOML and diagnostics

Manifest Version 1 intentionally uses a constrained TOML surface:

- basic quoted strings, integers and booleans;
- arrays of basic quoted strings;
- the dependency inline table;
- comments; and
- the tables documented on this page.

Canonical writers emit only this surface. Parsing rejects malformed TOML, duplicate keys, wrong value types, unknown top-level settings, unknown sections, unknown fixed-schema fields, invalid identities or versions, missing required fields and dependency import-name collisions. Keys inside `[Dependencies]` and `[Build.Defines]` are data rather than fixed field names.

Manifest paths are UTF-8, relative and `/`-separated. Backslashes, absolute roots, empty components and `.` components are invalid. A field whose name ends in `File` names a path inside the package and, like workspace paths, rejects `..`. Dependency and output paths may begin with `..` components, but parent traversal cannot follow a normal component.

Manifest URLs are absolute `http` or `https`, carry a host, and reject credentials, whitespace and control characters.

Diagnostics use `path:line:column: error: message`, retain the rejected source line, and name the owning table and field
for schema failures. Independent schema failures are reported together in source order; malformed TOML stops syntax
parsing at the first point where continuing would require guessing. Case-only mistakes include the canonical spelling,
type failures name the expected TOML type, and enum failures list their allowed values. Stable schema diagnostics link
back to this manifest reference. Manifest input must be valid UTF-8. Ordinary manifest failures do not throw through
compiler or CLI boundaries.

## Limits

All limits count UTF-8 bytes:

| Resource                          |    Limit |
| --------------------------------- | -------: |
| Manifest source                   |   65,536 |
| Dependencies / workspace packages | 256 each |
| Defines per table                 |      128 |
| Authors / keywords                |  32 each |
| Description                       |    2,048 |
| Author                            |      256 |
| URL or path                       |    2,048 |
| SPDX expression / version range   |      512 |
| Semantic version                  |      256 |
| Identity segment                  |       64 |

## Validation profiles

The selected validation policy is not stored in `Rux.toml`. Local validation accepts package and workspace manifests, allows a package to omit `Namespace` and `MinRux`, and permits path dependencies. Publication validation accepts only package manifests, requires `Namespace` and `MinRux`, and rejects every path dependency.

`rux publish` and `rux pack` apply the publication profile before doing any other work, so a manifest that cannot be published is reported locally rather than by the registry. Every other command applies the local profile.

## Publishing

`rux pack` builds the package archive and `rux publish` uploads it. In Rux 0.4.0 both accept only `Type = "SourceLibrary"`. Other valid types are rejected before credential lookup, archive construction, filesystem output, or network access with:

```text
[Package].Type = "<actual>" cannot be published by Rux 0.4.0; this release publishes only Type = "SourceLibrary"
```

The archive is a ZIP named `<Name>-<Version>.ruxpkg`. It contains `Rux.toml` at its root, every regular file below `Src/`, and the files named by `ReadmeFile` and `LicenseFile`. It must contain at least one `Src/**/*.rux` source. Entry paths are relative, `/`-separated UTF-8; entries are sorted and carry a fixed timestamp, so packing one tree twice produces identical bytes. Publication uploads the manifest beside the archive and the two copies must match byte for byte, comments and line endings included.

```sh
rux login
rux pack
rux publish --dry-run
rux publish
```

The registry defaults to the official API and is overridden by `RUX_REGISTRY_URL` or `--registry <url>`, which is how a local registry is targeted for testing. `--dry-run` validates and builds the archive without uploading and needs no credential.

### Credentials

`rux publish` needs a bearer credential carrying the registry's `publish` scope, and takes it from the first of these that applies:

1. the `RUX_TOKEN` environment variable;
2. the token `rux login` stored for the registry being published to.

The environment wins so a CI job is never shadowed by a file left behind on a self-hosted runner. Neither command has a `--token` flag: `rux login` reads the token from stdin — prompting without echo on a terminal, otherwise reading one line, so `echo "$TOKEN" | rux login` works — which keeps the credential out of shell history and the process list.

Stored tokens live in `%LOCALAPPDATA%\Rux\Credentials.toml` on Windows and `$HOME/.rux/credentials.toml` elsewhere, beside the package cache, restricted to your account:

```toml
[Registry."https://api.rux-lang.dev"]
Token = "rux_pat_..."
```

Entries are keyed by registry base URL rather than kept as one ambient token, so pointing `--registry` at a local registry cannot send it the credential for the official one. `rux logout` removes the entry for one registry and leaves the rest.

`rux login` checks the token against the registry before storing it and refuses one that is rejected. A registry that does not implement the check, or that cannot be reached, produces a warning and the token is stored unverified.

A published version is immutable: republishing an existing `major.minor.patch` — build metadata included — is rejected. `rux publish` does not build or check the package first; run `rux check` beforehand.

## Installation and resolution

`rux install` reads the registry's resolver index and downloads published archives. It contacts no host other than the selected registry, and `--registry` and `RUX_REGISTRY_URL` retarget resolution and download exactly as they retarget publication. No credential is involved: the read side is public.

Resolution walks the dependency graph breadth-first from the root manifest — or from every member of a workspace — and asks `/v1/index/<namespace>/<package>` for each package it reaches. `rux install` and `rux update` use the host target by default and accept `--target <triple>` to resolve for another supported target. Root requirements and transitive registry edges whose `TargetOS` allow-list excludes that target are pruned before they constrain or enter the graph. Installing an explicitly named package still installs that package; the target only filters its transitive dependencies.

A candidate version is eligible when it is not yanked, its `MinRux` is no newer than the running compiler, and it satisfies every applicable requirement gathered for that package. The highest eligible version wins, ordered by SemVer precedence with build metadata as the tie-break. A package resolves to one version; requirements that cannot be satisfied together are reported rather than resolved arbitrarily. `rux update` runs the same resolution and installs the newest version each requirement now allows. Registry index dependency objects carry an optional `target_os` array using the manifest spellings; an omitted field preserves the existing unconditional-edge behavior.

Each selected version is downloaded as its `.ruxpkg`, checked against the SHA-256 the registry publishes for it, and unpacked under the same archive contract `rux pack` applies — entry paths cannot escape the package, and the declared size and count limits hold. A download that fails any of those checks installs nothing. The unpacked manifest must also carry the identity and version it was published under.

Packages are cached per exact version. The namespace and name directories use the spelling the package was published under, and are matched by their normalized form, so a manifest that spells a dependency differently still resolves to the one entry. The leaf is the complete version text:

```text
%LocalAppData%\Rux\Packages\Rux\Io\0.1.0\   (Windows)
~/.rux/packages/Rux/Io/0.1.0/               (Unix-like hosts)
```

Several versions of one package can therefore be installed at once. `rux build`, `rux check`, `rux run` and `rux test` select among them locally — the highest installed version matching the manifest requirement — so a build never contacts the registry. A requirement with no matching installed version is an error naming the versions that are installed.

## Canonical serialization

`rux fmt --manifest-only`, `rux add`, `rux remove`, `rux new` and `rux init` write the same order:

1. `[Manifest]`;
2. `[Package]` or `[Workspace]`;
3. `[Dependencies]`;
4. `[Build]`; and
5. `[Build.Defines]`.

Only recognized fields are serialized. Required fields never rely on implicit defaults in the file. Metadata arrays retain their order, while dependency and define keys use a stable deterministic order.

## Migration and release

The compiler, CLI, first-party packages, workspaces and tests move to Manifest Version 1 in one atomic repository change. Official first-party packages use `Namespace = "Rux"`; local test packages may remain namespace-free. The cutover ships in Rux `0.4.0`, a permitted breaking minor release while Rux is pre-1.0. The release is gated on the package registry accepting the same schema and conformance cases, and on it serving the resolver index and download routes `rux install` now reads.

The package cache changed shape with the switch to registry installs. A cache written by an earlier release stored one unversioned directory per bare package name; `rux install` and `rux update` delete those entries when they find them, and the packages are reinstalled under their qualified identity.
