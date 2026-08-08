# Memory

Memory management functions: raw allocation and the block operations that go with it.

Every function dispatches on `#target.os` at compile time, so a build carries exactly one platform's implementation — `mmap`/`munmap` on Linux, macOS, and the BSDs, and the process heap on Windows.

## Installation

```sh
rux add Rux/Memory
```

## What it provides

| Function  | Purpose                                        |
| --------- | ---------------------------------------------- |
| `Alloc`   | Allocate an uninitialized block                |
| `Realloc` | Resize a block, preserving its contents        |
| `Free`    | Release a block                                |
| `Copy`    | Copy bytes between non-overlapping blocks      |
| `Set`     | Fill a block with a byte value                 |
| `Zero`    | Fill a block with zero                         |
| `Compare` | Order two blocks lexicographically by byte     |

## Example

```rux
import Memory::{ Alloc, Free };

var buffer = Alloc(1024);
Free(buffer);
```

## Documentation

<https://rux-lang.dev/docs/api/memory>

## License

Licensed under the [MIT License](https://github.com/rux-lang/Rux/blob/main/LICENSE.md).
