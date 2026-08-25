# Storage

Files, directories and filesystem operations.

## Installation

```sh
rux add Rux/Storage
```

## What it provides

- **`File` and `OpenOptions`** — a move-only owned handle the type system closes exactly once and copyable
  options created with `OpenOptions()`. Safe operations borrow the handle through `&File` or `&var File`; only
  stored platform handles and explicit output slots remain raw pointers.
- **Deterministic owners** — directory iterators, temporary files and directories, and atomic writes prohibit
  copying, transfer with `<-`, and clean up through `~Type` destructors. Fallible operations such as `Open`,
  `Create`, and `Begin` keep their descriptive names.
- **Native error translation** — every platform failure arrives as an `Io::IoError`: the kind a caller acts on,
  with the platform's raw code preserved beside it, translated by one table per platform rather than ad hoc at
  each call site.

## Documentation

<https://rux-lang.dev/docs/api/storage>

## License

Licensed under the [MIT License](LICENSE.md).
