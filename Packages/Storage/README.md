# Storage

Files, directories and filesystem operations.

## Installation

```sh
rux add Rux/Storage
```

## What it provides

- **`File` and `OpenOptions`** — an owned handle the type system closes exactly once, opened under options that
  say what may happen (`Read`, `Write`, `Append`) and what to do about existence (`Create`, `CreateNew`,
  `Truncate`), with the contradictions refused before the platform is asked.
- **Native error translation** — every platform failure arrives as an `Io::IoError`: the kind a caller acts on,
  with the platform's raw code preserved beside it, translated by one table per platform rather than ad hoc at
  each call site.

## Documentation

<https://rux-lang.dev/docs/api/storage>

## License

Licensed under the [MIT License](LICENSE.md).
