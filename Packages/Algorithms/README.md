# Algorithms

Generic algorithms over slices and mutable slices.

Sorting, searching, and the transformations that pair with the slice types in [`Rux/Core`](../Core) and the containers in [`Rux/Collections`](../Collections).

## Installation

```sh
rux add Rux/Algorithms
```

## Status

| Module   | Requires on `T` | Provides                                                 |
| -------- | --------------- | -------------------------------------------------------- |
| `Modify` | — or `==`       | Reordering, bulk movement, partitioning, transform, fold |
| `Find`   | — or `==`       | Linear queries, by value or by predicate                 |
| `Search` | `<`             | Sortedness checks and binary search                      |
| `Sort`   | `<`             | Introsort, ascending and descending                      |
| `MinMax` | `<`             | Sequence extremes and clamping                           |

Modules are split by what they ask of the element type, so a type that defines only one of the two comparison operators can still use the modules that need it, and a missing operator is reported against one module rather than the whole package.

A **predicate-taking** function asks nothing of `T` at all — the caller's function supplies the test — so those work on any element type whatsoever.

## The pointer-and-length shape

Every entry point takes a pointer and a length rather than a view struct:

```rux
func Reverse<T>(data: *var T, length: uint)
```

This is not a stylistic choice. Inside a generic function the compiler resolves an element to its type parameter only when it is reached through a pointer parameter, so `data[i]` works where `values.data[i]` does not once a bare `T` value is also in play. The shape also accepts every container without coupling this package to any of them.

Type arguments are always explicit — Rux does not infer them for free functions.

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

| Function       | Mutates | Returns                       | Complexity | Requires on `T` |
| -------------- | ------- | ----------------------------- | ---------- | --------------- |
| `Swap`         | yes     | —                             | O(1)       | —               |
| `Reverse`      | yes     | —                             | O(n)       | —               |
| `Rotate`       | yes     | `false` if `middle > length`  | O(n)       | —               |
| `Fill`         | yes     | —                             | O(n)       | —               |
| `Copy`         | yes     | `false` on a null with length | O(n)       | —               |
| `CopyBackward` | yes     | `false` on a null with length | O(n)       | —               |
| `Unique`       | yes     | surviving length              | O(n)       | `==`            |
| `RemoveValue`  | yes     | surviving length              | O(n)       | `==`            |

`Unique` and `RemoveValue` compact survivors toward the front and return the new length. They do not resize the storage, and elements from the returned length onward are left unspecified rather than cleared — the same contract as their C++ counterparts.

`Copy` moves elements front to back and `CopyBackward` back to front, so overlapping ranges are correct when the destination is respectively at or below the source and at or above the source. Neither detects overlap for you.

### Predicates — requiring nothing of `T`

A predicate is an ordinary function value of type `func(T) -> bool`. Rux has no
closures, so it carries no captured state: anything the test needs beyond the
element has to be reachable from the function itself.

| Function         | Mutates | Returns                                         | Complexity |
| ---------------- | ------- | ----------------------------------------------- | ---------- |
| `TryFindIf`      | no      | `true` + the first accepted index               | O(n)       |
| `TryFindLastIf`  | no      | `true` + the last accepted index                | O(n)       |
| `CountIf`        | no      | how many elements are accepted                  | O(n)       |
| `All`            | no      | whether every element is accepted               | O(n)       |
| `Any`            | no      | whether any element is accepted                 | O(n)       |
| `None`           | no      | the exact negation of `Any`                     | O(n)       |
| `RemoveIf`       | yes     | surviving length, order preserved               | O(n)       |
| `Partition`      | yes     | the boundary between accepted and rejected      | O(n)       |
| `PartitionPoint` | no      | the boundary of an already-partitioned sequence | O(log n)   |
| `Transform`      | yes     | `false` on a null with length                   | O(n)       |
| `Fold`           | no      | the accumulator                                 | O(n)       |

An empty sequence satisfies `All` vacuously and `Any` never, so `All` and
`None` are both true only when there is nothing to test.

`Partition` does **not** preserve order within either group: it swaps ends
inward for one pass and no memory. `RemoveIf` does preserve it. A stable
partition would need an auxiliary buffer or a rotation per element and is not
offered.

`Transform` and `Fold` let the result type differ from the element type, so
`Fold` both sums a sequence and builds something else entirely out of one:

```rux
import Algorithms::{ CountIf, Fold, Partition };

func IsEven(value: int32) -> bool { return value % 2 == 0; }
func Add(total: int32, value: int32) -> int32 { return total + value; }

var values: int32[5] = [1, 2, 3, 4, 5];
let evens = CountIf<int32>(@values[0], 5, IsEven);
let sum = Fold<int32, int32>(@values[0], 5, 0, Add);
let boundary = Partition<int32>(@values[0], 5, IsEven);
```

### `Find` — linear queries, requiring `==`

| Function      | Returns                                  | Complexity |
| ------------- | ---------------------------------------- | ---------- |
| `Contains`    | whether any element matches              | O(n)       |
| `Count`       | how many elements match                  | O(n)       |
| `TryFind`     | `true` + the first matching index        | O(n)       |
| `TryFindLast` | `true` + the last matching index         | O(n)       |
| `Equal`       | whether two sequences match element-wise | O(n)       |
| `TryMismatch` | `true` + the first differing index       | O(n)       |

### `Search` — ordered queries, requiring `<`

| Function          | Returns                                         | Complexity |
| ----------------- | ----------------------------------------------- | ---------- |
| `IsSorted`        | whether the sequence is non-decreasing          | O(n)       |
| `IsSortedUntil`   | the first index breaking the order, else length | O(n)       |
| `LowerBound`      | the first index not less than the value         | O(log n)   |
| `UpperBound`      | the first index greater than the value          | O(log n)   |
| `TryBinarySearch` | `true` + the lowest equivalent index            | O(log n)   |
| `EqualRange`      | the `Range<uint>` of equivalent elements        | O(log n)   |

The four bounded searches require the sequence to be sorted by the same `<`; `IsSorted` checks exactly that precondition. An unsorted sequence yields an unspecified index but never an access outside it.

`LowerBound`, `UpperBound` and `IsSortedUntil` return a position in `0..=length`, so every result is meaningful and none of them doubles as a not-found marker. Where a query really can fail, the `Try*` form writes through an out-parameter and returns a `bool`, leaving the index untouched on a miss so a caller can seed it with a fallback. A null out-parameter reports failure rather than searching.

`EqualRange` returns an empty range when nothing matched, and its endpoints are then the single position where the value would be inserted — so the absent case still answers "where would this go?".

Equivalence in this module is derived as `!(a < b) && !(b < a)`, never from `==`. A type that orders but does not define equality works here in full.

### `Sort` — ordering, requiring `<`

| Function         | Mutates | Returns                     | Complexity            | Stability |
| ---------------- | ------- | --------------------------- | --------------------- | --------- |
| `Sort`           | yes     | —                           | O(n log n) worst case | unstable  |
| `SortDescending` | yes     | —                           | O(n log n) worst case | unstable  |
| `NthElement`     | yes     | `false` if `n >= length`    | O(n) expected         | unstable  |
| `PartialSort`    | yes     | `false` if `count > length` | O(n log count)        | unstable  |

`Sort` is an introsort: quicksort with a three-way partition, falling back to heapsort once its recursion budget is spent and to insertion sort below 16 elements. The three-way partition means an all-equal sequence costs a single linear pass rather than the quadratic behavior a two-way partition produces on it, and input drawn from a small set of distinct values stops being a worst case. The heapsort fallback is what makes O(n log n) a guarantee rather than an average.

It allocates nothing and recurses only into the smaller side of each partition, so stack depth stays at about log2(n) — on the order of 64 frames for any sequence that fits in memory — however badly the pivots are chosen.

Sorting is **unstable**: equivalent elements may be reordered. It is deterministic, though — there is no randomized pivot, so the same input always produces the same output.

`SortDescending` sorts ascending and then reverses. That costs one extra linear pass and avoids maintaining a mirrored copy of every helper, which matters more than the pass does; order among equivalent elements is unspecified in both directions anyway.

`NthElement` places the element that a full sort would put at `n` and partitions around it, without ordering either side. Reach for it when only one order statistic is wanted — a median, a percentile — since it is linear on average where a sort is not. `PartialSort` orders the `count` smallest into the front and leaves the rest unspecified, at O(n log count) rather than O(n log n). Both allocate nothing, and neither recurses.

### `MinMax` — extremes and clamping, requiring `<`

| Function         | Returns                                     | Complexity |
| ---------------- | ------------------------------------------- | ---------- |
| `TryMinIndex`    | `true` + the first smallest element's index | O(n)       |
| `TryMaxIndex`    | `true` + the first largest element's index  | O(n)       |
| `TryMinMaxIndex` | `true` + both, in one pass                  | O(n)       |
| `Clamp`          | the value restricted to `low..=high`        | O(1)       |

The extremes are reported as indices, not values, so the caller keeps the identity of the element it found — with a type ordered by one field, two equivalent elements are not interchangeable. All three report the _first_ extreme, so `TryMinMaxIndex` agrees exactly with calling the two single searches separately.

Scalar two-argument `Min` and `Max` are deliberately absent: [`Rux/Math`](../Math) already owns those names for floating-point values, and a same-named generic here would make a bare `Min(1.0, 2.0)` ambiguous.

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

The planned ordering and search modules will require `func <` in the same way. A missing operator is reported where the algorithm is instantiated, not where it is declared.

## Guarantees and limitations

- **No allocation.** Every function is O(1) auxiliary space and the package depends only on `Rux/Core`.
- **Unchecked indexing**, matching `Slice`, `Array` and `List`: a length larger than the storage is undefined behavior, not a reported error.
- **Borrowing, not owning.** A view must not outlive the storage it borrows, and a `List` view is invalidated by any growth.
- **No closures.** A predicate is a plain function value carrying no captured state; anything it needs beyond the element must be reachable from the function itself.
- **The ordering functions have no comparator overload yet.** `Sort`, `LowerBound` and the rest order by the `<` operator, so an alternative order needs the wrapper-struct pattern above. The predicate-taking functions are already available.
- `Algorithms::Copy` and [`Memory::Copy`](../Memory) share a name. Importing both is fine — they take different parameter types and overload — but the Algorithms one is element-wise and correct for any `T`, while the Memory one is a byte copy.

## License

Licensed under the [MIT License](LICENSE.md).
