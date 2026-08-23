# Toml

TOML parsing, serialization and streaming.

## Installation

```sh
rux add Rux/Toml
```

## What it provides

- **Lexing** — every token TOML 1.1 defines, with source spans: bare and quoted keys, the four string forms,
  integers in four bases, floats, booleans, and the date and time types.
- **Parsing** — dotted keys, tables, arrays of tables, and the duplicate-definition rules that make TOML's
  table syntax unambiguous.
- **A semantic DOM** — ordered tables and arrays with move-safe ownership, and dates and times as `Rux/Time`
  values rather than strings.
- **Writing** — deterministic output that chooses a valid key and string form for whatever it is given.

> **Semantic parsing is not lossless.** Comments and original whitespace are not preserved, so a document read
> and written back is equivalent but not identical. This is not an editing library.

## Documentation

<https://rux-lang.dev/docs/api/toml>

## License

Licensed under the [MIT License](LICENSE.md).
