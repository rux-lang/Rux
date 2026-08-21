# Format

String conversion and formatting: turning values into text, and text back into values.

## Installation

```sh
rux add Rux/Format
```

## What it provides

- **`Display` and `Debug`** — the two ways a value turns into text: what a reader sees, and what an inspector sees, with quoting where the two differ. Both write into a `Writer` rather than answering with a string, so printing a value allocates nothing. Every primitive implements both, along with `String` and `StringView`.
- **`Writer`** — where formatted text goes: a builder, a fixed buffer, or the console, without the value knowing which.
- **`Format`** — placeholder-based formatting, used by [`Rux/Io`](../Io) to implement `Print` and `PrintLine`.
- **The digit machinery** — `WriteInt`, `WriteUint`, `WriteFloat` and the decimal helpers, writing into a stack buffer through `ByteCursor` so no primitive's rendering allocates.
- **Parsing** — `ParseInt64` and `ParseFloat64`, plus the non-trapping `TryParseInt64` and `TryParseFloat64`, which report failure through `ParseError`.
- **`ReplacementCharacter`** — what is written in place of anything that is not a whole character.
- **Float support** — `IsFinite`, `IsInfinite`, `IsNan`, `Pow10`, and `ScaleByPow10`, so decimal conversion stays exact where it can be.

## Example

```rux
import Format::{ ToString, TryParseInt64 };

var text = ToString(42);
var parsed = TryParseInt64("42");
```

## Documentation

<https://rux-lang.dev/docs/api/format>

## License

Licensed under the [MIT License](LICENSE.md).
