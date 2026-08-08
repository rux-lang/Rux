# Io

Streams, console I/O, readers, and writers.

Console access is written directly against each platform's own primitives, selected at compile time: the `write`/`read` syscalls on Linux, macOS, and the BSDs, and `WriteFile`/`ReadFile` on Windows. There is no C runtime in the path.

## Installation

```sh
rux add Rux/Io
```

## What it provides

| Function        | Purpose                                      |
| --------------- | -------------------------------------------- |
| `Print`         | Write a value to standard output             |
| `PrintLine`     | Write a value followed by a newline          |
| `ReadLine`      | Read one line from standard input            |
| `ReadStdinByte` | Read a single byte from standard input       |

`Print` and `PrintLine` accept any type implementing `Stringable` from [`Rux/Format`](../Format).

## Example

```rux
import Io::{ PrintLine, ReadLine };

func Main() -> int {
    PrintLine("What is your name?");
    var name = ReadLine();
    PrintLine(name);
    name.Free();
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/io>

## License

Licensed under the [MIT License](LICENSE.md).
