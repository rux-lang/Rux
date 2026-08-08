# Text

Strings and fundamental text manipulation.

## Installation

```sh
rux add Rux/Text
```

## What it provides

- **`String`** — an owning, heap-allocated string. Construct with `New`, `From` (from a slice, or from a `*char8` and a length), and `Clone`; inspect with `Data`, `Length`, `IsEmpty`, and `At`; compare with `Equals` and `==`. Ownership is explicit: a `String` is released with `Free`.
- **`StringBuilder`** — incremental construction, so building a string from many pieces does not reallocate on every append.
- **`IsSpace`** — whitespace classification.

## Example

```rux
import Io::PrintLine;
import Text::String;

func Main() -> int32 {
    var greeting = String::From("Hello");
    PrintLine(greeting);
    greeting.Free();
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/text>

## License

Licensed under the [MIT License](LICENSE.md).
