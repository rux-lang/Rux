# Core

Core declares primitive associated APIs, diagnostics, value protocols, and compile-time context.

Core is optional. Scalar types, arithmetic, arrays, inferred string literals, and range syntax work without it. Import a declaration to use its associated constants. Slice and range fields are compiler-owned. A replacement package can provide the same intrinsic declarations; the compiler assigns no privilege to the name or manifest identity of Core.

## Installation

```sh
rux add Rux/Core
```

## What it provides

- **Compile-time context** — `#target` (os, arch, abi, endian, pointer width, data model, object format, triple, `HasFeature`), `#build`, `#compiler`, `#source`, and `#config`, together with the enums they are typed by: `OperatingSystem`, `Architecture`, `ApplicationBinaryInterface`, `Endianness`, `DataModel`, `ObjectFormat`, `BuildMode`, `OptimizationMode`, `OutputKind`, `TargetFeature`.
- **Diagnostics** — `Assert`, `DebugAssert`, and `Panic` report and terminate; `#Error` and `#Warn` reject or flag an unsupported configuration while compiling and produce no runtime code. A release build removes `DebugAssert` checks without evaluating their arguments. Neither `Assert` nor `Panic` unwinds, so no destructor runs on the way out — anything that must be released on a failing path belongs to a caller that saw the failure, which is what `Result<T, E>` is for.
- **Value outcomes** — `Option<T>` for an absence that needs no explanation, `Result<T, E>` for a failure that does, `Unit` for an operation with nothing to report, and `Ordering` for a three-way comparison. Asking which case an outcome holds borrows it (`IsSome`, `IsNone`, `IsSuccess`, `IsError`); reaching a payload consumes it (`ValueOr`, `TryIntoValue`, `TryIntoError`), so pass a named outcome as `<-outcome`. A `Try*` extraction deliberately retains a nullable raw output pointer and leaves it unwritten on failure.
- **Interfaces for generic code** — `Equatable` and `Comparable` borrow their `Self`-typed operands through non-null references, `Hashable` gives the 64-bit summary a table keys on, and `Iterator`/`Iterable` name the iteration protocol the compiler drives by shape. Resource types prohibit copying with a bodyless canonical `=` and release themselves with `~Type`. Bounds such as `func Sort<T: Comparable>(...)` resolve `Self` to each type argument without dynamic dispatch.
- **Integer arithmetic** — `AddChecked`, `SubChecked`, and `MulChecked` report whether the true answer was representable and write the machine's answer either way; `AddWrapping`, `SubWrapping`, and `MulWrapping` name the wrapping that `+`, `-`, and `*` already do; `AddSaturating`, `SubSaturating`, and `MulSaturating` clamp to whichever limit the true answer ran past. `MaximumOf` and `MinimumOf` report a type's limits. All are generic over the integer type and work at every width, signed and unsigned, deriving a type's zero, signedness, and limits from one operand rather than being written out per width. The operation comes first in each name because `CheckedAdd`, `CheckedSub`, and `CheckedMul` already name the compiler's `uint64` intrinsics, which are emitted inline and are what allocation-size arithmetic is written with.
- **Bit operations** — `CountOnes`, `CountZeros`, `LeadingZeros`, `TrailingZeros`, `LeadingOnes`, `TrailingOnes`, `RotateLeft`, `RotateRight`, `ReverseBits`, `ReverseBytes`, `BitAt`, `BitWidthOf`, `IsPowerOfTwo`, and `ShiftRightLogical`. All are generic over the integer type and right at every width, signed and unsigned: `>>` copies the sign bit down on a signed type, so everything that reads a value's bits as bits goes through `ShiftRightLogical`, which brings zeros in either way.
- **Byte order** — `ToLittleEndian`, `FromLittleEndian`, `ToBigEndian`, and `FromBigEndian` convert a value already in a register; `StoreLittleEndian`, `StoreBigEndian`, `LoadLittleEndian`, and `LoadBigEndian` move one through raw bytes at any alignment, which is what a file or wire format needs. `TargetIsLittleEndian` reports this target's own order, established from its storage rather than from a list of known targets.
- **Checked conversion** — `ConvertChecked` reports whether a primitive value survived the conversion and writes the machine's answer either way; `ConvertWrapping` names the wrapping `as` already does, and `ConvertSaturating` clamps to the destination's range. Every check is a round trip in the source type, plus one test for the pair a round trip cannot see — a value and its conversion that disagree in sign, which is how `-1` reaches `uint64`. `IsSignedType` reports whether a type has values below zero.
- **Float classification** — `IsNaN`, `IsZero`, `IsInfinite`, `IsFinite`, `IsNegativeZero`, `IsSignNegative`, `IsSignPositive`, and `SignOf`. All are arithmetic rather than bit inspection, so one implementation serves every float width including the software-lowered ones, whose storage carries padding no generic function could locate a sign bit inside of. The sign of a zero, which comparison cannot see, is read from the infinity it divides into.
- **Primitive associated constants** — `Bits`, `Bytes`, `Min`, `Max` and the floating-point set (`Lowest`, `MinPositive`, `Epsilon`, `Infinity`, `NaN`) for the supported integer and character widths, width metadata for supported booleans, and the finite/special constant set for `float32` and `float64`. Reserved widths remain unimplemented. The string widths expose none: a string is a view over code units rather than a value with a width, so a width or a limit would describe neither it nor the units.

## Primitive declarations and imports

```rux
import Core::int8;

func Main() -> int {
    let minimum = int8::Min;
    let message: char8[..] = "hello";
    return message.length as int + minimum as int;
}
```

Without the `int8` import, `int8` still names the scalar type, but `int8::Min` is unavailable. Loading a dependency that imports it does not import its APIs into the caller. The same rule applies between source files.

Core defines integer and character limits, width metadata, and finite floating constants using ordinary constant expressions. Native integer metadata uses `sizeof`, so it follows the compilation target. Only floating-point `Infinity` and `NaN` use bodyless `intrinsic const` declarations inside extensions.

## Sequence views and ranges

The language supplies read-only `T[..]` and writable `var T[..]` views. Neither annotation needs a Core import. Their `.data` and `.length` fields are compiler-owned. A view owns no storage, and copying it copies two words. Its backing storage must remain alive while it is used.

```rux
func Sum(values: int[..]) -> int {
    var total = 0;
    for value in values { total += value; }
    return total;
}

func Fill(values: var int[..], value: int) {
    for index in 0..values.length { values[index] = value; }
}

func Main() -> int {
    var storage: int[3] = [1, 2, 3];
    let view = storage[..];
    Fill(view, 4);
    return Sum(view) - 12;
}
```

A writable view weakens to a read-only one implicitly; a read-only view cannot be strengthened. Binding a view with `let` does not freeze its pointee. `[]` constructs an empty view when a slice type is expected. Use `pointer[..length]` to view raw storage, and `view[start..end]` to select a subview. Pointer ranges require an end because pointers carry no length. These operations use element counts rather than byte counts.

Ranges are written `int..int`, `int..=int`, `int..`, `..int`, `..=int`, and `..` in annotations. Their `.start` and `.end` fields exist only when the corresponding bound exists, with no declaration or import required.

Literal text is `char8[..]`, `char16[..]`, or `char32[..]`; the prefixes are `c8`, `c16`, and `c32`, and an unprefixed literal uses UTF-8. The descriptor length counts code units, excluding the trailing NUL in literal storage. Text can be passed directly to a function taking the corresponding read-only character slice. Slicing can split an encoded scalar, so validated-text operations belong in Text and Unicode. A literal cannot become a writable view.

Core diagnostic messages and compile-time context text use `char8[..]`. External functions taking raw pointers receive `"literal".data`. Concrete slice extensions can add methods for one element type; reusable generic algorithms use ordinary generic functions.

## Ownership and borrowing

Core methods borrow ordinary values with `&T` and mutable receivers with `&var T`; callers pass the value directly and the compiler creates the short-lived borrow. Named move-only values require `<-` when ownership crosses a binding, call, assignment, return, or consuming receiver. Copyable Core values use generated structural copying. Canonical constructors are type calls such as `Unit()` and `SemanticVersion(1, 2, 3)`. Fallible or descriptive factories retain names such as `Option::Some` and `Result::Success`.

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
