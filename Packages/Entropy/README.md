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

## License

MIT. See [LICENSE.md](LICENSE.md).
