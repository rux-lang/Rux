# Io

Streams, console I/O, readers and writers.

Console access is written directly against each platform's own primitives, selected at compile time: the `write`/`read` syscalls on Linux, macOS, and the BSDs, and `WriteFile`/`ReadFile` on Windows. There is no C runtime in the path.

## Installation

```sh
rux add Rux/Io
```

## What it provides

| Function       | Purpose                                      |
| -------------- | -------------------------------------------- |
| `Print`        | Write a value to standard output             |
| `PrintLine`    | Write a value followed by a newline          |
| `ReadLine`     | Append one line from standard input          |
| `ReadExact`    | Fill a byte slice from any reader            |
| `WriteAll`     | Send a byte slice to any writer              |
| `ReadTextLine` | Append one validated line from a byte stream |

`Print` and `PrintLine` accept values implementing `Display` from [`Rux/Format`](../Format), along with their
primitive and text overloads.

Stream helpers borrow concrete implementations directly as `&var Reader` or `&var Writer` interface views. The
borrow neither copies nor consumes the stream. `BufferedReader` and `BufferedWriter` must keep their streams after
construction, so they store ordinary interface handles; raw-pointer adapters such as `Storage::FileStream` remain
for that deliberately escaping case. Both buffered types prohibit copying, move with `<-`, and release their owned
buffers through `~BufferedReader` and `~BufferedWriter`.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Io::{ PrintLine, ReadLine };
import Text::StringBuilder;

func Main() -> int {
    var system = SystemAllocator();
    let allocator: Allocator = system;
    var name = StringBuilder(allocator);
    PrintLine("What is your name?");
    if ReadLine(name).IsOk() {
        let text = name.IntoString();
        PrintLine(text);
    }
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/io>

## License

Licensed under the [MIT License](LICENSE.md).
