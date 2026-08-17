# Collections

Generic data structures.

## Installation

```sh
rux add Rux/Collections
```

## What it provides

| Type         | Status                                        |
| ------------ | --------------------------------------------- |
| `Array`      | Available — fixed-length heap-allocated sequence |
| `Vector`     | Available — growable sequence, and the stack  |
| `Deque`      | Available — circular buffer, and the queue    |
| `HashMap`    | Available — open-addressed associative table  |
| `HashSet`    | Available — open-addressed set                |

The planned modules exist as placeholders so the package layout and its documented surface stay stable while they are filled in. They declare nothing yet, so importing one is an error rather than a silent no-op.

Both available types own their storage through [`Rux/Memory`](../Memory) and are released explicitly.

`AsSlice` and `AsMutableSlice` hand out a borrowed view of the elements without copying them — read-only and writable respectively — which is how a container reaches [`Rux/Algorithms`](../Algorithms). A `List` view covers its initialized elements only, never its spare capacity, and any view is invalidated by a growth that moves the storage.

## Example

```rux
import Collections::{ CollectionError, Vector };

var numbers = Vector::New<int32>();
if !(numbers.Push(1) == CollectionError::None) {
    return;
}
numbers.Free();
```

An operation that has to allocate reports a `CollectionError` and leaves the container unchanged unless it returns `None`, so an exhausted allocator is never mistaken for an empty container or a rejected index. Operations that cannot allocate report an ordinary miss with a `bool` instead.

## License

Licensed under the [MIT License](LICENSE.md).
