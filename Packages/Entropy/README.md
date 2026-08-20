# Entropy

Unpredictable bytes from the operating system.

## Installation

```sh
rux add Rux/Entropy
```

## What it provides

| Module   | Covers                                                                     |
| -------- | -------------------------------------------------------------------------- |
| `Error`  | `EntropyError`, and the questions worth asking of one                      |
| `Source` | `FillFromSource`, the system generator reached the way each system wants   |
| `Fill`   | `EntropySource`, `Fill`, `FillFrom` and `FillWithRetries`                  |

## Design

This package does one thing: fill a buffer with bytes an adversary cannot guess. It is not a generator. There is no
state here, nothing is reproducible, and drawing many bytes is slower than drawing them from a seeded generator —
ask here for a seed, and take the rest from `Rux/Random`.

Four systems offer four interfaces. Linux and FreeBSD have a `getrandom` system call that may write fewer bytes than
asked for and may be interrupted by a signal, so filling a whole buffer means looping; a signal is retried a bounded
number of times and then reported as `Interrupted` rather than spun on forever. Darwin's `arc4random_buf` is seeded
before any user code runs, never blocks, and cannot fail, so it returns nothing at all. Windows has
`BCryptGenRandom`, which fills the whole buffer or fails, and reports through an `NTSTATUS` rather than an error
number. One `EntropyError` covers all four.

A failure leaves nothing usable behind. Whatever a short read managed to write is zeroized before the failure is
reported, because a half-filled key looks exactly as random as a whole one — a caller who ignores the result should
get an obviously wrong buffer rather than a subtly weak one.

## Injecting failures

Entropy failures are the ones nobody tests. Reaching them needs a kernel with no generator, a process being signalled
steadily, or a cryptographic provider that refuses — none of which a test can arrange. So `FillFrom` takes an
`EntropySource` rather than calling the system by name: `SystemEntropy` is the real one, and a test supplies one that
fails exactly when it wants it to. The code that handles a failure is then the same code in the test and in
production, which is the only way to know it works.

An implementor of `EntropySource` must keep its mutable state behind a pointer — a small struct holding a pointer to
the state, not the state itself. Coercing a value to an interface hands the interface its own copy, so a source that
counted its calls in a field of its own would count them where nobody can see.

`FillWithRetries` only retries a transient failure. `Interrupted` says a signal arrived rather than that anything is
wrong; a system with no source and a provider that refused are both reported at once, since repeating them would only
take longer to reach the same answer. Its `attempts` is how many *extra* tries to make, so zero is one attempt.

## License

MIT. See [LICENSE.md](LICENSE.md).
