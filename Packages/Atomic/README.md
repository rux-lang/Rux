# Atomic

Values that threads share without a lock: atomic integers, flags and pointers.

## Installation

```sh
rux add Rux/Atomic
```

## What it provides

| Module   | Covers                                                                                                 |
| -------- | ------------------------------------------------------------------------------------------------------ |
| `Atomic` | `Atomic<T>` with `Load`, `Store`, `Exchange`, `CompareExchange` and `FetchAdd`/`Sub`/`And`/`Or`/`Xor`, and `With` forms that take a `MemoryOrder`; `Zero` for every integer width and `False` for `bool` |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Atomic::Atomic;
import Core::MemoryOrder;

func Count(hits: &Atomic<uint64>) -> uint64 {
    hits.FetchAddWith(1, MemoryOrder::Relaxed);
    return hits.Load();
}
```

## Design

`Atomic<T>` holds one integer of 8 to 64 bits, `bool` or pointer and touches it only through `Core`'s atomic intrinsics, so no thread ever sees a value half written and each change is one indivisible step. Its methods take the value by shared reference, because several threads reaching it at once is the point; the hardware keeps their accesses apart. Any other `T` is refused where the methods are used.

A call such as `Atomic(0u64)` is no compile-time value, so a module variable starts as a constant instead: each integer width's `Zero`, or `bool`'s `False`, reached through an alias of the instantiation, as in `type Hits = Atomic<uint64>;` and `var Count: Hits = Hits::Zero;`.

The plain methods are sequentially consistent: every thread agrees on one order of all of them, which is never wrong. The `With` forms take a weaker `MemoryOrder` for code that has worked out it is enough, such as a relaxed counter or a release decrement of a reference count. An order an operation cannot have is strengthened to the nearest one it can, so `LoadWith(MemoryOrder::Release)` loads with `Acquire`. Because the intrinsics need their order written as a case, a `With` method selects among them; the plain methods involve no such choice.

`CompareExchange` succeeds with the value it replaced and fails with the value it found, so a retry loop starts again from what is really there. It never fails spuriously.

## Documentation

<https://rux-lang.dev/docs/api/atomic>

## License

Licensed under the [MIT License](LICENSE.md).
