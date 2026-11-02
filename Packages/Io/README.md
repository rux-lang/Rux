# Io

Streams, console I/O, readers and writers.

Console access is written directly against each platform's own primitives, selected at compile time: the `write`/`read` syscalls on Linux, macOS, and the BSDs, and `WriteFile`/`ReadFile` on Windows. There is no C runtime in the path.

## Installation

```sh
rux add Rux/Io
```

## What it provides

| Function       | Purpose                                          |
| -------------- | ------------------------------------------------ |
| `Print`        | Write a value to standard output                 |
| `PrintLine`    | Write a value followed by a newline              |
| `ReadLine`     | Append one line from standard input              |
| `ReadLineFrom` | Append one line from any reader, byte by byte    |
| `ReadExact`    | Fill a byte slice from any reader                |
| `WriteAll`     | Send a byte slice to any writer                  |
| `ReadTextLine` | Append one validated line from a buffered reader |

`Print` and `PrintLine` accept values implementing `Display` from [`Rux/Text`](../Text), along with their primitive and text overloads. The primitive overloads cover every boolean width from `bool8` to `bool512`, the floats `float8`, `float16`, `float32` and `float64`, the integers up to 64 bits with `int` and `uint`, and every character width from `char8` to `char512`. `float80` and wider have no overload, because their text allocates: render one through [`Rux/Format`](../Format)'s `Float80` to `Float512` values with `FromValue` and `FormatWith`, passing the allocator. They return the failure of the write as an `IoError?` — `none` when everything was written — rather than throwing it, so a bare `PrintLine("ready");` statement needs no handling while a caller who cares writes `if PrintLine(report) is IoError { ... }`.

Given arguments after it, the first text is a pattern: `PrintLine("{} of {}", done, total)` fills each `{}` or `{:spec}` placeholder from the next argument, and `{{` and `}}` write a literal brace. The pattern parameter is marked `#Format`, so when it is a string literal the compiler counts its placeholders against the arguments, and `PrintLine("{} {}", 1)` is a compile error rather than a silent failure at run time. A pattern known only at run time that does not match reports `InvalidInput` through the returned `IoError?` and prints nothing. With no arguments the text is not a pattern at all: `PrintLine("{{}}")` writes `{{}}` verbatim, braces and all.

Both line readers hold the same contract: a line feed ends a line, a carriage return immediately before it belongs to the ending, the last line needs no ending, and the line must be UTF-8. A line that is not is `InvalidText`, reported once the line has been consumed through its ending, so the next call starts on the next line. `ReadLine` and `ReadLineFrom` read one byte at a time and never take a byte past the line, which is right for standard input or any stream shared with whatever reads next; `ReadTextLine` reads through a `BufferedReader` and also bounds the line's length.

Every stream operation that can fail throws an `IoError`, so every call to one is marked with `try`: `Read` returns the number of bytes read, `Write` the number taken, `Seek` the new position, and `Flush`, `ReadExact`, `WriteAll`, and the line readers return nothing. The end of a stream is the `EndOfStream` error, which `IsEnd` recognizes; the helpers also take a read of zero bytes into a non-empty buffer as the end rather than asking again. `ReadExact` fails with `EndOfStream` when the stream ends before its first byte, so a loop reading records stops cleanly between them, and with `UnexpectedEnd` when it ends partway through the buffer. `IsInterrupted` names the one report worth retrying at once, and the helpers retry it. A `WouldBlock` from a non-blocking stream is returned to the caller instead, never retried: asking again before the stream is ready only spins, so the caller waits for readiness, as a `Net` `Poller` reports it, and reads or writes incrementally with `Read` and `Write`, since `ReadExact` and `WriteAll` do not report how far they got. An `IoError` renders through `{}` as its kind's words and the platform's code, as `not found (os error 2)`.

## Streams in memory

`ByteReader` reads a slice of bytes the caller holds and `ByteWriter` gathers what is written into a block that grows as needed, so code written against `Reader` and `Writer` can run without a file or a socket. Both seek within what they hold: a seek back in a `ByteWriter` overwrites in place, and a seek past the end is refused with `InvalidInput`. A `ByteReader` ends with `EndOfStream` like any other stream.

## Text and streams

`Display` writes into a `TextWriter` and a stream takes bytes, so something has to stand between them. `ConsoleWriter` is that adapter over standard output and `StreamWriter` is it over any `Writer` — a file, a buffered writer, a pipe — which is what lets a value describe itself anywhere other than standard output or memory:

```rux
import Io::{ IoError, StandardOut, StreamWriter, WriteValueLine, Writer };
import Text::TextWriter;

func Main() -> int {
    var out = StandardOut::Acquire();
    let measurement: int32 = 42;

    // The adapter turns a stream into a text writer. It cannot return an I/O error through the text protocol, so
    // it keeps the first one it saw in `failure` — which is why that is checked afterwards rather than at the call.
    var failure: IoError? = none;
    // The adapter keeps a `Writer` handle that addresses `out` rather than copying it.
    // Safety: `out` is a local that outlives `adapter` and stays where it is.
    var adapter = StreamWriter(unsafe { @out as Writer }, @failure);
    let text: &var TextWriter = adapter;

    // The type argument is required: the function is generic over the value so that it can borrow it rather than
    // take ownership, and a generic bound is not inferred from an interface value.
    let written = (try? WriteValueLine<int32>(text, measurement)) is .Some(_);
    if failure is IoError || !written {
        return 1;
    }
    return 0;
}
```

Neither error type can hold the other's failures, so the crossing is made explicitly in both directions. `FromIoError` turns what a stream threw into what a formatter understands, as in `try Write(...) catch { error => throw FromIoError(error) }`: every failure becomes `WriterFailure`, which is all `FormatError` can say about a destination. That is lossy on purpose, and the adapter makes the loss good by recording the original `IoError` in an optional slot the caller holds, `none` until something fails — the formatter learns the one thing it can act on, and the caller learns exactly what happened. Once a failure is recorded the stream is not touched again, so a value part-way through rendering stops rather than writing into a destination that has already refused.

`ToIoError` comes back the other way, and the distinction it preserves is the one that decides what to do next: a request that was wrong is `InvalidInput` and will be wrong again, while a destination that failed came back as an `IoError` in the first place. Bytes that were not text are `InvalidText`; a bounded destination that filled is `LimitExceeded`. Nothing is stuffed into `IoError`'s `raw`, which is `errno` or the Win32 code and is meant to be looked up; a caller who needs to know which formatting failure it was builds a writer and calls `Text::WriteValue` themselves.

`WriteValueLine` and `WriteFormatLine` write a value or a pattern followed by a newline, and write the newline **only if** what came before it went out whole. A line is a value and its terminator together, so a value that could not be written must not be followed by one. Every `PrintLine` overload follows the same rule. `WriteValueLine` takes its value by bound rather than as an interface view, so the call is resolved for the value's own type and nothing is dispatched at run time; `WriteValue` and the variadic `args: &Display...` of the format functions borrow theirs as `&Display` views, which a temporary may be lent to for the call.

Stream helpers borrow concrete implementations directly as `&var Reader` or `&var Writer` interface views, and a named stream passed straight into one of them borrows without any binding: `try WriteAll(file, contents)`. The borrow neither copies nor consumes the stream, and it ends at the call. `BufferedReader`, `BufferedWriter` and `StreamWriter` must keep their streams after construction, so they store a `Reader` or `Writer` interface value: a copyable handle that addresses the stream and owns nothing. A handle to a stream with state is made from its address, `unsafe { @file as Reader }`, whose caller promises that the stream outlives the adapter and is not moved while it holds the handle. The `BufferedReader` and `BufferedWriter` constructors throw `Other` when their buffer cannot be allocated, so they are called as `try BufferedReader(allocator, source, capacity)`. Both buffered types prohibit copying, move with `move`, and release their owned buffers through `~BufferedReader` and `~BufferedWriter`.

## Example

```rux
import Allocator::SystemAllocator;
import Io::{ PrintLine, ReadLine };
import Text::StringBuilder;

func Main() -> int {
    let allocator = SystemAllocator();
    var name = StringBuilder(allocator);
    PrintLine("What is your name?");
    try ReadLine(name) catch { _ => return 1 };
    let text = name.TakeString();
    PrintLine(text);
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/io>

## License

Licensed under the [MIT License](LICENSE.md).
