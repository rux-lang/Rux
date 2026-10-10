# Log

Structured logging: levels, typed fields, one line of text or JSON per record, and a process-wide log that every thread can write to.

## Installation

```sh
rux add Rux/Log
```

## What it provides

| Module   | Covers                                                                                                      |
| -------- | ----------------------------------------------------------------------------------------------------------- |
| `Level`  | `Level` from `Trace` to `Error` with `Name`, `Label` and `Parse`, and `CompiledLevel` from the `LogLevel` define |
| `Field`  | `Field` with `Text`, `Integer`, `Unsigned`, `Float` and `Boolean`, and its `Value`                              |
| `Record` | `LogFormat`: `Text` or `JsonLines`                                                                            |
| `Logger` | `Logger` over any `Io::Writer`, with a level, context fields, optional times, and `Trace` to `Error`            |
| `Global` | the process-wide log on standard error: `Trace` to `Error`, `Log`, `SetLevel`, `SetFormat`, `SetTimestamps`, `Enabled` |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Log::{ Field, Info, LogFormat, SetFormat, Warn };

func Serve(port: uint64) {
    SetFormat(LogFormat::JsonLines);
    Info("listening", [Field::Unsigned("port", port)]);
    Warn("no certificate configured");
}
```

## Design

A record has a level, a message, and fields whose values keep their types. As text it is one line: the time, the level, the message, then `key=value` for each field, with a value that holds spaces, quotes or `=` in quotes. As JSON lines it is one object per line, with `time`, `level` and `message` members followed by the fields, and a number that is not finite written as `null`. Line breaks and other control characters are escaped in both formats, so a record never spans lines. Times are RFC 3339 in UTC to the millisecond. They can be turned off for a destination that adds its own, such as a service manager's journal.

Each record is built whole and handed to its destination in a single write. The process-wide functions write to standard error. Their level, format and time settings are read and written atomically, and a lock is held only for the write of a finished record, so threads build records at the same time and their lines never interleave. A `Logger` writes to any `Io::Writer`, such as a file or a socket, and is used by one thread at a time. Logging never fails: a `Logger` keeps the first failure to write for `Failure` to report, and the process-wide log drops a record it cannot write.

The `LogLevel` build define sets the least severe level a build keeps: `trace`, `debug`, `info`, `warn` or `error`. A record below it is dropped before any of it is built, whatever level a logger is set to. So a release build defined with `LogLevel = "info"` does no tracing work, while the process-wide level, which starts at `Info`, and each logger's own level are changed at run time.

## Documentation

<https://rux-lang.dev/docs/api/log>

## License

Licensed under the [MIT License](LICENSE.md).
