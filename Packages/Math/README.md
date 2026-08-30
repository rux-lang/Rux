# Math

Portable mathematical constants, elementary functions, floating-point utilities, and generic integer operations for Rux. The package implements its elementary functions directly instead of forwarding them to the host system's `libm`. Argument reduction, polynomial kernels, and IEEE 754 edge-case handling therefore remain consistent across supported targets. The implementation allocates no memory and depends only on [`Rux/Core`](https://rux-lang.dev/docs/api/core).

## Installation

```sh
rux add Rux/Math
```

## Example

```rux
import Math::{ DegToRad, Hypot, SinCos };

let angle = DegToRad(30.0);
let (sine, cosine) = SinCos(angle);
let diagonal = Hypot(3.0, 4.0);
```

`SinCos` performs argument reduction once and returns `(sine, cosine)`, which is more efficient than calling `Sin` and `Cos` separately for the same angle.

## API overview

Most floating-point functions provide both `float64` and `float32` overloads. The `float32` forms use a wide intermediate where doing so improves accuracy, then round once to the requested width.

### Constants and angles

- **Exponential and logarithmic** — `E`, `Ln2`, `Log2E`, `Ln10`, and `Log10E`.
- **Circular** — `Pi`, `Tau`, `HalfPi`, `QuarterPi`, `InvPi`, and `InvTau`.
- **Radicals** — `Sqrt2` and `InvSqrt2`.
- **Angle conversion** — `RadPerDeg`, `DegPerRad`, `DegToRad`, and `RadToDeg`.

All constants are `float64` values rounded to the nearest representable value. See the [constants reference](https://rux-lang.dev/docs/api/math#constants) for the complete list.

### Elementary functions

| Category                 | Functions                                         |
| ------------------------ | ------------------------------------------------- |
| Trigonometric            | `Sin`, `Cos`, `Tan`, `Cotan`, `SinCos`            |
| Inverse trigonometric    | `ArcSin`, `ArcCos`, `ArcTan`, `ArcTan2`, `ArcCot` |
| Hyperbolic               | `Sinh`, `Cosh`, `Tanh`, `Cotanh`                  |
| Inverse hyperbolic       | `ArcSinh`, `ArcCosh`, `ArcTanh`                   |
| Exponential              | `Exp`, `Exp2`, `Expm1`                            |
| Logarithmic              | `Log`, `Log2`, `Log10`, `Log1p`                   |
| Powers and roots         | `Pow`, `PowInteger`, `Sqrt`, `Cbrt`, `Hypot`      |
| Magnitude and comparison | `Abs`, `Min`, `Max`                               |

`Expm1(x)` computes `Exp(x) - 1`, and `Log1p(x)` computes `Log(1 + x)`, without discarding the small result near zero. `PowInteger` uses repeated squaring and preserves exact integer powers when the intermediate values are exactly representable; use `Pow` for a general floating-point exponent.

### Rounding and conversion

- `Floor`, `Ceil`, `Trunc`, and `Round` provide the usual directed and ties-away rounding rules.
- `RoundTiesToEven` implements IEEE 754's default halfway rule.
- `RoundTo` selects a rule through `RoundingMode`.
- `ToIntegerChecked<T>` rounds and converts without silently saturating an out-of-range value.
- `Mod` computes the exact floating-point remainder with the sign of the dividend.

`ToIntegerChecked<T>` returns `true` on failure and writes zero to its output. The destination pointer must be non-null and writable:

```rux
import Math::{ RoundingMode, ToIntegerChecked };

var answer: int32 = 0;
let failed = ToIntegerChecked<int32>(42.5, 0, RoundingMode::ToNearestTiesEven, @answer);
```

### Floating-point inspection and manipulation

| Purpose           | API                                                                                              |
| ----------------- | ------------------------------------------------------------------------------------------------ |
| Classification    | `IsNan`, `IsInfinite`, `IsFinite`, `IsZero`, `IsSubnormal`, `IsNormal`, `Classify`, `FloatClass` |
| Sign handling     | `SignBit`, `CopySign`                                                                            |
| Adjacent values   | `NextAfter`, `NextUp`, `NextDown`                                                                |
| Special values    | `PositiveInfinity64`, `PositiveInfinity32`, `QuietNan64`, `QuietNan32`                           |
| Decomposition     | `ExponentOf`, `Decompose`, `SplitIntegral`                                                       |
| Scaling           | `ScaleByPowerOfTwo`                                                                              |
| Error measurement | `UlpDistance`                                                                                    |

`ExponentOf` reports `NoExponent` for zero and `IndeterminateExponent` for a NaN or infinity. `UlpDistance` counts representable steps rather than subtracting values, making it useful for numerical error checks across the full floating-point range.

### Generic integer operations

The integer APIs are generic over signed and unsigned integer widths:

- `AbsChecked<T>` and `AbsWrapping<T>` compute a magnitude with explicit overflow behavior.
- `Minimum<T>`, `Maximum<T>`, and `Clamp<T>` provide scalar ordering operations.
- `GcdChecked<T>` and `LcmChecked<T>` implement number-theory operations.
- `PowChecked<T>` raises an integer to a non-negative integer power.

Checked operations follow the same convention as checked arithmetic in `Rux/Core`: they return `true` when the mathematical result is not representable. Their raw output pointers must be non-null, correctly aligned, and writable for one value for the duration of the call.

## Numerical behavior

- **IEEE 754 special values are part of each contract.** Signed zeros, infinities, NaNs, subnormals, and domain errors are handled deliberately rather than left to host-library behavior.
- **Accuracy is documented per function.** The primary trigonometric, exponential, logarithmic, and root operations target an error of at most one ulp; inverse hyperbolic functions document a two-ulp bound.
- **Large-angle reduction retains precision.** Trigonometric functions reduce large finite inputs without first rounding them to a low-precision multiple of `Pi / 2`.
- **No allocation or hidden global state.** Every operation works on ordinary values and caller-provided outputs.

The bit-level implementation currently supports x86-64 and AArch64 targets.

## Value and ownership model

The package owns no storage. `FloatClass` and `RoundingMode` are structural `Copy` values, and multi-result operations such as `SinCos` return tuples. Ordinary inputs are passed by value. Raw pointers appear only in checked operations and decomposition helpers that write an additional result supplied by the caller.

## Documentation

See the complete [Math API reference](https://rux-lang.dev/docs/api/math).

## License

Licensed under the [MIT License](LICENSE.md).
