# Format

String conversion and formatting: turning values into text, and text back into values.

## Installation

```sh
rux add Rux/Format
```

Placeholder readers preserve their positioned failures through native fallibles and `?`, including nested specification offsets. `Render` releases its builder on a propagated failure and transfers the allocation to the returned string on success. Literal pattern text is written as complete UTF-8 characters; malformed or truncated sequences report `TextFailure(InvalidUtf8)`.

## What it provides

- **Placeholder rendering** — `WriteFormat` fills each `{}` from the next argument into a destination the caller supplies, and `Render` is the allocating entry point that answers with a `String` of its own. Used by [`Rux/Io`](../Io) to implement `Print` and `PrintLine`.
- **A pattern that is wrong is refused, not printed** — the grammar is `{}`, `{:spec}`, `{{` and `}}`, and anything else containing a brace is a mistake. A specification that does not parse reports `InvalidSpecification` with its byte offset into the pattern; a placeholder count that does not match the arguments reports `ArgumentCountMismatch` with both numbers. Both are found before anything is written, so a wrong pattern produces no output rather than a prefix of one. A value's own rendering can still fail partway, and what it managed to write stays written.
- **Primitive conversion, both directions** — every boolean, character and integer width, and `float8` through `float64`, implements `Display` and `Debug`, and the parsers read text back. `float80` and wider are reached through the wide values below, because their text allocates. The two halves share the digit machinery, the base handling and the exactness argument, which is why they are one package: splitting them would duplicate all three and let the halves disagree about what a value's text is.
- **The presentation contracts live in [`Rux/Text`](../Text)** — `Display`, `Debug`, `TextWriter`, `FormatSpec` and `FormatError` are declared there, so a package can describe its own values without depending on this conversion engine. What stays here is the numeric spelling of a value: digits, sign, base prefix and zero padding.
- **The digit machinery** — `WriteInt`, `WriteUint`, `WriteFloat` and the decimal helpers, writing into a stack buffer through `ByteCursor` so no primitive's rendering allocates.
- **Borrowed primitive receivers** — primitive `WriteDisplay` and `WriteDebug` methods take `self: &T`. Their scalar reads preserve the existing output without transferring ownership or allocating; both direct calls and interface views borrow the caller's storage.
- **Narrow floats read back exactly** — `ParseFloat64` and `ParseFloat32` form the decimal as an exact ratio and take the significand's bits from it, rounding to nearest with ties to even, so what a decimal names is the float nearest it and never one step off. A `float32` is read at its own precision rather than at a `float64`'s and narrowed, which would round twice. Overflow is refused rather than rounded to an infinity; underflow is the signed zero it rounds to. With the renderer this closes the loop: every `float32` and `float64` renders to a decimal that reads back as the same bits, across the whole exponent range.
- **Parsing, and where it stopped** — every width has a `Parse*` answering `T ! ParseError`, and every `ParseError` says where in the text it was found: `InvalidCharacter` at the byte, `Overflow` at the digit that passed the widest magnitude or at the end of a number that does not fit the width asked for, and `Empty` at zero, which is an empty text's only position. `ParseError` implements `Text::ParseFailure`, so an embedding grammar adds its token's offset and the positions compose. There is no `bool`-and-output-slot overload: a `Result` cannot be ignored the way a returned `bool` could, and it carries why as well as whether.
- **An allocation failure is not an overflow** — the wide float parser builds exact big integers to round with, and a failure to get storage for one reports `OutOfMemory` rather than `Overflow`. The two want different responses: one input is too large for the format whatever the machine does, the other reads fine on a less busy machine.
- **Two answers for a value that is not a character** — `Display` writes `ReplacementCharacter`, U+FFFD, for a `char8` above 0x7F, a `char16` that is half of a surrogate pair, and a `char32` or any wider character cast from a number no character has. `Debug` writes the value itself instead — `'\x{80}'` for a stray byte, `'\u{d800}'` for a lone half — so two values that render alike for a reader are told apart for a program, and a `char128`, `char256` or `char512` holding a number past 2^64 keeps every digit of it in the escape. A `char64` or wider is checked at its own width before anything is narrowed, because `0x100000041` narrows into a perfectly good `A` and the wrong character is harder to notice than a refused one. Neither half is a validation: `Rux/Text`'s strict conversions still refuse what is not text.
- **Character slices at all three widths** — `char8[..]` is copied through, and `char16[..]` and `char32[..]` are transcoded to UTF-8 under the same replacement rule, decoding surrogate pairs and replacing a lone half. A width counts the characters a slice renders rather than the units it holds, and nothing is transcoded into a buffer first, so a slice has no length bound.
- **Narrow floats render the shortest decimal that reads back as the same bits** — exactly, by the method in [Docs/NarrowFloat.md](../../Docs/NarrowFloat.md): the value and the gaps to its two neighbours are held as exact integer ratios in a fixed workspace, digits come out one at a time, and generation stops the moment what has been written is nearer this value than either neighbour. So `0.1` prints as `0.1` and a third of one prints to the sixteen digits it needs rather than to a fixed fifteen. Nothing allocates and nothing is approximated, so there is no fallback path. `float16` and `float8` render through the same generator at their own precision, so the shortest digits are the ones that read back at that width rather than at a `float32`'s — `float16::Max`, 65504, prints as `65500.0`, since no other `float16` is nearer. `{}` switches to scientific notation from a decimal exponent of 5 for a `float16` and 3 for a `float8`, the digit counts that tell every value of the width apart, as 9 does for a `float32` and 17 for a `float64`.
- **A precision is exact, or refused** — `{:.N}` means N digits after the decimal point, rounded to nearest with ties to even, generated from the same exact ratio the shortest form is. A carry runs the whole length of the digits, so `{:.1}` of 9.99 is `10.0`; a value far below the last place is a tail of zeros; and `{:.20}` of `0.1` is the float's own expansion rather than a tenth's. The supported precision is 0 to 1,100 digits, which is exactly enough for every `float64`, and a request past it reports `UnsupportedRequest` — there is no fallback to a different rendering.
- **Scientific notation on request** — `{:e}` and `{:E}` write a `float8`, `float16`, `float32` or `float64` as one digit, a point, the remaining digits, the mark and a signed exponent of at least two digits: `{:e}` of `123456.0` is `1.23456e+05` and `{:E}` of `1e300` is `1.0E+300`, the same spelling `{}` falls back to for a value too large or too small for fixed notation. Without a precision the digits are the shortest that read back as the same value, with `.0` after a lone digit; with one, the precision counts the digits after the point, exact and rounded with ties to even, so `{:.3e}` of `1234.5678` is `1.235e+03` and `{:.0e}` of `9.5` is `1e+01`. A zero is `0.0e+00`. Any other style on a float reports `UnsupportedRequest`.
- **NaN and infinity are words under every placeholder** — `NaN`, `Inf` and `-Inf`, whatever precision or style was asked for, with the placeholder's sign, width, fill and alignment applied: `{:+.2}` of an infinity is `+Inf`. A NaN shows no sign.
- **A based integer keeps its sign** — `{:b}`, `{:o}`, `{:x}` and `{:X}` of a negative signed integer write a minus sign and the magnitude's digits, not two's-complement bits, so `{:b}` of `-5i8` is `-101` and the text does not depend on the type's width.
- **Float support** — `IsFinite`, `IsInfinite` and `IsNan` for `float8`, `float16`, `float32` and `float64`, plus `Pow10` and `ScaleByPow10`, so decimal conversion stays exact where it can be.
- **The wide widths** — `Float80`, `Float128`, `Float256` and `Float512`, held as their bits. A value is made from bits, read from text, or taken from the primitive of its width with `FromValue` — `Float128::FromValue(x)` for a `float128` `x` — and `Value()` hands the primitive back, so a computed value renders with `Float128::FromValue(x).FormatWith(allocator, writer, spec)` and a parsed one is computed with as `ParseFloat128(allocator, text)?.Value()`. Both directions move the bits and nothing else; a `Float80` keeps only the low eighty, so the six padding bytes of a `float80`'s storage are neither read nor written. Each answers `IsNan`, `IsInfinite`, `IsFinite` and `IsNegative`, and renders through `FormatWith` as the shortest decimal that reads back to exactly the same bits — found with exact big-integer arithmetic, which allocates. None of the four implements `Display` or `Debug`, and neither do the primitives they bridge: the protocols offer nowhere to pass an allocator, and implementing them anyway would mean a hidden allocation behind an ordinary `{}` and no way for a caller who has chosen an allocator to make this one follow. Where a rendering allocates, the caller says from where. `ParseFloat80` through `ParseFloat512` read decimal text to the nearest value with ties to even, and every rendering and reading is held to reference vectors the compiler's own exact float arithmetic produced independently.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Format::{ Render, WriteFormat };
import Text::{ BufferWriter, String, TextWriter };

func Main() -> int ! FormatError {
    // Into a destination the caller owns, allocating nothing.
    var storage: char8[64];
    var buffer = BufferWriter((@storage[0])[..64]);
    let writer: &var TextWriter = buffer;
    WriteFormat(writer, "count={}", 42i64)?;

    // Or into a string of its own, which is the one place rendering allocates.
    var system = SystemAllocator();
    let allocator: Allocator = system;
    return match Render(allocator, "pi={:.2}", 3.14159f64) {
        .Success(_) => 0,
        .Failure(_) => 1
    };
}
```

Both answer with a native fallible: `WriteFormat` with `! FormatError` and `Render` with `String ! FormatError`. There is no status to forget to read — a result nobody handles is diagnosed as discarded — and no string handed back beside an error that says it is not really there. `Render` releases its partial text when it fails, so a caller never receives half a rendering and the allocator gets its block back either way.

## Documentation

<https://rux-lang.dev/docs/api/format>

## License

Licensed under the [MIT License](LICENSE.md).
