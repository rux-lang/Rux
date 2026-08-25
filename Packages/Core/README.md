# Core

Core language intrinsics: the declarations the compiler itself supplies, and the primitive types every other package builds on.

This is the one package almost everything depends on. Importing it is what turns compiler-backed names into ordinary Rux declarations you can call.

## Installation

```sh
rux add Rux/Core
```

## What it provides

- **Compile-time context** — `#target` (os, arch, abi, endian, pointer width, data model, object format, triple, `HasFeature`), `#build`, `#compiler`, `#source`, and `#config`, together with the enums they are typed by: `OperatingSystem`, `Architecture`, `ApplicationBinaryInterface`, `Endianness`, `DataModel`, `ObjectFormat`, `BuildMode`, `OptimizationMode`, `OutputKind`, `TargetFeature`.
- **Diagnostics** — `Assert`, `DebugAssert`, and `Panic` report and terminate; `#Error` and `#Warn` reject or flag an unsupported configuration while compiling and produce no runtime code. A release build removes `DebugAssert` checks without evaluating their arguments. Neither `Assert` nor `Panic` unwinds, so no destructor runs on the way out — anything that must be released on a failing path belongs to a caller that saw the failure, which is what `Result<T, E>` is for.
- **Value outcomes** — `Option<T>` for an absence that needs no explanation, `Result<T, E>` for a failure that does, `Unit` for an operation with nothing to report, and `Ordering` for a three-way comparison. Asking which case an outcome holds borrows it (`IsSome`, `IsNone`, `IsSuccess`, `IsError`); reaching a payload consumes it (`ValueOr`, `TryIntoValue`, `TryIntoError`), so pass a named outcome as `<-outcome`. A `Try*` extraction deliberately retains a nullable raw output pointer and leaves it unwritten on failure.
- **Interfaces for generic code** — `Equatable` and `Comparable` borrow their `Self`-typed operands through non-null references, `Hashable` gives the 64-bit summary a table keys on, and `Iterator`/`Iterable` name the iteration protocol the compiler drives by shape. Resource types prohibit copying with a bodyless canonical `=` and release themselves with `~Type`; `Drop` remains only as a temporary compatibility marker for packages not yet migrated. Bounds such as `func Sort<T: Comparable>(...)` resolve `Self` to each type argument without dynamic dispatch.
- **Ranges** — `Range`, `RangeFrom`, `RangeTo`, `RangeInclusive`, `RangeToInclusive`, and `RangeFull`, one per combination of ends the syntax can leave out or close over. Every range is half-open unless its name says otherwise, so `0..n` covers exactly `n` values; the `Inclusive` pair closes the upper end, which is what a range reaching a type's maximum needs. They carry bounds and no methods: a range type resolves to the compiler's own built-in form, which is what drives `for` and indexing, so `start` and `end` are read directly.
- **Slices** — `Slice`, a read-only `{ *T, length }` view, and `MutableSlice`, the same view over `*var T`. Both are copyable views that retain raw stored addresses because references cannot be fields. Construct an empty mutable view with `MutableSlice<T>()`; `MutableSlice::New<T>()` temporarily forwards for downstream compatibility. `AsSlice` weakens a mutable view and `Sub` clamps rather than running past the end.
- **Integer arithmetic** — `AddChecked`, `SubChecked`, and `MulChecked` report whether the true answer was representable and write the machine's answer either way; `AddWrapping`, `SubWrapping`, and `MulWrapping` name the wrapping that `+`, `-`, and `*` already do; `AddSaturating`, `SubSaturating`, and `MulSaturating` clamp to whichever limit the true answer ran past. `MaximumOf` and `MinimumOf` report a type's limits. All are generic over the integer type and work at every width, signed and unsigned, deriving a type's zero, signedness, and limits from one operand rather than being written out per width. The operation comes first in each name because `CheckedAdd`, `CheckedSub`, and `CheckedMul` already name the compiler's `uint64` intrinsics, which are emitted inline and are what allocation-size arithmetic is written with.
- **Bit operations** — `CountOnes`, `CountZeros`, `LeadingZeros`, `TrailingZeros`, `LeadingOnes`, `TrailingOnes`, `RotateLeft`, `RotateRight`, `ReverseBits`, `ReverseBytes`, `BitAt`, `BitWidthOf`, `IsPowerOfTwo`, and `ShiftRightLogical`. All are generic over the integer type and right at every width, signed and unsigned: `>>` copies the sign bit down on a signed type, so everything that reads a value's bits as bits goes through `ShiftRightLogical`, which brings zeros in either way.
- **Byte order** — `ToLittleEndian`, `FromLittleEndian`, `ToBigEndian`, and `FromBigEndian` convert a value already in a register; `StoreLittleEndian`, `StoreBigEndian`, `LoadLittleEndian`, and `LoadBigEndian` move one through raw bytes at any alignment, which is what a file or wire format needs. `TargetIsLittleEndian` reports this target's own order, established from its storage rather than from a list of known targets.
- **Checked conversion** — `ConvertChecked` reports whether a primitive value survived the conversion and writes the machine's answer either way; `ConvertWrapping` names the wrapping `as` already does, and `ConvertSaturating` clamps to the destination's range. Every check is a round trip in the source type, plus one test for the pair a round trip cannot see — a value and its conversion that disagree in sign, which is how `-1` reaches `uint64`. `IsSignedType` reports whether a type has values below zero.
- **Float classification** — `IsNaN`, `IsZero`, `IsInfinite`, `IsFinite`, `IsNegativeZero`, `IsSignNegative`, `IsSignPositive`, and `SignOf`. All are arithmetic rather than bit inspection, so one implementation serves every float width including the software-lowered ones, whose storage carries padding no generic function could locate a sign bit inside of. The sign of a zero, which comparison cannot see, is read from the infinity it divides into.
- **Primitive associated constants** — `Bits`, `Bytes`, `Min`, `Max` and the floating-point set (`Lowest`, `MinPositive`, `Epsilon`, `Infinity`, `NaN`) for every integer, floating-point, boolean, and character width.

## Ownership and borrowing

Core methods borrow ordinary values with `&T` and mutable receivers with `&var T`; callers pass the value directly and the compiler creates the short-lived borrow. Named move-only values require `<-` when ownership crosses a binding, call, assignment, return, or consuming receiver. Copyable Core values use generated structural copying. Canonical constructors are type calls such as `Unit()` and `SemanticVersion(1, 2, 3)`; their infallible `New` spellings remain temporary forwarding wrappers.

Raw pointers remain where absence, storage, or address arithmetic is part of the contract: slice fields, nullable `TryInto*` outputs, unchecked checked-arithmetic/conversion destinations, byte-order buffers, and zeroization.

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

```rux
import Core::{ Assert, Option, SemanticVersion };

func Major(version: &SemanticVersion) -> uint {
    return version.major;
}

func Main() -> int {
    let version = SemanticVersion(1, 2, 3);
    let answer = Option::Some<int32>(42i32);
    Assert(Major(version) == 1u, "borrowed version");
    Assert((<-answer).ValueOr(0i32) == 42i32, "explicit outcome move");
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/rux>

## License

Licensed under the [MIT License](LICENSE.md).
