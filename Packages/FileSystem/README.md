# FileSystem

Files, directories and filesystem operations.

## Installation

```sh
rux add Rux/FileSystem
```

## What it provides

- **`File` and `OpenOptions`** — a move-only owned handle the type system closes exactly once and copyable options created with `OpenOptions()`. Safe operations borrow the handle through `&File` or `&var File`; only stored platform handles and explicit output slots remain raw pointers.
- **Deterministic owners** — directory iterators, temporary files and directories, and atomic writes prohibit copying, transfer with `<-`, and clean up through `~Type` destructors. Fallible operations such as `Open`, `Create`, and `Begin` keep their descriptive names.
- **A file is a stream, and a stream is usually a borrow** — `File` implements `Reader`, `Writer` and `Seeker`, so anything wanting a stream for the length of a call takes the file:

  ```rux
  WriteAll(file, contents);
  ```

  The borrow is checked and ends at the call, which means the compiler also refuses reading the file through while a borrow of it is still live. `FileStream` is the stored form, for a value such as a `BufferedReader` that keeps its source past the call that made it — binding the file itself into an interface value there would move it, and there would be nothing left to close. Its representation is private and `File::Stream` is the only way to obtain one; it must not outlive the file or survive a move of it, and nothing tracks either. A stream over a *closed* file is not unsafe: the file checks its own handle, so every operation refuses with `InvalidHandle`. `AtomicWrite` is itself a `Writer`, so replacing a file needs no adapter at all.

- **Native error translation** — every platform failure arrives as an `Io::IoError`: the kind a caller acts on, with the platform's raw code preserved beside it, translated by one table per platform rather than ad hoc at each call site.

## Documentation

<https://rux-lang.dev/docs/api/filesystem>

## License

Licensed under the [MIT License](LICENSE.md).
