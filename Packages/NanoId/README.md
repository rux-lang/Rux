# NanoId

Short, URL-safe, unpredictable identifiers in the Nano ID format.

## Installation

```sh
rux add Rux/NanoId
```

## What it provides

| Module     | Covers                                                                                         |
| ---------- | ---------------------------------------------------------------------------------------------- |
| `NanoId`   | the `NanoId` value: `Generate`, `Parse`, `Text`, `Equals`, `Compare`, `Display` and `Debug`     |
| `Generate` | `GenerateNanoId` and `GenerateNanoIdFrom`, which fill a buffer of any length from any alphabet  |
| `Alphabet` | `NanoIdAlphabet`, `NanoIdLength`, `NanoIdError` and `ValidateAlphabet`                          |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import NanoId::{ GenerateNanoId, NanoId };

func NewSessionToken() -> bool {
    let id = NanoId::Generate() catch { else => return false };
    var code: char8[8];
    GenerateNanoId(code[..], "0123456789") catch { else => return false };
    return id.Text().length == 21;
}
```

## Design

A standard identifier is twenty-one characters from the sixty-four URL-safe characters `A`–`Z`, `a`–`z`, `0`–`9`, `_` and `-`, which is 126 random bits: more than a version 4 UUID carries, in fewer characters, and safe unescaped in a URL or a file name. `NanoId` holds one inline as a `Copy` value. `GenerateNanoId` fills a buffer of any length instead, from the default alphabet or a custom one.

Every character of the alphabet is equally likely at every position. Each character is a random byte masked down to the smallest power of two that covers the alphabet, and a masked value past the end of the alphabet is thrown away rather than folded back with a remainder, which would favour the first characters. A custom alphabet must have between two and 256 distinct ASCII characters; `ValidateAlphabet` says which rule it broke, before any entropy is spent.

Random bytes come from the operating system through `Rux/Entropy`. Generation fails rather than producing a predictable identifier when the system has none, and leaves the destination zeroized rather than holding the start of an identifier. `GenerateNanoIdFrom` takes any `EntropySource`, which is how a test gets reproducible identifiers. The default alphabet is listed in the reference implementation's order, so the same random bytes give the same identifier here as in other implementations.

`Equals` compares every byte whatever it finds, so comparing a guessed identifier with a real one takes the same time however much of the guess was right.

## Documentation

<https://rux-lang.dev/docs/api/nanoid>

## License

Licensed under the [MIT License](LICENSE.md).
