# Math

Mathematical constants and functions, implemented in Rux rather than bound to a platform library.

The elementary functions use the argument-reduction and polynomial-kernel structure of a conventional libm, so results are consistent on every target rather than inheriting whatever the host system happens to ship.

## Installation

```sh
rux add Rux/Math
```

## What it provides

- **Constants** — `Pi`, `Tau`, `HalfPi`, `QuarterPi`, `InvPi`, `E`, `Ln2`, `Ln10`, `Log2E`, `Log10E`, `Sqrt2`, `InvSqrt2`, and the degree/radian conversion factors.
- **Trigonometric** — `Sin`, `Cos`, `Tan`, `Cotan`, and the inverses `ArcSin`, `ArcCos`, `ArcTan`, `ArcCot`.
- **Hyperbolic** — `Sinh`, `Cosh`, `Tanh`, `Cotanh`.
- **Exponential and logarithmic** — `Exp`, `Exp2`, `Expm1`, `Log`, `Log2`, `Log10`, `Pow`.
- **Roots and magnitudes** — `Sqrt`, `Cbrt`, `Hypot`, `Abs`.
- **Rounding** — `Ceil`, `Floor`, `Round`, `Trunc`, `Mod`.
- **Comparison and angles** — `Min`, `Max`, `DegToRad`, `RadToDeg`.

## Example

```rux
import Math::{ Pi, Sqrt, Sin };

let root = Sqrt(2.0);
let wave = Sin(Pi / 4.0);
```

## Documentation

<https://rux-lang.dev/docs/api/math>

## License

Licensed under the [MIT License](LICENSE.md).
