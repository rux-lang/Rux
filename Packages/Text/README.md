# Text

Strings and fundamental text manipulation.

## Installation

```sh
rux add Rux/Text
```

## What it provides

- **`StringView`** — a borrowed run of bytes checked once to be UTF-8, so everything downstream may assume text and be right. Byte-indexed; a cut off a character boundary is refused rather than rounded. Search, trim, split and the walk over lines live here, since they answer questions about bytes that already exist and allocate nothing.
- **Running out of memory terminates the program** — a string, a builder or a C string that cannot get its storage does not return, so `builder.Append(view);` needs no error handling and `TextError` holds only what is wrong with the bytes: `InvalidUtf8` and `NotABoundary`. Code that can recover uses `String::TryClone` and `StringBuilder::TryReserve`, which throw the allocator's own `AllocError`; once `TryReserve` has succeeded, appends that fit in the room it took allocate nothing.
- **`String`** — immutable, move-only text. `Clone` copies it into an independent block from the same allocator, and `TryClone` does the same but throws `AllocError` for a refused allocation instead of terminating.
- **`StringBuilder`** — where text is assembled. Keeps spare room and doubles it, holds well-formed UTF-8 at every moment, and hands its block over through `TakeString` without copying it. Copying is prohibited.
- **UTF-8** — validation, decoding and encoding by Table 3-7 of the Unicode standard, so overlong forms, surrogate halves and values past U+10FFFF are refused structurally. `Encode` returns a `uint` and `DecodeAt` a `DecodedScalar` (the scalar and its width), both throwing `Utf8Error`, and `Validate` returns nothing and throws `Utf8Failure`, which says where the bytes stopped being UTF-8 as well as why.
- **`CString`** — move-only, NUL-checked interop with C: text holding a zero byte is refused rather than truncated on the way out, and bytes from C are bounded and validated on the way in. `CString::FromRaw` is an `unsafe func`, called from an `unsafe` block: the caller promises that a non-null `raw` is readable up to its terminator or for `limit` bytes, whichever ends first.
- **Transforms** — `Concat`, `Join`, `Repeat`, `Replace`, the ASCII-only case conversions, and `ToUpper`, `ToLower` and `FoldCase` by Unicode's full, unconditional mappings from [`Rux/Unicode`](../Unicode).
- **The presentation contracts** — `Display` and `Debug` are what a value implements to describe itself, `TextWriter` is where the text goes, and `FormatSpec` is what a placeholder asked for. They live here rather than in [`Rux/Format`](../Format) so that a package can describe its own values without depending on the numeric conversion engine: `Time`, `Uuid` and `Path` all implement them and none of them needs to know how a float is rounded. `WriteAligned` applies a spec's width, fill and alignment, which is the one part every type applies the same way, to text already rendered; `WriteDisplayAligned` applies them around a value that writes its text in pieces, as dates and identifiers do.
- **Text presents itself** — `String` and `StringView` implement `Display` as their own characters and `Debug` as those characters quoted and escaped. What is escaped is narrow on purpose: the quote and the backslash, which collide with the quoting, and the C0 and C1 controls, which act on a terminal instead of being read. All ordinary non-ASCII text is left alone, because escaping it would make most of the world's scripts unreadable in the one place a reader is trying to read them.
- **`BufferWriter`** — a fixed buffer that holds text at every moment. Every write is validated as UTF-8 before any of it lands, and a write that does not fit lands not at all, so what has been written is always the concatenation of the runs that succeeded. That second promise is what makes the buffer usable after a failure: a caller who runs out of room writes the same run somewhere larger without first working out how much of it went in.
- **`FormatSpec` and `ParseFormatSpec`** — what a placeholder asked for, parsed once by the grammar `[[fill]align][+][#][0][width][.precision][style]`. A style is an identifier, so `x` and `iso8601` parse by the same rule; the parser recognizes syntax and the value decides whether the style means anything, refusing what it does not support with `UnsupportedRequest`. An integer understands the bases `b`, `o`, `x` and `X`, and renders a negative value in any of them as a minus sign and the magnitude rather than as two's-complement bits: `{:b}` of `-5` is `-101`. A float understands `e` and `E`, which ask for scientific notation. A malformed specification reports the byte offset where parsing stopped.
- **Errors describe themselves** — `TextError`, `TextFailureKind`, `FormatError` and `Utf8Error` implement `Display` and `Debug`, and so do `AllocError` and `PageError`, which surface through the `Try*` operations and whose owning packages do not depend on this one. `Display` is the stable description a reader is shown, short enough to sit inside a sentence a program is already writing; `Debug` names the case and its payload, so `FormatError::InvalidSpecification(12)` says where. The descriptions are stable and are not a parsing contract: a program branches on the case, not the words. `EntropyError` is deliberately absent — presenting it would need a dependency edge the layering forbids in both directions — and callers handle its cases or wrap it in a diagnostic type of their own.
- **Outcomes and runs show what is inside them** — `WriteOptionalDebug`, `WriteResultDebug` and `WriteSliceDebug` write an optional `T?` as `Some(value)` or `none`, a `Result<T, E>` as `Success(value)` or `Failure(reason)`, and a run of elements as `[first, second, third]`. None of the three can be an interface implementation, because native forms implement no interface and what a `T?` looks like is entirely a question about `T`; they are handwritten functions carrying the bound, resolved at each instantiation with nothing dispatched at run time. They borrow, which is the point: `??` and `?` consume the optional because taking a payload out is a transfer of ownership, while showing one is not, so an optional holding a move-only value can be written as often as wanted and still holds it. The specification reaches the elements unchanged; the brackets, commas and case names are punctuation and are never padded. Sequential containers are [`Rux/Collections`](../Collections)' own writers, built on `WriteSliceDebug`.

- **`FormatError`** — why a value could not be turned into text, with no case for success. `TextError` next door is the same shape for this package's validation operations, which throw `TextError`; `FromTextError` is the one sanctioned crossing between the two, used in a `catch` arm that throws the mapped error: `try builder.AppendBytes(bytes) catch { e => throw FromTextError(e) }`.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Io::PrintLine;
import Text::{ String, TextError };

func Main() throws TextError {
    var system = SystemAllocator();
    let allocator: Allocator = system;
    let greeting = try String::FromBytes(allocator, "Hello");
    PrintLine(greeting);
}
```

Use `String(allocator)`, `StringBuilder(allocator)`, and `StringView()` for empty values. `String` and `StringBuilder` keep one storage invariant that their `unsafe` blocks rely on: `length <= capacity`, and `data` is null exactly when `capacity` is zero and otherwise addresses a block of `capacity` bytes from the stored allocator whose first `length` bytes are initialized, well-formed UTF-8. The storage fields of `String`, `StringBuilder` and `CString` are private, so only this package's operations can change them: `Bytes`, `View` and `Length` read them, `FromBytes` and `AppendBytes` check bytes on the way in, and `FromValidated` and `AppendValidated` are the trusted paths for bytes the caller has already checked. A `String` owns its block, so it is move-only like builders and C strings: hand one over with `move`, and copy it with `Clone()`, or with `TryClone()`, which throws `AllocError` where a refused allocation must be handled. `Io::Print` and `PrintLine` borrow a `String`, so printing one neither copies nor consumes it.

## Documentation

<https://rux-lang.dev/docs/api/text>

## License

Licensed under the [MIT License](LICENSE.md).
