# Uuid

UUID representation, parsing, formatting and generation.

## Installation

```sh
rux add Rux/Uuid
```

## What it provides

- **A 128-bit `Copy` value** — stored as the sixteen bytes RFC 9562 orders them in, with borrowed `&Uuid`
  inspection and the version and variant readable from it without copying or exposing an address.
- **Text** — the canonical hyphenated form and the `urn:uuid:` form, written in lower case because RFC 9562
  says output should be, and read in either case because input arrives as it arrives.
- **Version 4** — random, from `Rux/Entropy`, with the version and variant bits forced whatever the source
  bytes said.
- **Version 7** — time-ordered, so byte order is time order and a database index built on one does not
  fragment; with a caller-owned `MonotonicUuid()` generator for identifiers minted in the same millisecond.
- **Windows GUIDs** — an explicit, self-inverse byte-order conversion, because the two layouts disagree about
  the first three fields and a silent reinterpretation produces a valid-looking UUID with the wrong timestamp.

Formatting and comparison borrow UUID values directly. Parsing and generation retain raw scalar error-output pointers,
and entropy retains raw byte buffers; neither kind transfers ownership.

## Documentation

<https://rux-lang.dev/docs/api/uuid>

## License

Licensed under the [MIT License](LICENSE.md).
