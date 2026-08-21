# Path

Native operating-system strings and filesystem paths.

## Installation

```sh
rux add Rux/Path
```

## What it provides

- **`OsString` and `OsStringView`** — text as the operating system actually stores it: arbitrary bytes on the Unix
  systems, arbitrary UTF-16 code units on Windows, unpaired surrogates included. Lossless by construction: whatever
  name the system hands over survives the round trip, whether or not it is valid text. Conversion **from** UTF-8
  always succeeds; conversion **to** UTF-8 is fallible, because the system's names are under no obligation to be
  text.

More arrives with the rest of the phase: borrowed `Path` and owning `PathBuffer`, components, joining, and lexical
normalization.

## Documentation

<https://rux-lang.dev/docs/api/path>

## License

Licensed under the [MIT License](LICENSE.md).
