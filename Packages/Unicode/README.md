# Unicode

Unicode character properties, case, normalization and segmentation, from tables generated out of the Unicode 17.0.0
Character Database.

## Installation

```sh
rux add Rux/Unicode
```

## What it provides

- **Properties** — the general category, canonical combining class, and the `White_Space`, `Alphabetic` and numeric
  properties, answered by binary search over compressed ranges.
- **Case** — simple per-character mappings, the unconditional full mappings, and locale-independent full case
  folding. The conditional mappings are deliberately absent: they depend on locale or context, and applying them
  silently is how `i` stops round-tripping in Turkish.
- **Normalization** — canonical and compatibility decomposition (Hangul arithmetically), canonical ordering, and
  composition: all four normal forms, over scalar-value slices.
- **Graphemes** — extended grapheme cluster boundaries by the UAX #29 rules, emoji sequences and regional-indicator
  pairs included.

The tables under `Src/Generated*.rux` are committed output of `Tools/UnicodeGen`, which records the SHA-256 of every
input it read; regenerate with `Bin/Tools/rux-unicode-gen` after changing the data under `Tools/UnicodeGen/Data`.

## Documentation

<https://rux-lang.dev/docs/api/unicode>

## License

Licensed under the [MIT License](LICENSE.md).
