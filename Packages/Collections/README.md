# Collections

Generic data structures.

## Installation

```sh
rux add Rux/Collections
```

## What it provides

| Type         | Status                                        |
| ------------ | --------------------------------------------- |
| `Array`      | Available — fixed-size heap-allocated sequence |
| `List`       | Available — growable sequence                 |
| `Dictionary` | Planned                                       |
| `HashMap`    | Planned                                       |
| `HashSet`    | Planned                                       |
| `Queue`      | Planned                                       |
| `Stack`      | Planned                                       |

The planned modules exist as placeholders so the package layout and its documented surface stay stable while they are filled in. They declare nothing yet, so importing one is an error rather than a silent no-op.

Both available types own their storage through [`Rux/Memory`](../Memory) and are released explicitly.

`AsSlice` and `AsMutableSlice` hand out a borrowed view of the elements without copying them — read-only and writable respectively — which is how a container reaches [`Rux/Algorithms`](../Algorithms). A `List` view covers its initialized elements only, never its spare capacity, and any view is invalidated by a growth that moves the storage.

## Example

```rux
import Collections::List;

var numbers = List::New<int32>();
numbers.Add(1);
numbers.Add(2);
numbers.Free();
```

## License

Licensed under the [MIT License](LICENSE.md).
