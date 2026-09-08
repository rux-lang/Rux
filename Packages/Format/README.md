# Format

String conversion and formatting: turning values into text, and text back into values.

## Installation

```sh
rux add Rux/Format
```

## What it provides

- **Placeholder rendering** — `WriteFormat` fills each `{}` from the next argument into a destination the caller supplies, and `Render` is the allocating entry point that answers with a `String` of its own. Used by [`Rux/Io`](../Io) to implement `Print` and `PrintLine`.
- **A pattern that is wrong is refused, not printed** — the grammar is `{}`, `{:spec}`, `{{` and `}}`, and anything else containing a brace is a mistake. A specification that does not parse reports `InvalidSpecification` with its byte offset into the pattern; a placeholder count that does not match the arguments reports `ArgumentCountMismatch` with both numbers. Both are found before anything is written, so a wrong pattern produces no output rather than a prefix of one. A value's own rendering can still fail partway, and what it managed to write stays written.
- **Primitive conversion, both directions** — every implemented boolean, character, integer and floating width implements `Display` and `Debug`, and the parsers read text back. The two halves share the digit machinery, the base handling and the exactness argument, which is why they are one package: splitting them would duplicate all three and let the halves disagree about what a value's text is.
- **The presentation contracts live in [`Rux/Text`](../Text)** — `Display`, `Debug`, `TextWriter`, `FormatSpec` and `FormatError` are declared there, so a package can describe its own values without depending on this conversion engine. What stays here is the numeric spelling of a value: digits, sign, base prefix and zero padding.
- **The digit machinery** — `WriteInt`, `WriteUint`, `WriteFloat` and the decimal helpers, writing into a stack buffer through `ByteCursor` so no primitive's rendering allocates.
- **Parsing** — `ParseInt64` and `ParseFloat64`, plus the non-trapping `TryParseInt64` and `TryParseFloat64`, which report failure through `ParseError`.
- **`ReplacementCharacter`** — what is written in place of anything that is not a whole character.
- **Float support** — `IsFinite`, `IsInfinite`, `IsNan`, `Pow10`, and `ScaleByPow10`, so decimal conversion stays exact where it can be.
- **The wide widths** — `Float80`, `Float128`, `Float256` and `Float512`, held as their bits. The language has no arithmetic at these widths yet, so a value is made from bits or read from text, answers `IsNan`, `IsInfinite`, `IsFinite` and `IsNegative`, and renders through `Display` as the shortest decimal that reads back to exactly the same bits — found with exact big-integer arithmetic (`BigNat`), which is why these, unlike the narrow widths, take an allocator. `ParseFloat80` through `ParseFloat512` read decimal text to the nearest value with ties to even, and every rendering and reading is held to reference vectors the compiler's own exact float arithmetic produced independently.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Format::{ Render, WriteFormat };
import Text::{ BufferWriter, String, TextWriter };

func Main() -> int {
    // Into a destination the caller owns, allocating nothing.
    var storage: char8[64];
    var buffer = BufferWriter((@storage[0])[..64]);
    let writer: &var TextWriter = buffer;
    if WriteFormat(writer, "count={}", 42i64).IsError() { return 1; }

    // Or into a string of its own, which is the one place rendering allocates.
    var system = SystemAllocator();
    let allocator: Allocator = system;
    return match <-Render(allocator, "pi={:.2}", 3.14159f64) {
        .Success(_) => 0,
        .Error(_) => 1
    };
}
```

Both answer with a `Result`: `WriteFormat` with `Result<Unit, FormatError>` and `Render` with `Result<String, FormatError>`. There is no status to forget to read, and no string handed back beside an error that says it is not really there. `Render` releases its partial text when it fails, so a caller never receives half a rendering and the allocator gets its block back either way.

## Documentation

<https://rux-lang.dev/docs/api/format>

## License

Licensed under the [MIT License](LICENSE.md).
