# Uuid

UUID representation, parsing, formatting and generation.

## Installation

```sh
rux add Rux/Uuid
```

## What it provides

- **A 128-bit `Copy` value** — stored as the sixteen bytes RFC 9562 orders them in, with borrowed `&Uuid` inspection and the version and variant readable from it without copying or exposing an address.
- **Text** — the canonical hyphenated form and the `urn:uuid:` form, written in lower case because RFC 9562 says output should be, and read in either case because input arrives as it arrives.
- **Refusals that say where** — `Parse` answers a `Result`, and every failure carries the zero-based byte offset into the text it was given: the character that is not a hexadecimal digit, the hyphen that is missing or misplaced, the misspelling in a `urn:uuid:` prefix, or the end of a text that stops short. Inside a URN the offsets are into the whole string, so a caller underlining the character does not have to know which form it was handed.
- **Version 4** — random, from `Rux/Entropy`, with the version and variant bits forced whatever the source bytes said.
- **Version 7** — time-ordered, so byte order is time order and a database index built on one does not fragment; with a caller-owned `MonotonicUuid()` generator for identifiers minted in the same millisecond.
- **Windows GUIDs** — an explicit, self-inverse byte-order conversion, because the two layouts disagree about the first three fields and a silent reinterpretation produces a valid-looking UUID with the wrong timestamp.

Formatting and comparison borrow UUID values directly. Generation retains raw scalar error-output pointers and entropy retains raw byte buffers; neither kind transfers ownership.

## Documentation

<https://rux-lang.dev/docs/api/uuid>

## License

Licensed under the [MIT License](LICENSE.md).
