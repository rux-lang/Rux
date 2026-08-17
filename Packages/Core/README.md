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
- **`Result`** — the fallible-return enum.
- **Ranges** — `Range`, `RangeFrom`, `RangeTo`, `RangeInclusive`, `RangeToInclusive`, and `RangeFull`.
- **Slices** — `Slice`, a read-only `{ *T, length }` view, and `MutableSlice`, the same view over `*var T`. Both borrow and never own; `MutableSlice::AsSlice` weakens one to the other, and `Sub` clamps rather than running past the end. `SemanticVersion` sits beside them.
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
