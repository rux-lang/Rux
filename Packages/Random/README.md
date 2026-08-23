# Random

Pseudorandom number generators and distributions.

> **Not for keys, tokens or anything an adversary would like to guess.** These generators are fast and
> reproducible, which is the opposite of what secrecy needs: anyone who sees enough output can reconstruct the
> state and predict the rest. For randomness that must not be guessed, use [`Rux/Entropy`](../Entropy).

## Installation

```sh
rux add Rux/Random
```

## What it provides

- **Generators** — `Xoshiro256` and `Pcg64Dxsm`, each seedable explicitly so a run is reproducible, or from
  `Rux/Entropy` when it should not be.
- **A generator contract** — `RandomGenerator`, which everything else is written against, so a caller's own
  generator works with all of it.
- **Uniform values** — integers of every width, floats in the unit interval, and bounded draws that are
  unbiased rather than merely close, using Lemire's method instead of a modulo.
- **Distributions** — normal, exponential, gamma, beta, Bernoulli, binomial, Poisson and weighted choice,
  built on exact reductions rather than on approximations that drift in the tail.
- **Sampling** — shuffling, choosing an index, weighted choice, and reservoir sampling for a stream whose
  length is not known in advance.

## Documentation

<https://rux-lang.dev/docs/api/random>

## License

Licensed under the [MIT License](LICENSE.md).
