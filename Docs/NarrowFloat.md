# Narrow Float Conversion

This page is the settled contract for converting `float32` and `float64` to and from decimal text: which algorithm the first-party packages use, where it comes from, how large its workspace is and why that size is enough, what precision is supported, and how the results are checked against something that did not come from the same code. It exists because the phase D plan requires all five recorded before any of the conversion is written — an exactness claim made after the fact is a claim nobody can audit.

The wide widths are a separate contract. `Float80` through `Float512` convert through the allocating `BigNat` in [`Rux/Format`](../Packages/Format), take an allocator explicitly, and do not implement `Display` or `Debug`. Nothing on this page changes that.

Return to the [main README](../README.md) for the complete documentation index.

## The constraint that chooses the algorithm

Ordinary rendering allocates no working storage. That is a release constraint rather than a preference: `Display` is what a value implements to describe itself, an implementation is reached from a placeholder in the middle of writing something else, and a rendering that can fail for want of memory makes every `{}` a fallible operation for a reason that has nothing to do with the value. So a narrow float's conversion has to run in storage whose size is known when the code is compiled.

That rules out the table-driven shortest-representation algorithms in their published forms. Grisu3, Ryū and Schubfach are all faster than what is chosen here, and all three carry lookup tables of pre-computed powers — Ryū's is roughly ten kilobytes for `float64` — which would have to be generated, checked and carried as data. Grisu3 additionally has a fallback path for the inputs it cannot answer, and a fallback is exactly what the plan forbids.

## The algorithm

**Exact rational digit generation**, the method Steele and White described in *How to Print Floating-Point Numbers Accurately* (1990) and Burger and Dybvig refined in *Printing Floating-Point Numbers Quickly and Accurately* (1996), commonly called Dragon4.

The value is `f × 2^e` with `f` an integer below `2^53` and `e` an integer. The algorithm forms it as an exact ratio of two integers, `R / S`, together with two more integers `M+` and `M-` that measure the distance to the neighbouring representable values. Digits come out one at a time by multiplying the remainder by ten and dividing, and generation stops as soon as what has been emitted is closer to the value than to either neighbour — which is the definition of a shortest representation that reads back to the same bits. Fixed precision stops on a digit count instead and rounds the remainder to nearest with ties to even. Parsing runs the same machinery in the other direction: the decimal is formed as an exact ratio and the quotient is taken to three bits past the significand with a sticky bit, which is enough to round to nearest with ties to even without ever being wrong.

Every step is integer arithmetic on exact values. There is no approximation to correct and therefore no fallback path, no error bound to argue about, and nothing that behaves differently for inputs nobody tested.

## Provenance and license

The algorithm is published mathematics, described in the two papers above and in every subsequent treatment of the problem. It is not patented and is not encumbered.

The implementation in `Rux/Format` is written from the description rather than ported from any existing codebase. No third-party source is copied, adapted or translated, so no third-party license attaches to it and the repository's own MIT license covers the whole of it. The same is already true of the wide-width conversion through `BigNat`, which uses the same method with allocated storage.

This matters more than it looks. The reference implementations of the faster algorithms carry real licenses — Ryū is Apache 2.0 with an alternative Boost license, Schubfach's reference is Apache 2.0 — and adopting one of those would add a license obligation to a package that currently has none. Writing an unencumbered algorithm from its published description avoids the question rather than answering it.

## The workspace, and why its size is enough

The fixed workspace is `FixedNat`: an unsigned integer of exactly **twenty 64-bit limbs, 1,280 bits**, with an explicit length and no allocation anywhere in it. Every operation that could exceed the capacity reports that it did rather than truncating or wrapping.

The bound comes from the worst case of the four values the algorithm holds, for `float64`, which is the wider of the two narrow formats:

| Quantity                                       | Largest value                        | Bits    |
| ---------------------------------------------- | ------------------------------------ | ------- |
| `R` for a large value, `f × 2^(e+1)`           | `2^53 × 2^972` = `2^1025`            | 1,025   |
| `S` for a small value, `2^(1-e)` at `e = -1074` | `2^1075`                             | 1,075   |
| `R` after scaling a small value by `10^324`    | `2^54 × 10^324` ≈ `2^1131`           | 1,131   |
| `S` after scaling a large value by `10^309`    | `2 × 10^309` ≈ `2^1029`              | 1,029   |

The largest is 1,131 bits, from the smallest subnormal scaled up to where its first significant digit appears. Long division shifts a remainder left by at most one bit per digit and never past the divisor's width, so nothing during generation exceeds that figure by more than a limb. Twenty limbs gives 1,280 bits: **149 bits of headroom over the worst case**, which is more than two limbs.

`float32` needs 149 bits for the equivalent quantity and is served by the same workspace with room to spare. The size is not tuned per width, because one workspace that serves both is one thing to get right.

The bound is asserted rather than asserted-about: the workspace test drives a value to the capacity limit and checks that the next operation reports the overflow instead of producing a wrong answer, and the conversion tests include the smallest subnormal and the largest finite value of both widths, which are the inputs that reach the figures above.

## Supported precision

Fixed and scientific rendering support a precision of **0 through 1,100 digits inclusive**. A request past that reports `UnsupportedRequest`, with no fallback and no silent clamp.

The bound is chosen to be exactly enough rather than round. The smallest `float64` subnormal is `2^-1074`, whose exact decimal expansion has 1,074 fractional digits and no more; the largest finite `float64` has 309 integer digits. Every `float64` therefore has an exact decimal representation within 1,100 digits, and a precision past that could only ask for zeros. `float32` needs 149 fractional digits at most and is covered by the same bound.

Refusing rather than clamping is the point. A caller that asks for 5,000 digits has miscalculated something, and answering with 1,100 digits and 3,900 zeros would hide it.

## Reference vectors

An implementation checked only against itself proves nothing. Three independent sources are used, and a vector is only accepted when it comes from something that is not this code:

1. **The compiler's own exact float arithmetic**, which is a separate implementation in C++ and already produces the wide-width reference vectors this package is held to. It is the primary source for the narrow widths as well.
2. **Structural values with known exact expansions**, which need no third implementation to be right: powers of two, the smallest and largest subnormal, the smallest and largest normal, the two zeros, and the values adjacent to a rounding boundary. `2^-1074` has one correct decimal expansion and it is a matter of arithmetic rather than of trusting a tool.
3. **Round-trip closure**, which checks the pair rather than either half: for a set of values covering every exponent decade and both widths, rendering and reading back must return the identical bits. This catches a renderer and a parser that are wrong in the same direction, which the first two sources would not.

Ties, subnormals, adjacent values, long decimal inputs and the absence of allocation each keep their own vectors, as the plan requires, and those vectors stay in the test tree rather than being regenerated from the implementation.

## What this page does not promise

It describes the intended contract for tasks 22 through 25. Until all four are implemented and verified, `float32` and `float64` conversion is what it was: correct for ordinary values and not held to the exactness claims above. Nothing here should be read as a statement that the current implementation already meets them.
