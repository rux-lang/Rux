# Rux.toml Manifest

`Rux.toml` describes one Rux package or a workspace of packages. Every manifest starts with an explicit schema version:

```toml
Version = 1
```

Version 1 is a hard cutover. Unversioned files, unsupported version numbers and legacy field spellings are errors; tools do not infer a version or silently upgrade files.

Top-level `Version` is the manifest schema version. `[Package].Version` is a separate TOML path containing the package's SemVer string.

## Package manifests

```toml
Version = 1

[Package]
Namespace = "Rux"
Name = "Math"
Version = "0.1.0"
Type = "Source"
Description = "Mathematical constants and functions"
Authors = ["Rux Contributors <info@rux-lang.dev>"]
Keywords = ["math", "numeric"]
License = "MIT"
Repository = "https://github.com/rux-lang/Rux"
Homepage = "https://rux-lang.dev"

[Dependencies]
"Rux/Memory" = "^0.1.0"

[Build]
Output = "Bin"

[Build.Defines]
CheckedArithmetic = "true"
```

`[Package]` contains:

| Field         | Presence                              | Contract                                               |
| ------------- | ------------------------------------- | ------------------------------------------------------ |
| `Namespace`   | Optional locally; required to publish | One package-identity segment                           |
| `Name`        | Required                              | One package-identity segment                           |
| `Version`     | Required                              | Strict Semantic Versioning 2.0.0 without a leading `v` |
| `Type`        | Required                              | Exactly `Program`, `Library` or `Source`               |
| `Description` | Optional                              | String                                                 |
| `Authors`     | Optional                              | Array of strings                                       |
| `Keywords`    | Optional                              | Array of strings                                       |
| `License`     | Optional                              | String                                                 |
| `Repository`  | Optional                              | String                                                 |
| `Homepage`    | Optional                              | String                                                 |

The scalar legacy form of `Authors` is invalid. Version 1 does not validate URLs or SPDX expressions and adds no field-specific metadata length limits beyond the manifest and package size limits.

Package types determine top-level command behavior:

- `Program` builds an executable and may be run.
- `Library` builds the platform shared-library artifact.
- `Source` may be checked and consumed as a dependency, but `rux build` and
  `rux run` reject it as a top-level target.

`[Build]` is optional. `Output` is a string and defaults to `Bin`. `[Build.Defines]` is an optional string-to-string table whose values are exposed to
compile-time configuration.

## Workspace manifests

```toml
Version = 1

[Workspace]
Packages = [
    "Packages/Math",
    "Packages/Memory",
]
```

A workspace manifest contains a non-empty, duplicate-free string array named `Packages`. `[Workspace]` and `[Package]` are mutually exclusive. A workspace cannot declare dependencies or build settings and cannot be published.

## Package identity

`Namespace` and `Name` are separate identity segments. Each is 1–64 characters, starts with an ASCII letter and otherwise contains only ASCII letters, digits, hyphens or underscores:

```text
^[A-Za-z][A-Za-z0-9_-]{0,63}$
```

Tools preserve spelling for display. Registry lookup and uniqueness lowercase ASCII letters and fold `_` to `-`, so `Rux/My_Pkg` and `rux/my-pkg` identify the same package.

A namespace-free package is local-only. `rux new` and `rux init` therefore retain their simple local workflow and accept an optional `--namespace <Namespace>` when a qualified identity is wanted.

## Dependencies

Registry dependencies use quoted qualified keys:

```toml
[Dependencies]
"Rux/Memory" = "^0.1.0"
"Rux/Text" = ">=1.2.0, <2.0.0"
```

The key is the registry identity and the value is its version requirement. The default import name is the final identity segment. Manifest Version 1 has no alias syntax; two dependencies cannot produce the same import name.

Local path dependencies keep an unqualified import key:

```toml
[Dependencies]
Memory = { Path = "../Memory" }
```

The referenced manifest supplies the dependency identity. Path dependencies are valid for local builds but make a manifest unpublishable. A path and registry dependency cannot claim the same import name.

`rux add Namespace/Name@<requirement>` writes a registry dependency. Omitting the requirement writes `*`. `rux add Name --path <path>` writes a local path dependency.

Workspace overrides match registry dependencies by normalized qualified identity. A namespace-free workspace member cannot override a qualified registry dependency.

## Version requirements

Rux uses a deliberately small SemVer range language:

- `*` matches any stable version.
- A complete version such as `1.2.3` is exact.
- `^1.2.3`, `^0.2.3` and `^0.0.3` have exclusive upper bounds `2.0.0`, `0.3.0` and `0.0.4`.
- `~1.2.3` has the exclusive upper bound `1.3.0`.
- Comparators `=`, `>`, `>=`, `<` and `<=` may be intersected using whitespace or a comma, for example `>=1.2.0, <2.0.0`.

Every operand is a complete strict SemVer value. Requirements reject build metadata, OR expressions, partial versions, component wildcards other than the standalone `*`, and hyphen ranges. A prerelease is eligible only when the requirement explicitly includes a prerelease operand.

Package versions may contain build metadata. The complete version text forms publication identity, while SemVer precedence ignores build metadata.

## Supported TOML and diagnostics

Manifest Version 1 intentionally uses a constrained TOML surface:

- basic quoted strings and integers;
- arrays of basic quoted strings;
- the local path-dependency inline table;
- comments; and
- the tables documented on this page.

Canonical writers emit only this surface. Parsing rejects malformed TOML, duplicate keys, wrong value types, unknown top-level settings, unknown sections, unknown fixed-schema fields, invalid identities or versions, missing required fields and dependency import-name collisions. Keys inside `[Dependencies]` and `[Build.Defines]` are data rather than fixed field names.

Diagnostics identify the manifest path, line and column and explain the rejected field or syntax. Ordinary manifest failures do not throw through compiler or CLI boundaries.

## Canonical serialization

`rux fmt --manifest-only`, `rux add`, `rux remove`, `rux new` and `rux init` write the same order:

1. top-level `Version`;
2. `[Package]` or `[Workspace]`;
3. `[Dependencies]`;
4. `[Build]`; and
5. `[Build.Defines]`.

Only recognized fields are serialized. Required fields never rely on implicit defaults in the file. Metadata arrays retain their order, while dependency and define keys use a stable deterministic order.

## Migration and release

The compiler, CLI, first-party packages, workspaces and tests move to Manifest Version 1 in one atomic repository change. Official first-party packages use `Namespace = "Rux"`; local test packages may remain namespace-free. Registry dependencies become quoted qualified keys and local path keys remain unqualified.

The cutover ships in Rux `0.4.0`, a permitted breaking minor release while Rux is pre-1.0. The release is gated on the package registry accepting the same schema and conformance cases. Remote registry index resolution and publish/download transport remain separate integration work.
