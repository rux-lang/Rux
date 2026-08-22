# Crypto

Cryptographic hashes, message authentication codes and key derivation.

## Installation

```sh
rux add Rux/Crypto
```

## What it provides

- **Digests** — an incremental contract every algorithm implements, with one-shot forms beside it.
- **Constant-time comparison** — `Equal`, which takes the same time whichever bytes differ, because the
  ordinary comparison leaks where the first difference is and that is enough to forge a tag one byte at a time.
- **Zeroization** — `Wipe`, written so it cannot be optimized away.

## Documentation

<https://rux-lang.dev/docs/api/crypto>

## License

Licensed under the [MIT License](LICENSE.md).
