# Text

Strings and fundamental text manipulation.

## Installation

```sh
rux add Rux/Text
```

## What it provides

- **`StringView`** — a borrowed run of bytes checked once to be UTF-8, so everything downstream may assume text and be right. Byte-indexed; a cut off a character boundary is refused rather than rounded. Search, trim and split live here, since they answer questions about bytes that already exist and allocate nothing.
- **`String`** — immutable text with value semantics. Language copies allocate an independent block from the same allocator; `Clone` reports allocation failure when it must be handled explicitly.
- **`StringBuilder`** — where text is assembled. Keeps spare room and doubles it, holds well-formed UTF-8 at every moment, and hands its block over through `IntoString` without copying it. Copying is prohibited.
- **UTF-8** — validation, decoding and encoding by Table 3-7 of the Unicode standard, so overlong forms, surrogate halves and values past U+10FFFF are refused structurally.
- **`CString`** — move-only, NUL-checked interop with C: text holding a zero byte is refused rather than truncated on the way out, and bytes from C are bounded and validated on the way in.
- **Transforms** — `Concat`, `Repeat`, `Replace`, and the ASCII-only case conversions, each allocating exactly once.
- **The presentation contracts** — `Display` and `Debug` are what a value implements to describe itself, `TextWriter` is where the text goes, and `FormatSpec` is what a placeholder asked for. They live here rather than in [`Rux/Format`](../Format) so that a package can describe its own values without depending on the numeric conversion engine: `Time`, `Uuid` and `Path` all implement them and none of them needs to know how a float is rounded. `WriteAligned` applies a spec's width, fill and alignment, which is the one part every type applies the same way.
- **Text presents itself** — `String` and `StringView` implement `Display` as their own characters and `Debug` as those characters quoted and escaped. What is escaped is narrow on purpose: the quote and the backslash, which collide with the quoting, and the C0 and C1 controls, which act on a terminal instead of being read. All ordinary non-ASCII text is left alone, because escaping it would make most of the world's scripts unreadable in the one place a reader is trying to read them.
- **`BufferWriter`** — a fixed buffer that holds text at every moment. Every write is validated as UTF-8 before any of it lands, and a write that does not fit lands not at all, so what has been written is always the concatenation of the runs that succeeded. That second promise is what makes the buffer usable after a failure: a caller who runs out of room writes the same run somewhere larger without first working out how much of it went in.
- **`FormatSpec` and `ParseFormatSpec`** — what a placeholder asked for, parsed once by the grammar `[[fill]align][+][#][0][width][.precision][style]`. A style is an identifier, so `x` and `iso8601` parse by the same rule; the parser recognizes syntax and the value decides whether the style means anything, refusing what it does not support with `UnsupportedRequest`. A malformed specification reports the byte offset where parsing stopped.
- **Errors describe themselves** — `TextError`, `TextFailureKind`, `FormatError` and `Utf8Error` implement `Display` and `Debug`, and so do `AllocError` and `PageError`, which surface through `TextError` and whose owning packages do not depend on this one. `Display` is the stable description a reader is shown, short enough to sit inside a sentence a program is already writing; `Debug` names the case and its payload, so `FormatError::InvalidSpecification(12)` says where. The descriptions are stable and are not a parsing contract: a program branches on the case, not the words. `EntropyError` is deliberately absent — presenting it would need a dependency edge the layering forbids in both directions — and callers handle its cases or wrap it in a diagnostic type of their own.
- **Outcomes and runs show what is inside them** — `WriteOptionDebug`, `WriteResultDebug` and `WriteSliceDebug` write an `Option` as `Some(value)` or `None`, a `Result` as `Success(value)` or `Error(reason)`, and a run of elements as `[first, second, third]`. None of the three can be an interface implementation, because what an `Option<T>` looks like is entirely a question about `T` and an implementation has nowhere to say "when `T` can be shown"; they are handwritten functions carrying the bound, resolved at each instantiation with nothing dispatched at run time. They borrow, which is the point: `Option::ValueOr` consumes the option because taking a payload out is a transfer of ownership, while showing one is not, so an option holding a move-only value can be written as often as wanted and still holds it. The specification reaches the elements unchanged; the brackets, commas and case names are punctuation and are never padded. Sequential containers are [`Rux/Collections`](../Collections)' own writers, built on `WriteSliceDebug`.

- **`FormatError`** — why a value could not be turned into text, with no case for success. `TextError` next door keeps its `None` and its `IsOk` idiom for this package's storage and validation operations; `FromTextError` is the one sanctioned crossing between the two, and it turns success into `Result::Success` rather than into an error payload that means nothing went wrong.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Io::PrintLine;
import Text::{ String, TextError };

func Main() -> int {
    var system = SystemAllocator();
    let allocator: Allocator = system;
    var error = TextError::None;
    let greeting = String::FromBytes(allocator, "Hello", @error);
    if error != TextError::None {
        return 1;
    }
    PrintLine(greeting);
    return 0;
}
```

Use `String(allocator)`, `StringBuilder(allocator)`, and `StringView()` for empty values. Move builders and C strings explicitly with `<-`. Ordinary `String` copy syntax is infallible and terminates on allocation failure; use `Clone(@error)` in recoverable paths.

## Documentation

<https://rux-lang.dev/docs/api/text>

## License

Licensed under the [MIT License](LICENSE.md).
