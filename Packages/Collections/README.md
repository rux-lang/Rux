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

## Example

```rux
import Collections::List;

var numbers = List<int32>::New();
numbers.Add(1);
numbers.Add(2);
numbers.Free();
```

## License

Licensed under the [MIT License](https://github.com/rux-lang/Rux/blob/main/LICENSE.md).
