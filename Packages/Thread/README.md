# Thread

Threads of execution: starting, joining and detaching them, and asking about the current one.

## Installation

```sh
rux add Rux/Thread
```

## What it provides

| Module    | Covers                                                                                      |
| --------- | ------------------------------------------------------------------------------------------- |
| `Spawn`   | `Spawn`, `SpawnWith`, `ThreadOptions` with a stack size, and `JoinHandle<R>` with `Join` and `Detach` |
| `Current` | `CurrentId`, `Yield` and `AvailableParallelism`                                             |
| `Error`   | `ThreadError`, why a thread could not be started                                            |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Thread::Spawn;

func Square(value: uint64) -> uint64 {
    return value * value;
}

func Main() -> int {
    var handle = Spawn(Square, 12u64) catch { else => return 1 };
    return handle.Join() == 144u64 ? 0 : 2;
}
```

## Design

Rux has no closures, so a thread runs a function on a context value the caller moves to it, and leaves the function's result for whoever joins it. `Spawn(entry, context)` returns a `JoinHandle<R>`; `Join` waits for the thread and returns the result. Dropping the handle, or calling `Detach`, lets the thread run on and release itself when it finishes, destroying a result nobody took.

On Linux, FreeBSD and macOS threads are started with the C library's `pthread_create`, which gives each one its stack and its thread-local block, so a program that starts threads is linked against the C library. On Windows they are started with `CreateThread`. Each thread has its own copy of every `thread var`, starting at its initial value.

A panic on any thread ends the whole process, as it does on the main thread. Sharing data between threads takes an `Atomic<T>` or a lock from the `Sync` package; this package only starts and joins them.

## Documentation

<https://rux-lang.dev/docs/api/thread>

## License

Licensed under the [MIT License](LICENSE.md).
