# Format

String conversion and formatting: turning values into text, and text back into values.

## Installation

```sh
rux add Rux/Format
```

## What it provides

- **`Stringable` and `ToString`** — the conversion protocol every printable type implements, and the function that drives it. Every primitive is covered, along with the text types `String` and `Slice<char8>`, so a value of any of them can be passed as a format argument.
- **`Format`** — placeholder-based formatting, used by [`Rux/Io`](../Io) to implement `Print` and `PrintLine`.
- **Writers** — `WriteInt`, `WriteUint`, `WriteFloat`, `WriteFixed`, `WriteScientific`, `WriteDecimal`, `WriteBool`, and `WriteChar`, with a dedicated module per primitive width (`Int8` through `Int64`, `Uint8` through `Uint64`, `Float32`, `Float64`, `Bool8` through `Bool32`, `Char8` through `Char32`).
- **Parsing** — `ParseInt64` and `ParseFloat64`, plus the non-trapping `TryParseInt64` and `TryParseFloat64`, which report failure through `ParseError`.
- **UTF-8** — encoding and decoding, including the `Replacement` character for malformed input.
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
