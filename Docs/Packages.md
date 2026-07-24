# First-Party Packages

First-party Rux packages live under `Packages/` in the repository root. Return to the [main README](../README.md) for the complete documentation index.

## Package Status

Packages marked **Planned** reserve their package names and source layout but do not expose an API yet. **Partial** means some modules are usable while others are still placeholders.

| Package       | Status    | Description                                              |
| ------------- | --------- | -------------------------------------------------------- |
| `Algorithms`  | Planned   | Generic algorithms over slices, ranges, and iterators    |
| `C`           | Available | C standard library bindings                              |
| `Collections` | Partial   | Generic data structures; `Array` and `List` are usable   |
| `Format`      | Available | String conversion and formatting                         |
| `Hash`        | Planned   | Hash functions and checksums                             |
| `Io`          | Available | Streams, console I/O, readers, and writers               |
| `Json`        | Planned   | JSON parsing and serialization                           |
| `Math`        | Available | Mathematical constants and functions                     |
| `Memory`      | Available | Memory-management functions                              |
| `Random`      | Planned   | Pseudorandom generators and distributions                |
| `Rux`         | Available | Core language intrinsics                                 |
| `Text`        | Available | Strings and fundamental text manipulation                |
| `Time`        | Planned   | Durations, monotonic clocks, and wall clocks             |
| `Uuid`        | Planned   | UUID representation, parsing, formatting, and generation |

## Target-Specific Packages

The `Bsd`, `Linux`, `MacOS`, and `Windows` packages provide operating-system bindings. Source packages can conditionally import these packages according to the compilation target.

## Package Layout and Tests

Each package has a manifest and source directory. Executable package tests are centralized under `Tests/Packages/` and use local first-party dependencies:

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
