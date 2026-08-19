# Core

Core language intrinsics: the declarations the compiler itself supplies, and the primitive types every other package builds on.

This is the one package almost everything depends on. Importing it is what turns compiler-backed names into ordinary Rux declarations you can call.

## Installation

```sh
rux add Rux/Core
```

## What it provides

- **Compile-time context** — `#target` (os, arch, abi, endian, pointer width, data model, object format, triple, `HasFeature`), `#build`, `#compiler`, `#source`, and `#config`, together with the enums they are typed by: `OperatingSystem`, `Architecture`, `ApplicationBinaryInterface`, `Endianness`, `DataModel`, `ObjectFormat`, `BuildMode`, `OptimizationMode`, `OutputKind`, `TargetFeature`.
- **Diagnostics directives** — `#Error` and `#Warn`, which reject an unsupported configuration while compiling rather than at run time.
- **Assertions and panics** — `Assert`, `DebugAssert`, and `Panic`. Release builds remove `DebugAssert` checks without evaluating their arguments.
- **Value outcomes** — `Option<T>` for an absence that needs no explanation, `Result<T, E>` for a failure that does, `Unit` for an operation with nothing to report, and `Ordering` for a three-way comparison. Asking which case an outcome holds borrows it (`IsSome`, `IsNone`, `IsSuccess`, `IsError`); reaching a payload consumes it (`ValueOr`, `TryIntoValue`, `TryIntoError`), so a move-only value can never end up with two owners. A `Try*` extraction refuses a null destination and leaves it unwritten, while still consuming the outcome.
- **Interfaces for generic code** — `Drop` marks a type move-only so its value has one owner at a time, and the compiler runs it once per value that was not moved away, `Equatable` and `Comparable` give equality and a total order through `Self`-typed operands, `Hashable` gives the 64-bit summary a table keys on, and `Iterator`/`Iterable` name the iteration protocol the compiler drives by shape. All are written as bounds: `func Sort<T: Comparable>(...)` resolves `Self` to the type argument at each instantiation, with no dynamic dispatch.
- **Ranges** — `Range`, `RangeFrom`, `RangeTo`, `RangeInclusive`, `RangeToInclusive`, and `RangeFull`.
- **Slices** — `Slice`, a read-only `{ *T, length }` view, and `MutableSlice`, the same view over `*var T`. Both borrow and never own; `MutableSlice::AsSlice` weakens one to the other, and `Sub` clamps rather than running past the end. `SemanticVersion` sits beside them.
- **Integer arithmetic** — `AddChecked`, `SubChecked`, and `MulChecked` report whether the true answer was representable and write the machine's answer either way; `AddWrapping`, `SubWrapping`, and `MulWrapping` name the wrapping that `+`, `-`, and `*` already do; `AddSaturating`, `SubSaturating`, and `MulSaturating` clamp to whichever limit the true answer ran past. `MaximumOf` and `MinimumOf` report a type's limits. All are generic over the integer type and work at every width, signed and unsigned, deriving a type's zero, signedness, and limits from one operand rather than being written out per width. The operation comes first in each name because `CheckedAdd`, `CheckedSub`, and `CheckedMul` already name the compiler's `uint64` intrinsics, which are emitted inline and are what allocation-size arithmetic is written with.
- **Bit operations** — `CountOnes`, `CountZeros`, `LeadingZeros`, `TrailingZeros`, `LeadingOnes`, `TrailingOnes`, `RotateLeft`, `RotateRight`, `ReverseBits`, `ReverseBytes`, `BitAt`, `BitWidthOf`, `IsPowerOfTwo`, and `ShiftRightLogical`. All are generic over the integer type and right at every width, signed and unsigned: `>>` copies the sign bit down on a signed type, so everything that reads a value's bits as bits goes through `ShiftRightLogical`, which brings zeros in either way.
- **Byte order** — `ToLittleEndian`, `FromLittleEndian`, `ToBigEndian`, and `FromBigEndian` convert a value already in a register; `StoreLittleEndian`, `StoreBigEndian`, `LoadLittleEndian`, and `LoadBigEndian` move one through raw bytes at any alignment, which is what a file or wire format needs. `TargetIsLittleEndian` reports this target's own order, established from its storage rather than from a list of known targets.
- **Primitive associated constants** — `Bits`, `Bytes`, `Min`, `Max` and the floating-point set (`Lowest`, `MinPositive`, `Epsilon`, `Infinity`, `NaN`) for every integer, floating-point, boolean, and character width.

`Assert` and `Panic` failures have the same UTF-8, LF-terminated layout on every supported target:

```text
Assertion failed: message
  at Function (path:line:column)
```

`Panic` uses `Panic: message` on the first line. The location uses a package-relative path when the source belongs to the package root, and qualified method or module names are retained.

## Example

```rux
import Core::{ #target, #Error };

when #target.os {
    .Windows, .Linux, .macOS => {},
    else => #Error("Unsupported operating system")
}
```

## Documentation

<https://rux-lang.dev/docs/api/rux>

## License

Licensed under the [MIT License](LICENSE.md).
