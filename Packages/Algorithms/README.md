# Algorithms

Generic algorithms over slices and mutable slices.

> **Partially implemented.** `Modify` is usable. Ordering, searching, and selection are still placeholders — see [Status](#status).

## Installation

```sh
rux add Rux/Algorithms
```

## Status

| Module   | Status                                                          |
| -------- | --------------------------------------------------------------- |
| `Modify` | Available — reordering and bulk element movement                 |
| `Search` | Planned — linear and binary search                               |
| `Sort`   | Planned — introsort, and the sortedness predicates that pair with it |
| `MinMax` | Planned — sequence extremes and clamping                         |

## The pointer-and-length shape

Every entry point takes a pointer and a length rather than a view struct:

```rux
func Reverse<T>(data: *var T, length: uint)
```

This is not a stylistic choice. Inside a generic function the compiler resolves
an element to its type parameter only when it is reached through a pointer
parameter, so `data[i]` works where `values.data[i]` does not once a bare `T`
value is also in play. The shape also accepts every container without coupling
this package to any of them.

Type arguments are always explicit — Rux does not infer them for free
functions.

```rux
import Algorithms::{ Reverse, Unique };
import Collections::List;

var numbers = List::New<int32>();
numbers.Add(3);
numbers.Add(1);
numbers.Add(1);

let view = numbers.AsMutableSlice();
Reverse<int32>(view.data, view.length);

let kept = Unique<int32>(view.data, view.length);
numbers.Free();
```

A [`Rux/Core`](../Core) `MutableSlice` works the same way, and a plain fixed
array needs no view at all:

```rux
import Algorithms::Rotate;
import Core::MutableSlice;

var values: int32[5] = [1, 2, 3, 4, 5];
Rotate<int32>(@values[0], 5, 2);

let view = MutableSlice::From<int32>(@values[0], 5);
Rotate<int32>(view.data, view.length, 2);
```

## What it provides

| Function       | Mutates | Returns                        | Complexity | Requires on `T` |
| -------------- | ------- | ------------------------------ | ---------- | --------------- |
| `Swap`         | yes     | —                              | O(1)       | —               |
| `Reverse`      | yes     | —                              | O(n)       | —               |
| `Rotate`       | yes     | `false` if `middle > length`   | O(n)       | —               |
| `Fill`         | yes     | —                              | O(n)       | —               |
| `Copy`         | yes     | `false` on a null with length  | O(n)       | —               |
| `CopyBackward` | yes     | `false` on a null with length  | O(n)       | —               |
| `Unique`       | yes     | surviving length               | O(n)       | `==`            |
| `RemoveValue`  | yes     | surviving length               | O(n)       | `==`            |

`Unique` and `RemoveValue` compact survivors toward the front and return the
new length. They do not resize the storage, and elements from the returned
length onward are left unspecified rather than cleared — the same contract as
their C++ counterparts.

`Copy` moves elements front to back and `CopyBackward` back to front, so
overlapping ranges are correct when the destination is respectively at or below
and at or above the source. Neither detects overlap for you.

## Custom element types

Anything requiring `==` works on a struct that defines it:

```rux
struct Record {
    key: int32;
    origin: int32;
}

extend Record {
    func ==(self, other: Record) -> bool {
        return self.key == other.key && self.origin == other.origin;
    }
}
```

The planned ordering and search modules will require `func <` in the same way.
A missing operator is reported where the algorithm is instantiated, not where
it is declared.

## Guarantees and limitations

- **No allocation.** Every function is O(1) auxiliary space and the package
  depends only on `Rux/Core`.
- **Unchecked indexing**, matching `Slice`, `Array` and `List`: a length larger
  than the storage is undefined behavior, not a reported error.
- **Borrowing, not owning.** A view must not outlive the storage it borrows,
  and a `List` view is invalidated by any growth.
- **No predicate- or comparator-taking functions** — `FindIf`, `Partition`,
  `Transform` and `Fold` need a `func(T, T) -> bool` parameter, which the
  compiler cannot yet substitute. See `BACKLOG.md` at the repository root.
- `Algorithms::Copy` and [`Memory::Copy`](../Memory) share a name. Importing
  both is fine — they take different parameter types and overload — but the
  Algorithms one is element-wise and correct for any `T`, while the Memory one
  is a byte copy.

## License

Licensed under the [MIT License](LICENSE.md).
