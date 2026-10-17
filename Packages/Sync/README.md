# Sync

Locks, conditions, once-only initialization, semaphores, barriers, wait groups and channels for threads.

## Installation

```sh
rux add Rux/Sync
```

## What it provides

| Module     | Covers                                                                                             |
| ---------- | -------------------------------------------------------------------------------------------------- |
| `Lock`     | `Lock`, a bare lock, and `Once`, a once-only initialization; both can be module variables          |
| `Mutex`    | `Mutex<T>` with its `MutexGuard<T>`, and `Condition` to wait on while holding one                  |
| `RwLock`   | `RwLock<T>` with `ReadGuard<T>` and `WriteGuard<T>`: many readers or one writer                    |
| `Counters` | `Semaphore`, `Barrier` and `WaitGroup`                                                             |
| `Channel`  | `Channel<T>`, a bounded queue any number of threads send to and receive from                       |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Sync::Mutex;
import Thread::{ JoinHandle, Spawn };

func AddOne(total: Mutex<uint64>) -> () {
    let guard = total.Lock();
    *guard.Get() = *guard.Get() + 1u64;
}

func Main() -> int {
    let total = Mutex<uint64>(0u64);
    var first = Spawn(AddOne, total.Clone()) catch { else => return 1 };
    var second = Spawn(AddOne, total.Clone()) catch { else => return 1 };
    first.Join();
    second.Join();
    let guard = total.Lock();
    return *guard.Get() == 2u64 ? 0 : 2;
}
```

## Design

Every type here waits the same way: a thread sleeps on a 32-bit word only while the word still holds the value it last saw, and the system checks that as it puts the thread to sleep, so a change that lands just before the sleep is never missed. That is `futex` on Linux, `_umtx_op` on FreeBSD, `__ulock_wait` on macOS and `WaitOnAddress` on Windows. Taking a free lock and releasing one nobody waits for are each one atomic instruction; only contention costs a system call.

`Lock` and `Once` live where they are declared and need no destroying, so a module variable can be one, starting as `Lock::Unlocked` or `Once::Incomplete`, and every thread names it directly. The other types are handles to storage their clones share: `Clone` gives another handle to move to another thread, and the storage, with the value a `Mutex<T>`, `RwLock<T>` or `Channel<T>` holds, is destroyed when the last handle and the last guard are gone. A guard is the only way to reach a guarded value, and it releases its lock when it is dropped. The value's address a guard hands out is valid only while the guard holds the lock.

None of the locks is reentrant, and none queues its waiters in order: `RwLock` can keep a writer waiting behind a steady stream of readers. A `Condition` wait can return without a notification, so it is always written in a loop that checks the condition. Running out of memory while creating one of these objects is a program error, as is misuse the type cannot recover from, such as marking a `WaitGroup` done more times than work was added.

## Documentation

<https://rux-lang.dev/docs/api/sync>

## License

Licensed under the [MIT License](LICENSE.md).
