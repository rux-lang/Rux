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

## Value model

Unicode scalar values, `GeneralCategory`, and `GraphemeBreak` are structural `Copy` values. Text is borrowed through
`Slice<char32>` and results are written into caller-owned `MutableSlice<char32>` storage; the package allocates and
owns nothing. Length results remain raw writable pointers, so pass an address such as `@written`. It must be non-null
and valid for one `uint` throughout the call. The slices carry their bounds but not ownership, and their backing
storage must outlive the operation.

## Example

```rux
import Core::MutableSlice;
import Unicode::ToUpperFull;

var output: char32[3];
var written: uint = 0;
ToUpperFull(0xDFu32 as char32, MutableSlice::From<char32>(@output[0], 3), @written);
```

## Documentation

<https://rux-lang.dev/docs/api/unicode>

## License

Licensed under the [MIT License](LICENSE.md).
