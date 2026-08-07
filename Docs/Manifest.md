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
Type = "Source"
Description = "Mathematical constants and functions"
Authors = ["Rux Contributors <info@rux-lang.dev>"]
Keywords = ["Math", "Numeric"]
License = "MIT"
Repository = "https://github.com/rux-lang/Rux"
Homepage = "https://rux-lang.dev"
Readme = "README.md"

[Dependencies]
Memory = { Namespace = "Rux", Version = "^0.1.0" }

[Build]
Output = "Dist"

[Build.Defines]
CheckedArithmetic = "true"
```

`[Manifest]` contains:

| Field     | Presence                              | Contract                                                        |
| --------- | ------------------------------------- | --------------------------------------------------------------- |
| `Version` | Required                              | Integer schema version; Version 1 is the only accepted value    |
| `MinRux`  | Optional locally; required to publish | Strict SemVer with precedence at least `0.4.0`                  |

`MinRux` is the oldest compiler release that can build the package. A compiler older than the declared minimum refuses to build, check, run, test or install the package; manifest editing stays available. Omitting it locally keeps `rux new`, `rux init` and repository test packages free of a field only publication needs.

`[Package]` contains:

| Field         | Presence                              | Contract                                                |
| ------------- | ------------------------------------- | ------------------------------------------------------- |
| `Namespace`   | Optional locally; required to publish | One identity segment                                    |
| `Name`        | Required                              | One identity segment                                    |
| `Version`     | Required                              | Strict Semantic Versioning 2.0.0 without a leading `v`  |
| `Type`        | Required                              | Exactly `Program`, `Library` or `Source`                |
| `Description` | Optional                              | String                                                  |
| `Authors`     | Optional                              | Array of strings                                        |
| `Keywords`    | Optional                              | Array of identity segments, unique after normalization  |
| `License`     | Optional                              | SPDX expression; cannot be combined with `LicenseFile`  |
| `LicenseFile` | Optional                              | Package-relative path; cannot be combined with `License` |
| `Repository`  | Optional                              | Absolute `http`/`https` URL with a host and no credentials |
| `Homepage`    | Optional                              | Absolute `http`/`https` URL with a host and no credentials |
| `Readme`      | Optional                              | Package-relative path                                   |

The scalar legacy form of `Authors` is invalid.

Package types determine top-level command behavior:

- `Program` builds an executable with a `Main` entry point and may be run.
- `Library` builds the platform shared-library artifact, linked by dependents and loaded at run time.
- `Source` is compiled directly into dependent packages. It may be checked and consumed as a dependency, but `rux build` and `rux run` reject it as a top-level target.

There is no separate `SharedLibrary` type because `Library` already denotes one, and static archives are not a package type.

`rux new` and `rux init` create a `Program` by default and with `--bin`, a `Library` with `--lib`, and a `Source` package with `--source`. The three flags are mutually exclusive.

`[Build]` is optional. `Output` is a package-relative path and defaults to `Bin`. `[Build.Defines]` is an optional table whose values are exposed to compile-time configuration.

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
```

A registry dependency requires `Namespace` and `Version`. A path dependency requires `Path` and cannot carry `Namespace` or `Version`. Either form may set `Package` to name a package whose spelling differs from the import name; `Package` defaults to the import name.

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

Manifest paths are UTF-8, relative and `/`-separated. Backslashes, absolute roots, empty components and `.` components are invalid. `Readme`, `LicenseFile` and workspace paths also reject `..`. Dependency and output paths may begin with `..` components, but parent traversal cannot follow a normal component.

Diagnostics identify the manifest path, line and column and explain the rejected field or syntax. Ordinary manifest failures do not throw through compiler or CLI boundaries.

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

## Canonical serialization

`rux fmt --manifest-only`, `rux add`, `rux remove`, `rux new` and `rux init` write the same order:

1. `[Manifest]`;
2. `[Package]` or `[Workspace]`;
3. `[Dependencies]`;
4. `[Build]`; and
5. `[Build.Defines]`.

Only recognized fields are serialized. Required fields never rely on implicit defaults in the file. Metadata arrays retain their order, while dependency and define keys use a stable deterministic order.

## Migration and release

The compiler, CLI, first-party packages, workspaces and tests move to Manifest Version 1 in one atomic repository change. Official first-party packages use `Namespace = "Rux"`; local test packages may remain namespace-free. The cutover ships in Rux `0.4.0`, a permitted breaking minor release while Rux is pre-1.0. The release is gated on the package registry accepting the same schema and conformance cases. Remote registry index resolution and publish/download transport remain separate integration work.
