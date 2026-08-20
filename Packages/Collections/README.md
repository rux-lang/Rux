# Collections

Generic data structures.

## Installation

```sh
rux add Rux/Collections
```

## What it provides

| Type              | Role                                          | Also known as |
| ----------------- | --------------------------------------------- | ------------- |
| `Array<T>`        | Fixed-length owned heap storage, allocator-injected |         |
| `Vector<T>`       | Growable contiguous sequence                  | the stack     |
| `Deque<T>`        | Double-ended queue over a circular buffer, allocator-injected | the queue |
| `HashMap<K, V>`   | Open-addressed associative table              |               |
| `HashSet<T>`      | Open-addressed set of distinct values         |               |

There is no `Stack` or `Queue` type. `Vector` already is the stack — `Push` and `TryPop` work at the end that costs nothing to reach — and `Deque` already is the queue, with `PushBack` and `TryPopFront`. Wrapper types would duplicate them to add no safety Rux can enforce. There is no `Dictionary` either: `HashMap` is the one associative name.

There are no linked lists. They cost a pointer chase per element and an allocation per node, and nothing in this repository wants that.

## Four ways to name a run of elements

`Array` is the one that owns heap storage whose length is decided at run time. The others are not interchangeable with it:

| Spelling          | Storage        | Length              | Owns it |
| ----------------- | -------------- | ------------------- | ------- |
| `T[N]`            | inline         | fixed at compile time | —     |
| `Slice<T>`        | borrowed       | run time            | no      |
| `MutableSlice<T>` | borrowed       | run time            | no      |
| `Array<T>`        | heap           | run time, then fixed | yes    |
| `Vector<T>`       | heap           | run time, growable  | yes     |

`AsSlice` and `AsMutableSlice` hand out a borrowed view of a container's elements without copying them, which is how a container reaches [`Rux/Algorithms`](../Algorithms). A `Vector` view covers its initialized elements only, never its spare capacity.

Only `Slice` supports `values[i]`, `values[1..3]` and `for x in`; those are compiler built-ins, not methods. Every container here offers `At` and `Set` instead, which are unchecked like slice indexing, plus `TryGet` and `TrySet`, which are not.

## Failure model

Two return types, and which one an operation uses says what can go wrong.

An operation that has to **allocate** returns a `CollectionError`:

```rux
enum CollectionError: uint8 {
    None = 0,
    OutOfMemory = 1,
    CapacityOverflow = 2,
    IndexOutOfRange = 3,
    Unsupported = 4
}
```

The five are not interchangeable. `OutOfMemory` is the only one worth retrying — `IsTransient` says so — because the
allocator having nothing to give is a state that changes. A capacity that cannot be represented will not start being
representable, an index the collection does not have will not appear, and a layout the allocator refuses will be
refused the same way next time. `FromAllocError` is what maps an allocator's own answer onto these.

An operation that **cannot** allocate reports an ordinary miss with a `bool`, through the `Try*` prefix and an out-parameter. So an exhausted allocator is never confused with an empty container, a missing key, or a rejected index — the distinction is in the type, not in the documentation.

```rux
import Collections::{ CollectionError, Vector };

var numbers = Vector::New<int32>();
if !(numbers.Push(1) == CollectionError::None) {
    return;
}

var last: int32 = 0;
if numbers.TryPop(@last) {
    // ...
}
numbers.Free();
```

**A failed operation changes nothing.** Capacity arithmetic is checked before anything is allocated, and a reallocated block lands in a local before it replaces the owned pointer — so a failure leaves the original allocation valid and owned, rather than leaking it and nulling the container.

Every count is checked for overflow. `count * sizeof(T)` is the most dangerous expression a container writes: a product that wraps asks for a small block and then writes far past it.

## Ownership

`Array<T>` owns its storage and its elements, so it implements `Drop` and is move-only. Handing one to a function
transfers it, and reading the source afterwards is rejected while compiling rather than discovered later. Destruction
destroys every element and then releases the block — in that order, because an element destroyed after its storage
was released would be read from freed memory.

The allocator is injected at construction and kept, so an array built from an arena disappears with the arena and one
built from a pool comes back to the pool.

Every constructor *copies* its elements into the storage, which means an array of a move-only element type cannot be
built this way: copying such an element would give two owners to a value that promises one. `Vector<T>` and its
`Push` is the shape that works, since each element is moved in exactly once.

A container destroys an element it does not know is droppable by reading it into a local and letting the local's life
end. That is the only mechanism the language offers, it costs nothing for an element type that owns nothing, and it
is what `Array::Drop` does.

## Elements that own something

`Vector::Push` moves its argument in rather than copying it, so a vector is how a sequence of owning elements is
built: each is moved in exactly once, and destroyed exactly once, when the vector is. `TryPop` moves one back out,
which is how an element leaves without being destroyed.

`Array`'s constructors copy, so an array of owning elements has to be built through a vector first.

Whether handing a value to a generic container consumes it depends on what its element type turns out to be, which
the container's own body cannot know — the answer is settled at each instantiation. The tests count destructions
rather than assuming them: pushing seven elements destroys none, destroying the vector destroys exactly those seven,
and truncating destroys exactly the ones it drops.

## Iterating

`ValueIterator` yields elements by copy and `ReferenceIterator` yields a pointer to each, and both are driven by the
compiler's `for` loop, which matches an iterator by shape rather than through an interface. Copy is what reading a
sequence of numbers wants; a pointer is how an element that owns something is read without being moved, since copying
one would give two owners to a value that promises one.

Both borrow. Neither owns what it walks, and neither may outlive the container it came from or survive that container
growing, rehashing or being freed.

A `for` loop over an iterator *value* walks a copy of it, so the iterator the caller holds is left where it was. A
loop is not a way to advance an iterator someone else is also reading.

There is no iterator yielding a writable pointer yet, though the shape is obvious and the use is real: a generic
iterator reporting `Option<*var T>` does not survive lowering today. Until it does, changing elements while walking
means asking the container for a `MutableSlice<T>` and indexing it, which is what `Rux/Algorithms` does throughout.

## Hashing

`HashMap` and `HashSet` are one kernel — Robin Hood open addressing over a power-of-two table — with the set storing
nothing alongside its keys. A fix to the probing or the deletion is a fix to both.

Every entry records how far it sits from the slot its hash names. An insertion that meets an entry closer to home
than itself takes the slot and carries that entry onward, so the distances along a chain never decrease and the worst
one stays near the average. That also makes a miss cheap: a search reaching a slot whose occupant is closer to home
than the search has travelled can stop, because the key would have taken that slot on the way past.

Removal shifts the following entries back one slot each, stopping at an empty slot or one already at home. That
leaves the table exactly as if the removed entry had never been inserted — **there are no tombstones**, so a table
that is churned rather than grown does not slowly fill with them and rehash for no reason. The tests churn five
hundred insert-remove pairs at a steady size and assert the capacity never moves.

### The hash is seeded, and the seed is a secret

Without one, an adversary who chooses keys can compute a few thousand that collide and turn every lookup into a
linear scan — a denial of service against anything keyed on input it did not write. The seed comes from
`Rux/Entropy`, and the ready-made hashers use `SipHash`, which is what makes guessing the seed the only way in.

`TableSeed::Random` draws one; each draw is a system call, so a program making many tables should draw once and pass
it to `WithSeed`. `TableSeed::Of` takes the words directly, for a test that needs the same table twice.

A table's iteration order is its seed's, so it differs between two tables in one program and between two runs of it.
Nothing may rely on that order.

### Reading and writing in one operation

There is no `Entry` type. An entry API is closure-shaped, and Rux has no closures — so the operation it exists for is
offered directly instead:

```rux
var count: *var int32 = null;
var inserted = false;
counts.GetOrInsert(word, 0, @count, @inserted);
*count = *count + 1;
```

One probe when the key is present, and the key named once. `Replace` is the other half: it stores a value and hands
back the one that was there, where `Insert` throws it away.

`RemoveEntry` hands back both halves, so a key that owns something leaves intact too — and it hands back the *stored*
key rather than the one looked up. The two compare equal, but a key type carrying anything the equality ignores makes
them different values, and the map's is the one it kept.

### Supplying the pair

The hash and equality are supplied as functions rather than taken from the key type, because Rux has no closures and
an interface value would be stored and dispatched through on every lookup. `Hashing` has ready-made pairs for the
usual key types. A caller keying on its own type writes them, and the one rule is that keys comparing equal must hash
the same.

## Complexity

| Operation                                                  | Time                            |
| ---------------------------------------------------------- | ------------------------------- |
| `Array` construction, `Clone`, `FromSlice`                 | O(n)                            |
| `Array` / `Vector` `At`, `Set`, `TryGet`, `TrySet`         | O(1)                            |
| `Vector::Push`, `TryPop`, `SwapRemoveAt`, `Truncate`       | O(1) amortized                  |
| `Vector::Insert`, `RemoveAt`                               | O(n)                            |
| `Deque` push, pop, peek, `At` at either end                | O(1) amortized                  |
| `Deque::MakeContiguous`, `AsSlice`                         | O(capacity)                     |
| `HashMap` / `HashSet` lookup, insert, remove               | O(1) expected, O(capacity) worst |
| `Reserve`, `ShrinkToFit`, `Clone`, one full traversal      | O(capacity)                     |

Growth doubles from a floor of four slots, so a run of additions costs an amortized constant. `Deque` and both tables round to powers of two, which is what lets a wrap be a mask rather than a division.

## Working with `Rux/Algorithms`

Algorithms takes a pointer and a length, so a container passes the fields of a view straight in. The compacting functions return a surviving length without resizing anything, and `Vector::Truncate` is what accepts it:

```rux
import Algorithms::Unique;

let view = numbers.AsMutableSlice();
let kept = Unique<int32>(view.data, view.length);
numbers.Truncate(kept);
```

## Guarantees and limitations

- **Manual ownership.** Rux has no destructors, no move semantics, and no borrow checking. `Free` exactly once for every allocation.
- **Assignment aliases.** Copying a container value copies the struct, so both name one allocation and freeing both is a double free. `Clone` is what makes an independent copy. Nothing enforces this.
- **`Clone` is shallow.** Element bytes are copied verbatim. An element that owns a resource ends up referenced by both copies, and releasing it stays the element owner's job. Removing an entry from a table is the only chance to reclaim what it owned.
- **Elements are relocated bytewise.** Growth moves them with a byte copy. An element holding a pointer into itself would not survive that.
- **Views borrow, never own.** A view must not outlive the container's allocation. Growth invalidates every pointer and view; so does `Free`, and so does `Deque::MakeContiguous`, which moves elements without reallocating.
- **Unchecked indexing** in `At` and `Set`, matching `Slice`: an index past the end is undefined behavior, not a reported error. `TryGet` and `TrySet` check.
- **This is not memory safety.** The package reports allocation failure and checks its arithmetic. Use-after-free, double-free, and dangling views remain possible and undetected, because Rux's ownership model cannot express otherwise.
- **Constructor out-parameters must not be null.** A `result` pointer is the whole point of the call, unlike the optional out-parameters elsewhere, and it is not checked.

## License

Licensed under the [MIT License](LICENSE.md).
