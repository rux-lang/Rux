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
- **`FormatSpec` and `ParseFormatSpec`** — what a placeholder asked for, parsed once by the grammar `[[fill]align][+][#][0][width][.precision][style]`. A style is an identifier, so `x` and `iso8601` parse by the same rule; the parser recognizes syntax and the value decides whether the style means anything, refusing what it does not support with `UnsupportedRequest`. A malformed specification reports the byte offset where parsing stopped.
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
