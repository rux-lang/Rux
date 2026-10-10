# Encoding

Hexadecimal, Base64, Base32, Base58 and percent-encoding of byte sequences.

## Installation

```sh
rux add Rux/Encoding
```

## What it provides

| Module    | Covers                                                                                                   |
| --------- | -------------------------------------------------------------------------------------------------------- |
| `Hex`     | `HexEncode`, `HexDecode`, their length functions, `HexCase`, and the `ViewHex` display adapter            |
| `Base64`  | `Base64Encode`, `Base64Decode`, their length functions, the four `Base64Variant`s, and `ViewBase64`       |
| `Base32`  | `Base32Encode`, `Base32Decode`, their length functions, and the RFC 4648 and Crockford `Base32Variant`s   |
| `Base58`  | `Base58Encode`, `Base58Decode` and `Base58EncodedMaxLength`, in the Bitcoin alphabet                      |
| `Percent` | `PercentEncode`, `PercentDecode`, their length functions, and the `Component`, `Path` and `Form` sets      |
| `Stream`  | `EncodingWriter` and `DecodingReader`, which encode and decode hexadecimal, Base64 and Base32 through `Io` streams |
| `Error`   | `DecodeError`, shared by every decoder                                                                   |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Encoding::{ Base64Decode, Base64DecodedLength, Base64Encode, Base64EncodedLength, Base64Variant };

func RoundTrip() -> bool {
    let message = "Many hands make light work.";
    var text: char8[64];
    let written = Base64Encode(message, text[..], Base64Variant::UrlUnpadded) catch { else => 0 };
    var bytes: byte[64];
    let read = Base64Decode(text[..written], bytes[..], Base64Variant::UrlUnpadded) catch { else => 0 };
    return read == message.length && written == Base64EncodedLength(message.length, Base64Variant::UrlUnpadded);
}
```

## Design

Every function writes into a destination the caller provides and returns how much it wrote, so nothing here allocates. Each scheme has a length function that says exactly how large that destination must be, except Base58, whose length depends on the value and which offers a bound instead. `ViewHex` and `ViewBase64` borrow bytes and render them under a format placeholder, which is the way to get an allocated string: `Render("{}", ViewHex(digest))`.

A destination that is too short is refused rather than filled with what fits, because a truncated encoding is a different value rather than a shorter one. Encoders report `FormatError::InsufficientCapacity`; decoders report `DecodeError::InsufficientCapacity` with the length they needed. Decoders check the whole text before writing a byte, so every refusal except Base58's capacity refusal leaves the destination exactly as it was.

Decoding is strict, so that each value has one encoded form and two systems cannot read the same text differently. Hexadecimal and Base32 accept either case and nothing else: no prefix, no separators, no whitespace. Base64 accepts only its variant's alphabet, padding only where the variant puts it, and a last character whose unused bits are zero. The exceptions are deliberate: Crockford's Base32 reads `I` and `L` as `1`, `O` as `0` and skips hyphens because its specification asks readers to, and percent-decoding passes through characters an encoder would have escaped because text arrives from many encoders.

Each `DecodeError` carries where it was found: the byte offset of the character that is wrong, or the end of the text when the length is.

## Streams

`EncodingWriter` encodes what is written to it onto another `Io::Writer`, and `DecodingReader` decodes text from another `Io::Reader`, for hexadecimal, Base64 and Base32 in every form a `StreamCodec` names. Both work a group at a time through the slice functions, so the text a stream writes is exactly the text encoding the whole input would give. A writer holds back the bytes of an incomplete group until `Finish` ends the text with its last group and padding. A reader decodes the text it has read in whole groups and fails with `InvalidInput` when it reaches text the decoder refuses. Base58 has no streaming form, since every one of its characters depends on the whole input.

## Timing

Keys, tokens and secrets travel through these encodings as often as anything else, so hexadecimal, Base64 and RFC 4648 Base32 encode and decode in time that depends only on the length. They map digits with masks built from arithmetic rather than branches or table lookups, which would otherwise make the timing and the cache lines touched depend on the secret. Base58 and Crockford's Base32 are meant for identifiers and do not make that promise.

## Documentation

<https://rux-lang.dev/docs/api/encoding>

## License

Licensed under the [MIT License](LICENSE.md).
