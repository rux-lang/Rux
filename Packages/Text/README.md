# Text

Strings and fundamental text manipulation.

## Installation

```sh
rux add Rux/Text
```

## What it provides

- **`StringView`** — a borrowed run of bytes checked once to be UTF-8, so everything downstream may assume text and
  be right. Byte-indexed; a cut off a character boundary is refused rather than rounded. Search, trim and split live
  here, since they answer questions about bytes that already exist and allocate nothing.
- **`String`** — text that owns its bytes, carries the allocator it gives them back to, and releases itself when it
  dies. Immutable: every transform produces a fresh string and leaves the receiver whole.
- **`StringBuilder`** — where text is assembled. Keeps spare room and doubles it, holds well-formed UTF-8 at every
  moment, and hands its block over through `IntoString` without copying it.
- **UTF-8** — validation, decoding and encoding by Table 3-7 of the Unicode standard, so overlong forms, surrogate
  halves and values past U+10FFFF are refused structurally.
- **`CString`** — NUL-checked interop with C: text holding a zero byte is refused rather than truncated on the
  way out, and bytes from C are bounded and validated on the way in.
- **Transforms** — `Concat`, `Repeat`, `Replace`, and the ASCII-only case conversions, each allocating exactly once.

## Example

```rux
import Allocator::{ Allocator, SystemAllocator };
import Io::PrintLine;
import Text::{ String, TextError };

func Main() -> int {
    var system = SystemAllocator::New();
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

## Documentation

<https://rux-lang.dev/docs/api/text>

## License

Licensed under the [MIT License](LICENSE.md).
