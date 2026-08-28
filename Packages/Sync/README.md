# Rux/Sync

Hardware-level atomic operations, memory barriers, and synchronization primitives for lock-free and thread-safe concurrency in Rux.

## Features

- **Memory Orders**: `Relaxed`, `Acquire`, `Release`, `AcqRel`, `SeqCst`.
- **Atomic Primitives**: `AtomicBool`, `AtomicInt32`, `AtomicInt64`, `AtomicUint32`, `AtomicUint64`, `AtomicPtr`.
- **Hardware Instructions**: Lock-free swap (`xchg`), compare-and-swap (`cmpxchg`), fetch-and-add (`xadd`), and memory fences (`mfence`/`dmb`).
- **Locks**: Lightweight, low-overhead `SpinLock` and thread-safe `Once` initialization.
