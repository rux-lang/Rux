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

`Print` and `PrintLine` accept values implementing `Display` from [`Rux/Text`](../Text), along with their primitive and text overloads.

## Text and streams

`Display` writes into a `TextWriter` and a stream takes bytes, so something has to stand between them. `ConsoleWriter` is that adapter over standard output and `StreamWriter` is it over any `Writer` — a file, a buffered writer, a pipe — which is what lets a value describe itself anywhere other than standard output or memory:

```rux
import Io::{ IoError, StandardOut, StreamWriter, Writer, WriteValueLine };
import Text::TextWriter;

func Main() -> int {
    var out = StandardOut::Acquire();
    // `StreamWriter` takes a `Writer` by value, and a concrete stream does not coerce to an interface parameter
    // on its own, so the binding is written out.
    let sink: Writer = out;
    let measurement: int32 = 42;

    // The adapter turns a stream into a text writer. It cannot return an I/O error through the text protocol, so
    // it keeps the first one it saw in `failure` — which is why that is checked afterwards rather than at the call.
    var failure = IoError::Ok();
    var adapter = StreamWriter(sink, @failure);
    let text: &var TextWriter = adapter;

    // The type argument is required: the function is generic over the value so that it can borrow it rather than
    // take ownership, and a generic bound is not inferred from an interface value.
    WriteValueLine<int32>(text, measurement);
    if failure.IsError() {
        return 1;
    }
    return 0;
}
```

Neither error type can hold the other's failures, so the crossing is made explicitly in both directions. `FromIoError` turns what a stream reported into what a formatter understands: success becomes success and every failure becomes `WriterFailure`, which is all `FormatError` can say about a destination. That is lossy on purpose, and the adapter makes the loss good by recording the original `IoError` in a slot the caller holds — the formatter learns the one thing it can act on, and the caller learns exactly what happened. Once a failure is recorded the stream is not touched again, so a value part-way through rendering stops rather than writing into a destination that has already refused.

`ToIoError` comes back the other way, and the distinction it preserves is the one that decides what to do next: a request that was wrong is `InvalidInput` and will be wrong again, while a destination that failed came back as an `IoError` in the first place. Bytes that were not text are `InvalidText`; a bounded destination that filled is `LimitExceeded`. Nothing is stuffed into `IoError`'s `raw`, which is `errno` or the Win32 code and is meant to be looked up; a caller who needs to know which formatting failure it was builds a writer and calls `Text::WriteValue` themselves.

`WriteValueLine` and `WriteFormatLine` write a value or a pattern followed by a newline, and write the newline **only if** what came before it went out whole. A line is a value and its terminator together, so a value that could not be written must not be followed by one. Every `PrintLine` overload follows the same rule. `WriteValueLine` takes its value by bound rather than as an interface value, so the value is borrowed rather than given up: binding a move-only value to a `Display` transfers ownership of it, which would let such a value be written exactly once.

Stream helpers borrow concrete implementations directly as `&var Reader` or `&var Writer` interface views, and a named stream passed straight into one of them borrows without any binding: `WriteAll(file, contents)`. The borrow neither copies nor consumes the stream, and it ends at the call. `BufferedReader` and `BufferedWriter` must keep their streams after construction, so they store ordinary interface handles; a stream that cannot be stored that way — because binding it would move it — supplies a stored adapter of its own, such as `FileSystem::FileStream`, whose representation is private and whose lifetime contract is documented where it lives. Both buffered types prohibit copying, move with `<-`, and release their owned buffers through `~BufferedReader` and `~BufferedWriter`.

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
