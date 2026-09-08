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
- **Why a primitive's receiver is a raw pointer** — every primitive's `WriteDisplay` and `WriteDebug` takes `self: *T` rather than `self: &T`, which is the one place in the first-party packages where a raw pointer is not a documented unsafe boundary. It is the only form that compiles: an interface implementation may not take its receiver by value, and a borrowed scalar cannot be read back out into a value. See [Known Compiler Defects](../../Docs/CompilerDefects.md) for the narrowing. Nothing is stored, aliased or arithmetic is done on the pointer; it is dereferenced once, immediately.
- **Parsing, and where it stopped** — every width has a `Parse*` answering a `Result`, and every `ParseError` says where in the text it was found: `InvalidCharacter` at the byte, `Overflow` at the digit that passed the widest magnitude or at the end of a number that does not fit the width asked for, and `Empty` at zero, which is an empty text's only position. `ParseError` implements `Text::ParseFailure`, so an embedding grammar adds its token's offset and the positions compose. There is no `bool`-and-output-slot overload: a `Result` cannot be ignored the way a returned `bool` could, and it carries why as well as whether.
- **An allocation failure is not an overflow** — the wide float parser builds exact big integers to round with, and a failure to get storage for one reports `OutOfMemory` rather than `Overflow`. The two want different responses: one input is too large for the format whatever the machine does, the other reads fine on a less busy machine.
- **Two answers for a value that is not a character** — `Display` writes `ReplacementCharacter`, U+FFFD, for a `char8` above 0x7F, a `char16` that is half of a surrogate pair, and a `char32` or `char64` cast from a number no character has. `Debug` writes the value itself instead — `'\x{80}'` for a stray byte, `'\u{d800}'` for a lone half — so two values that render alike for a reader are told apart for a program. A `char64` is checked at its own width before anything is narrowed, because `0x100000041` narrows into a perfectly good `A` and the wrong character is harder to notice than a refused one. Neither half is a validation: `Rux/Text`'s strict conversions still refuse what is not text.
- **Character slices at all three widths** — `char8[..]` is copied through, and `char16[..]` and `char32[..]` are transcoded to UTF-8 under the same replacement rule, decoding surrogate pairs and replacing a lone half. A width counts the characters a slice renders rather than the units it holds, and nothing is transcoded into a buffer first, so a slice has no length bound.
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
