# Collections

Generic data structures.

## Installation

```sh
rux add Rux/Collections
```

## What it provides

| Type            | Role                                      | Also known as |
| --------------- | ----------------------------------------- | ------------- |
| `Array<T>`      | Fixed-length owned heap storage           |               |
| `Vector<T>`     | Growable contiguous sequence              | the stack     |
| `Deque<T>`      | Double-ended queue over a circular buffer | the queue     |
| `HashMap<K, V>` | Open-addressed associative table          |               |
| `HashSet<T>`    | Open-addressed set of distinct values     |               |
| `TreeMap<K, V>` | Ordered red-black-tree map                |               |
| `TreeSet<T>`    | Ordered red-black-tree set                |               |
| `PriorityQueue<T>` | Max-heap that hands out the largest first | the heap   |
| `BitSet`        | Growable set of small integers, a bit each |              |
| `SmallVector<T>` | Vector holding 64 bytes of elements inline |             |
| `LruCache<K, V>` | Bounded map evicting the least recently used |           |

There is no `Stack` or `Queue` type. `Vector` already is the stack — `Push` and `Pop` work at the end that costs nothing to reach — and `Deque` already is the queue, with `PushBack` and `PopFront`. Wrapper types would duplicate them to add no safety Rux can enforce. There is no `Dictionary` either: `HashMap` is the one associative name.

There are no linked lists. They cost a pointer chase per element and an allocation per node, and nothing in this repository wants that.

## Four ways to name a run of elements

`Array` is the one that owns heap storage whose length is decided at run time. The others are not interchangeable with it:

| Spelling    | Storage  | Length                | Owns it |
| ----------- | -------- | --------------------- | ------- |
| `T[N]`      | inline   | fixed at compile time | —       |
| `T[..]`     | borrowed | run time              | no      |
| `var T[..]` | borrowed | run time              | no      |
| `Array<T>`  | heap     | run time, then fixed  | yes     |
| `Vector<T>` | heap     | run time, growable    | yes     |

`AsSlice` and `AsMutableSlice` hand out a borrowed view of a container's elements without copying them, which is how a container reaches [`Rux/Algorithms`](../Algorithms). A `Vector` view covers its initialized elements only, never its spare capacity.

Both read-only and writable slices support `values[i]` and `values[1..3]` directly. Containers use bounds-checked `Get`, `At`, `AtMutable`, `Set`, and `Replace` methods and provide iterator values for `for` loops. `Set` destroys the element it overwrites once the new one is stored, and `Replace` hands that element back instead, so an element that owns something is never leaked by either.

## Failure model

Running out of memory terminates the program. `Push`, `Insert`, `Reserve`, `Clone` and every other operation that allocates either gets its storage or does not return, so a caller writes `numbers.Push(1);` and nothing more, and a capacity whose byte size cannot be represented terminates the same way. Code that can recover calls a `Try*` operation instead, which throws the allocator's own `AllocError` and leaves the container exactly as it was:

| Container   | Recoverable forms                         |
| ----------- | ----------------------------------------- |
| `Vector<T>` | `TryPush`, `TryReserve`, `TryClone`       |
| `HashMap<K, V>` | `TryInsert`, `TryReserve`             |

A `Try*` operation throws `Unsupported` for a size no allocator could serve, and otherwise what the allocator refused with; `AllocError::IsTransient` says whether trying again could help.

What is left for a caller to handle is an index the collection does not have. `Set`, `Replace` and a vector's `Insert` throw `CollectionError`, whose one case is `IndexOutOfRange`, so a call to one is marked with `try`:

```rux
enum CollectionError: uint8 {
    IndexOutOfRange = 1
}
```

An operation that reports an ordinary miss does it with an optional — `Get`, `Remove`, `Pop`, and `PopFront` return `T?`, and the accessors that lend an element in place, `At`, `Find`, `First`, `Front` and their kin, return a reference `&T?` — and so do the iteration cursors `HashMap::NextEntry` and `HashSet::NextElement`, which advance a `&var uint` cursor. So a missing key is never confused with a rejected index — the distinction is in the type, not in the documentation.

```rux
import Allocator::{ Allocator, SystemAllocator };
import Collections::Vector;

let allocator = SystemAllocator();
var numbers = Vector<int32>(allocator);
numbers.Push(1);
try numbers.TryReserve(1024) catch { _ => return 1 };

if numbers.Pop() is .Some(last) {
    // ...
}
```

**A failed operation changes nothing.** Capacity arithmetic is checked before anything is allocated, and a reallocated block lands in a local before it replaces the owned pointer — so a refused `Try*` operation leaves the original allocation valid and owned, rather than leaking it and nulling the container.

Every count is checked for overflow. `count * sizeof(T)` is the most dangerous expression a container writes: a product that wraps asks for a small block and then writes far past it.

Containers talk to the allocator in elements rather than bytes, through three helpers in `ContainerStorage.rux` that compute the layout from `sizeof(T)` and `alignof(T)` once. Those helpers are package-private, and there is nothing to import: they are how a container is built, not something a caller composes with. What they promise is visible through any container — an element occupying no bytes still gets a run with a real address rather than the shared empty one, a `Try*` growth the allocator refuses leaves the container exactly as it was, and everything held is destroyed exactly once.

## Ownership

Every owning container prohibits copying. Hand one to a consuming function with `move`, as in `Consume(move values)`; the compiler rejects later reads of the source. Canonical destructors destroy initialized elements before releasing their storage. The same capability is derived through generic fields, so a map or consuming iterator containing an owning kernel is also move-only.

Canonical empty construction is `Vector<T>(allocator)`, `Deque<T>(allocator)`, `HashMap<K, V>(...)`, `HashSet<T>(...)`, `TreeMap<K, V>(...)`, and `TreeSet<T>(...)`. Descriptive factories such as `FromSlice`, `WithCapacity`, and `Filled` retain their names.

The allocator is injected at construction and kept, so an array built from an arena disappears with the arena and one built from a pool comes back to the pool.

Every constructor _copies_ its elements into the storage, which means an array of a move-only element type cannot be built this way: copying such an element would give two owners to a value that promises one. `Vector<T>` and its `Push` is the shape that works, since each element is moved in exactly once.

A container destroys an element it does not know is droppable by relocating it from raw backing storage into a local and letting the local's life end. This raw-storage boundary is deliberate: safe references cannot transfer ownership through borrowed storage. It costs nothing for an element type that owns nothing and is what `~Array` does.

## Elements that own something

`Vector::Push`, deque pushes, and map/set insertion move their arguments into raw backing storage. Pass a named move-only element explicitly, for example `values.Push(move value)`. Each element is destroyed exactly once, when it is removed without being returned or when its container dies. `Pop`, removals, and consuming iterators move elements back out.

`Array`'s constructors copy, so an array of owning elements has to be built through a vector first.

Whether handing a value to a generic container consumes it depends on what its element type turns out to be, which the container's own body cannot know — the answer is settled at each instantiation. The tests count destructions rather than assuming them: pushing seven elements destroys none, destroying the vector destroys exactly those seven, and truncating destroys exactly the ones it drops.

## Iterating

`ValueIterator` yields elements by copy, `ReferenceIterator` yields read-only pointers, and `MutableReferenceIterator` yields writable pointers. The compiler's `for` loop drives all three. Use pointer iteration for move-only elements: it leaves each element in its storage and copies no element.

All three borrow their storage. The storage must outlive the iterator and every pointer it yields. Array and Vector provide `References()` and `MutableReferences()`; the writable form borrows the container mutably when the cursor is created and visits only initialized elements.

`IntoIterator` is the owning alternative for vectors, maps, and sets. Invoke it on a named owner as `(move values).IntoIterator()`; whatever is not yielded is destroyed with the consuming iterator. Borrowed iterators retain raw storage addresses because references cannot be stored in fields or returned.

A `for` loop over an iterator _variable_ advances that variable, so a loop that stops early leaves it where the walk stopped and a later loop or `Next` call resumes there. Iterate over a fresh iterator, or over the container, to walk from the start without touching the caller's.

For example, `for element in numbers.MutableReferences() { *element = *element * 2; }` doubles the stored numbers.

Construct `MutableReferenceIterator<T>(items)` directly to walk a `var T[..]`. Repeated `Next()` calls return `(*var T)?` and keep returning `none` after exhaustion. These cursors store raw pointers: callers must coordinate aliases and avoid moving or destroying the owner, or growing, shrinking, or reallocating its storage while an iterator or yielded pointer is in use. Iteration itself does not allocate or destroy elements.

## Hashing

`HashMap` and `HashSet` are one kernel — Robin Hood open addressing over a power-of-two table — with the set storing nothing alongside its keys. A fix to the probing or the deletion is a fix to both. Each slot also keeps the top byte of its key's hash, so a lookup passing other keys compares one byte for each and calls the equality function only on a likely match: for text keys that saves reading each passed key's bytes from wherever they live, at one byte per slot.

Every entry records how far it sits from the slot its hash names. An insertion that meets an entry closer to home than itself takes the slot and carries that entry onward, so the distances along a chain never decrease and the worst one stays near the average. That also makes a miss cheap: a search reaching a slot whose occupant is closer to home than the search has travelled can stop, because the key would have taken that slot on the way past.

Removal shifts the following entries back one slot each, stopping at an empty slot or one already at home. That leaves the table exactly as if the removed entry had never been inserted — **there are no tombstones**, so a table that is churned rather than grown does not slowly fill with them and rehash for no reason. The tests churn five hundred insert-remove pairs at a steady size and assert the capacity never moves.

### The hash is seeded, and the seed is a secret

Without one, an adversary who chooses keys can compute a few thousand that collide and turn every lookup into a linear scan — a denial of service against anything keyed on input it did not write. The seed comes from `Rux/Entropy`, and the ready-made hashers use `SipHash`, which is what makes guessing the seed the only way in.

`TableSeed::Random` draws one; each draw is a system call, so a program making many tables should draw once and pass it to `WithSeed`. `TableSeed::Of` takes the words directly, for a test that needs the same table twice.

A table's iteration order is its seed's, so it differs between two tables in one program and between two runs of it. Nothing may rely on that order.

### Reading and writing in one operation

There is no `Entry` type. An entry API is closure-shaped, and Rux has no closures — so the operation it exists for is offered directly instead:

```rux
let count = counts.GetOrInsert(word, 0);
count = count + 1;
```

`GetOrInsert` lends the stored value as a `&var V`, so the write goes straight into the map. One probe when the key is present, and the key named once. `Replace` is the other half: it stores a value and hands back the one that was there, where `Insert` throws it away.

`RemoveEntry` hands back both halves, so a key that owns something leaves intact too — and it hands back the _stored_ key rather than the one looked up. The two compare equal, but a key type carrying anything the equality ignores makes them different values, and the map's is the one it kept.

### Supplying the pair

The hash and equality are supplied as functions rather than taken from the key type, because Rux has no closures and an interface value would be stored and dispatched through on every lookup. `Hashing` has ready-made pairs for the usual key types. A caller keying on its own type writes them, and the one rule is that keys comparing equal must hash the same.

## Ordered maps

`HashMap` is the one to reach for by default: constant expected time against a pointer chase per level. `TreeMap` earns its keep when order is part of the question. It walks smallest key first, answers `Floor` and `Ceiling` — the nearest key at or below, at or above — and hands back everything between two bounds through `Range`, none of which a hash table can do at all, since a hash scatters keys deliberately and moves them between slots as it grows.

The ordering is the caller's, given once as a function. `Comparing` has ready-made ones for the key types `Hashing` covers. The rules are the ordinary ones: consistent, the same every call, and agreeing with equality — two keys that compare `Equal` are one key, and the second to arrive replaces the first's value.

`Range(start, end)` is half-open, so `Range(a, b)` and `Range(b, c)` together give exactly `Range(a, c)` with nothing repeated and nothing missed. Starting a range costs a descent rather than a walk from the smallest key.

Each entry of a `TreeMap` is its own allocation, and removal relinks the successor rather than copying into the removed node, so a tree never moves a value it keeps. A reference from `Find` or `FindMutable` still borrows the whole map, as one from a `HashMap` does, because nothing in its type says which entry it reaches.

## Ordered sets

`TreeSet` is to `HashSet` what `TreeMap` is to `HashMap`: order, at the price of a pointer chase per level. It walks smallest first, answers `Floor` and `Ceiling`, and hands back what lies between two bounds through `Range`.

Its elements are the keys of a tree whose values are the unit `()`, which occupies no bytes — a set is a map with the values compiled away rather than a second copy of the same balancing.

An element must be copyable, exactly as a `HashSet`'s must: the comparison takes its two elements by value and every descent compares against elements the set holds, so an element that owned something would be destroyed by the first comparison it took part in. Owning values belong in a `TreeMap` under copyable keys.

`IntersectWith` and `Subtract` allocate nothing, and ask for the next element by key rather than by following a pointer: removal relinks the nodes around what was removed, including the very node a pointer walk would visit next. `UnionWith` and `SymmetricDifferenceWith` can grow, one node at a time, because a tree has no capacity to reserve in advance.

## Set algebra

The predicates — `IsSubsetOf`, `IsSupersetOf`, `IsDisjointFrom`, `Equals` — allocate nothing and cannot fail. So do `IntersectWith` and `Subtract`, which only ever make a set smaller.

`UnionWith` and `SymmetricDifferenceWith` can grow. Both take room for the worst case first, so the table grows at most once; reserving more than needed is the price, and `ShrinkToFit` gives it back.

`IntersectWith` and `Subtract` sweep the slots until a whole sweep removes nothing. Removing shifts the entries that follow a slot back into it, and a chain running past the end of the table shifts them past the end too, so no single sweep in either direction can be sure it looked at everything — and which entries move at all depends on where the table's random seed happened to put them, which is what makes getting this wrong show up in one run out of several rather than every time. That is the one place the absence of tombstones costs something.

## Complexity

| Operation                                             | Time                             |
| ----------------------------------------------------- | -------------------------------- |
| `Array` construction, `Clone`, `FromSlice`            | O(n)                             |
| `Array` / `Vector` `At`, `Set`, `TryGet`, `TrySet`    | O(1)                             |
| `Vector::Push`, `Pop`, `SwapRemoveAt`, `Truncate`     | O(1) amortized                   |
| `Vector::Insert`, `RemoveAt`                          | O(n)                             |
| `Deque` push, pop, peek, `At` at either end           | O(1) amortized                   |
| `Deque::MakeContiguous`, `AsSlice`                    | O(capacity)                      |
| `HashMap` / `HashSet` lookup, insert, remove          | O(1) expected, O(capacity) worst |
| `Reserve`, `ShrinkToFit`, `Clone`, one full traversal | O(capacity)                      |

Growth doubles from a floor of four slots, so a run of additions costs an amortized constant. `Deque` and both tables round to powers of two, which is what lets a wrap be a mask rather than a division.

## Working with `Rux/Algorithms`

Algorithms takes a pointer and a length, so a container passes the fields of a view straight in. The compacting functions return a surviving length without resizing anything, and `Vector::Truncate` is what accepts it:

```rux
import Algorithms::Unique;

let view = numbers.AsMutableSlice();
let kept = Unique<int32>(view);
numbers.Truncate(kept);
```

## Showing what a container holds

`WriteVectorDebug`, `WriteArrayDebug` and `WriteDequeDebug` write a sequential container as `[first, second, third]` into any [`Rux/Text`](../Text) `TextWriter`, given an element type that implements `Debug`:

```rux
import Allocator::{ Allocator, SystemAllocator };
import Collections::{ Vector, WriteVectorDebug };
import Io::{ ConsoleWriter, IoError };
import Text::{ FormatError, FormatSpec, TextWriter };

// `Rux/Format` is imported for its `Debug` implementations, not for a name: `WriteVectorDebug` is bound on
// `T: Debug`, and the implementation for `int32` lives there. Without this the bound is unsatisfied.
import Format::*;

func Show() throws FormatError {
    let allocator = SystemAllocator();
    var numbers = Vector<int32>(allocator);
    numbers.Push(1);
    numbers.Push(2);

    var failure: IoError? = none;
    var console = ConsoleWriter(@failure);
    let sink: &var TextWriter = console;
    try WriteVectorDebug<int32>(sink, numbers, FormatSpec::Plain());
}
```

They are functions carrying the bound rather than `Debug` implementations, because what a `Vector<T>` looks like is entirely a question about `T` and an implementation has nowhere to say "when the element can be shown". Each instantiation resolves the element's `WriteDebug` at compile time.

Everything is borrowed. A container owns its elements and hands out no copy of a move-only one, so a diagnostic that took an element out to print it would be taking ownership of it; these show every element through a borrow, and a vector of move-only elements can be written as often as wanted and still holds all of them. Capacity is not shown, since the spare room past the length holds nothing. A deque is written front to back whether or not its ring has wrapped, and nothing is rearranged to do it — `MakeContiguous` is a caller's decision, not a side effect of printing. The specification reaches the elements unchanged, so `{:x}` over a vector of integers writes each one in hexadecimal.

`WriteHashMapDebug`, `WriteHashSetDebug`, `WriteTreeMapDebug` and `WriteTreeSetDebug` write a map as `{key: value, key: value}` and a set as `{element, element}` — braces rather than brackets, because what they hold is not a run of things. An ordered map or set is written in key order, so its rendering is a fact about its contents: two maps holding the same entries agree, and one map renders the same way every time. A hash map or set is written in the order of its table, which follows from the seed drawn when it was made and from the displacement every insertion caused, so it is not the insertion order, it does not hold between two maps with the same entries, and it changes between runs. Nothing sorts, since that would need an ordering on the key a hash container does not have and was built not to need; `TreeMap` is the container whose rendering is stable.

`CollectionError` implements `Display` and `Debug` itself. `Display` is the stable description a reader is shown — `index the collection does not have`. `Debug` names the case: `CollectionError::IndexOutOfRange`. Neither accepts a style or a precision, because a description has one form and truncating it produces a sentence that says something else.

## Guarantees and limitations

- **Owners are move-only.** Their canonical destructor releases elements and storage, which makes them move-only. `Clone` is explicit and copies elements byte for byte, so it is available only for a copyable element type, plain data that `Core::Copyable` describes; `Vector<String>::Clone` is rejected at the element copy where the container's code makes it.
- **Elements are lent by reference.** `At`, `AtMutable`, `First`, `Last`, `Front`, `Back`, `Peek`, `Find` and `FindMutable` return `&T?` or `&var T?`, which keeps the container borrowed while the reference is in use: a shared one blocks growth, removal and every write, a mutable one every other use, so no element moves under a reference to it.
- **Raw storage remains deliberate.** Allocator blocks, tree links, stored iterator addresses, and scalar output slots cannot be references. They never transfer ownership merely by being passed.
- **Views borrow, never own.** Growth, rehashing, destruction, and `Deque::MakeContiguous` invalidate relevant views and raw element pointers. The compiler tracks reference loans, but deliberately raw addresses remain the caller's responsibility.
- **Relocation uses the raw-storage compatibility boundary.** Explicit moves are used for named owners and elements; internal relocation through allocator memory remains an intentional raw operation until ownership-aware raw places are expressible.
- **Kernel functions that take a raw node or slot count are `unsafe func`.** The tree kernel's `Tree::Minimum`, `Maximum`, `Successor` and `Predecessor` take a node pointer they cannot check: the caller must pass null or a live node of a tree whose links all point at live nodes of it. `Tree::Detach` must be given a live node of that very tree, and frees it, so nothing may read through the pointer afterwards. The hash-table kernel's `Table::Rehash` must be given a power of two no smaller than `MinimumTableCapacity`, which is what `Table::CapacityFor` returns, because every slot index is masked by it. Each call therefore sits in an `unsafe` block whose `// Safety:` comment says why that holds. The containers built on the kernels call them only that way, so nothing a container offers needs `unsafe`.
- **Indexing is checked.** `Get`, `At`, `AtMutable`, and `Set` report a miss or `IndexOutOfRange` rather than indexing past the allocation.

## Documentation

<https://rux-lang.dev/docs/api/collections>

## License

Licensed under the [MIT License](LICENSE.md).
