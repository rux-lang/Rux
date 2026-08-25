# Path

Native operating-system strings and filesystem paths.

## Installation

```sh
rux add Rux/Path
```

## What it provides

- **`OsString` and `OsStringView`** — text as the operating system actually stores it: arbitrary bytes on the Unix
  systems, arbitrary UTF-16 code units on Windows, unpaired surrogates included. Lossless by construction: whatever
  name the system hands over survives the round trip, whether or not it is valid text. Conversion **from** UTF-8
  always succeeds; conversion **to** UTF-8 is fallible, because the system's names are under no obligation to be
  text.

- **`Path` and `PathBuffer`** — borrowed structure and the owning, growable buffer; component iteration where
  separator runs count once and an absolute path opens with an empty root component.
- **Parts and joins** — `Parent`, `FileName`, `Stem`, `Extension`, `Join`, and lexical `Normalize`, each edge
  decided in writing — including that `x/..` simplification is the truth about directories and a lie about
  symlinks, with the filesystem-true answer deferred to canonicalization.
- **Prefixes** — the Windows family (drive, UNC share, verbatim) behind `PrefixLength`, `HasRoot` and
  `IsAbsolute`; the Unix systems have no prefixes, and nothing here folds case.

`Path` and `OsStringView` are copyable borrowed values whose receivers use `&T`. Immutable `OsString` has deep-copy
value semantics: a language copy allocates an independent native-unit block from the same allocator. `PathBuffer`
prohibits copying, transfers with `<-`, and releases its block through `~PathBuffer`.

Use `Path()`, `OsStringView()`, `OsString(allocator)`, and `PathBuffer(allocator)` for empty values. Descriptive and
fallible factories such as `FromText`, `FromPath`, `Join`, and `Normalize` retain their names. Raw pointers remain
only for native storage and scalar output slots, where nullable or stored addresses are required.

## Documentation

<https://rux-lang.dev/docs/api/path>

## License

Licensed under the [MIT License](LICENSE.md).
