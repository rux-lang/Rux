# Algorithms

Generic algorithms over slices and mutable slices.

Sorting, searching, and the transformations that pair with the slice types in [`Rux/Core`](../Core) and the containers in [`Rux/Collections`](../Collections).

## Installation

```sh
rux add Rux/Algorithms
```

## Status

| Module   | Takes             | Requires on `T` | Provides                                                 |
| -------- | ----------------- | --------------- | -------------------------------------------------------- |
| `Find`   | `Slice<T>`        | — or `==`       | Linear queries, sequence comparison, by value or predicate |
| `MinMax` | `Slice<T>`        | — or `<`        | Sequence extremes and clamping                           |
| `Search` | `Slice<T>`        | `<`             | Sortedness checks, bounds and binary search              |
| `Modify` | `MutableSlice<T>` | — or `==`       | Reordering, bulk movement, removal and partitioning      |
| `Sort`   | `MutableSlice<T>` | `<`             | Introsort, selection, and partial sorting                |
| `Stable` | `MutableSlice<T>` | `<`             | Merge sort, with scratch the caller supplies             |

Everything takes a view: a `Slice<T>` where it only reads, and a `MutableSlice<T>` where it writes.

Modules are split by what they ask of the element type, so a type that defines only one of the two comparison operators can still use the modules that need it, and a missing operator is reported against one module rather than the whole package.

A **predicate-taking** function asks nothing of `T` at all — the caller's function supplies the test — so those work on any element type whatsoever.

## What a search answers with

A search that found something answers with the index it found, wrapped in an `Option`:

```rux
func IndexOf<T>(items: Slice<T>, value: T) -> Option<uint>
```

Not found is an ordinary outcome, not a failure, and a sentinel index is a bug waiting for the one sequence long
enough to reach it. An empty slice is not a special case anywhere: it contains nothing, matches no search, satisfies
every universal claim and no existential one.

A comparison answers with an `Ordering` rather than a `bool`, and takes one where it needs the caller to decide.
`Compare` is lexicographic — the first differing element decides, and the shorter of two otherwise-equal sequences is
the smaller — and `CompareBy` is the same for a type with no ordering of its own.

## What the ordered searches assume

Every search in `Search` assumes the slice is already sorted by the same order the search uses. That is the caller's
promise, and nothing checks it on the way in — checking would cost the linear scan a binary search exists to avoid.
A search over an unsorted slice does not fail; it answers, and the answer is meaningless. `IsSorted` is there for a
caller who wants to check once, where checking is worth it.

A bound is a position rather than an element. `LowerBound` is the first position the value could be inserted at
without breaking the order and `UpperBound` is the last, so between them lie exactly the equal elements — which is
what `EqualRange` returns in one pass. All three answer for a value that is not there at all: the range is empty, and
its start is where the value would go.

Equality in these searches is the ordering's, not the type's. Two elements neither of which is less than the other
are equal here, whatever `==` would say. When several compare equal, `BinarySearch` reports the first, so the answer
depends on the sequence rather than on how the search happened to divide it.

## Nothing here resizes anything

None of these own the storage they work on, so a removal cannot shorten it. `Unique`, `RemoveValue` and
`RemoveWhere` compact the survivors toward the front and report how many are left; the caller, who does own the
storage, is what truncates it.

What lies past that count is unspecified. It is neither cleared nor preserved — the elements there were moved from,
and reading them back gives whatever the compaction happened to leave. For a `Copy` element that is a stale
duplicate; for anything else it is a value with no owner.

`Partition` is unstable: it exchanges elements from both ends inward, which is what keeps it to one pass and no
scratch storage, and means the order within each group is not the order it started in. A caller that needs the
original order needs a stable partition, which needs somewhere to put things.

## Stability, and what it costs

`Sort` is faster and needs no storage at all, and for most sequences the difference between equal elements is not
observable. It becomes observable the moment a second key is involved: sort a table by one column, then by another,
and only a stable sort leaves the first ordering intact within each group of the second.

`StableSort` is a bottom-up merge sort, so it needs somewhere to merge into. It asks the caller for scratch as long
as the sequence — `RequiredScratch` says how much — and reports false, changing nothing, when given less. Asking
rather than allocating is deliberate: this package allocates nothing anywhere, which is what lets it be used where
allocation is not available, and a caller who already has a buffer should not have a second one taken behind their
back.

Its comparisons and moves are O(n log n) for every input. There is no bad case, unlike a quicksort; what it costs
over `Sort` is the storage and about a constant factor.

## What sorting promises

`Sort` is an introsort: quicksort with a three-way partition, falling back to heapsort once the recursion budget is
spent and to insertion sort on small partitions. Each part covers the others' weakness, and the combination has a
genuine O(n log n) worst case that no one of the three has alone.

Precisely: comparisons are O(n log n) for **every** input, with nothing probabilistic about it — the depth budget is
`2 * floor(log2 n)`, and a range still unsorted when it runs out goes to heapsort, which never degrades. Auxiliary
storage is O(1). Stack depth is O(log n) frames, about sixty-four for any addressable length, because the recursive
call always takes the smaller side of a partition while the loop handles the larger.

Sorting is **not stable**: equal elements may come out in a different order than they went in. It is deterministic —
no randomized pivots, so the same input always gives the same output and the same number of comparisons — but a
caller who needs equal elements to keep their order needs a stable sort, which needs somewhere to put things.

Type arguments are always explicit — Rux does not infer them for free functions.

```rux
import Algorithms::{ Reverse, Unique };
import Collections::{ CollectionError, Vector };

let source: Slice<int32> = [3, 1, 1];
var numbers = Vector::New<int32>();
if !(Vector::FromSlice<int32>(source, @numbers) == CollectionError::None) {
    return;
}

let view = numbers.AsMutableSlice();
Reverse<int32>(view.data, view.length);

// Unique compacts the survivors toward the front and returns how many are
// left, without resizing anything. Truncate is what tells the Vector.
let kept = Unique<int32>(view.data, view.length);
numbers.Truncate(kept);
numbers.Free();
```

A [`Rux/Core`](../Core) `MutableSlice` works the same way, and a plain fixed array needs no view at all:

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

A predicate is an ordinary function value of type `func(T) -> bool`. Rux has no closures, so it carries no captured state: anything the test needs beyond the element has to be reachable from the function itself.

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

An empty sequence satisfies `All` vacuously and `Any` never, so `All` and `None` are both true only when there is nothing to test.

`Partition` does **not** preserve order within either group: it swaps ends inward for one pass and no memory. `RemoveIf` does preserve it. A stable partition would need an auxiliary buffer or a rotation per element and is not offered.

`Transform` and `Fold` let the result type differ from the element type, so `Fold` both sums a sequence and builds something else entirely out of one:

```rux
import Algorithms::{ CountIf, Fold, Partition };

func IsEven(value: int32) -> bool { return value % 2 == 0; }
func Add(total: int32, value: int32) -> int32 { return total + value; }

var values: int32[5] = [1, 2, 3, 4, 5];
let evens = CountIf<int32>(@values[0], 5, IsEven);
let sum = Fold<int32, int32>(@values[0], 5, 0, Add);
let boundary = Partition<int32>(@values[0], 5, IsEven);
```

### Comparators — ordering by something other than `<`

Every ordering function has a `*By` counterpart taking an explicit comparator: `SortBy`, `SortDescendingBy`, `NthElementBy`, `PartialSortBy`, `IsSortedBy`, `IsSortedUntilBy`, `LowerBoundBy`, `UpperBoundBy`, `TryBinarySearchBy`, `EqualRangeBy`, `TryMinIndexBy`, `TryMaxIndexBy`, `TryMinMaxIndexBy` and `ClampBy`.

The comparator answers "does `a` come strictly before `b`" — the same question `<` answers — and must be a strict weak ordering. Equivalence is derived from it alone, so **a comparator is enough on its own**: the element type need not define any operator at all.

```rux
import Algorithms::{ SortBy, LowerBoundBy };

struct Task {
    priority: int32;
    id: int32;
}

func ByPriority(a: Task, b: Task) -> bool { return a.priority < b.priority; }
func ById(a: Task, b: Task) -> bool { return a.id < b.id; }

var tasks: Task[3] = [Task { priority: 3, id: 40 },
                      Task { priority: 1, id: 50 },
                      Task { priority: 2, id: 30 }];
let count: uint = 3;

SortBy<Task>(@tasks[0], count, ByPriority);
SortBy<Task>(@tasks[0], count, ById);
```

They are separate names rather than overloads of the operator forms. Rux resolves an untyped integer literal against a single candidate but not against an overload set, so adding an overload would have made `Sort<int32>(data, 8)` stop compiling for everyone already calling it.

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
    func ==(self: Record, other: Record) -> bool {
        return self.key == other.key && self.origin == other.origin;
    }
}
```

Both operands are values, so both are written the same way. A larger element can take its receiver by reference instead — `func ==(self: *Record, other: Record)` — but the other operand stays a value either way: only the receiver is addressed for you, so a `*Record` there would leave `a == b` with no operator to find.

The planned ordering and search modules will require `func <` in the same way. A missing operator is reported where the algorithm is instantiated, not where it is declared.

## Guarantees and limitations

- **No allocation.** Every function is O(1) auxiliary space and the package depends only on `Rux/Core`.
- **Unchecked indexing**, matching `Slice` and the `At`/`Set` pair on every [`Rux/Collections`](../Collections) container: a length larger than the storage is undefined behavior, not a reported error. Those containers also offer `TryGet`/`TrySet`, which check; nothing here does.
- **Borrowing, not owning.** A view must not outlive the storage it borrows, and a `Vector` view is invalidated by any growth.
- **No closures.** A predicate or comparator is a plain function value carrying no captured state; anything it needs beyond the elements must be reachable from the function itself.
- **No stable sort.** Every sort here is unstable. A stable sort needs an auxiliary buffer, which would end the package's zero-allocation property.
- `Algorithms::Copy` and [`Memory::Copy`](../Memory) share a name. Importing both is fine — they take different parameter types and overload — but the Algorithms one is element-wise and correct for any `T`, while the Memory one is a byte copy.

## License

Licensed under the [MIT License](LICENSE.md).
