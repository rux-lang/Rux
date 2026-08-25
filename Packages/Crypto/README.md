# Crypto

Cryptographic hashes, message authentication codes and key derivation.

> **Not published, and not to be published until reviewed.** Every algorithm here matches its published test
> vectors, but passing vectors is not the same as being safe to rely on: the things that go wrong in
> cryptographic code — a comparison that leaks timing, a secret that outlives its use, a construction used
> outside the assumptions it was proved under — pass vectors happily. This package is awaiting an independent
> review by someone who did not write it. Until that review is recorded, treat it as unaudited. The
> [review checklist](../../Docs/CryptoReview.md) says what is already established and what to look at first.

## Installation

```sh
rux add Rux/Crypto
```

## What it provides

- **SHA-2** — SHA-224, SHA-256, SHA-384, SHA-512, and the two SHA-512 truncations.
- **SHA-3 and SHAKE** — the Keccak permutation, the four fixed-size hashes, and the two extendable-output
  functions.
- **BLAKE3** — unkeyed, keyed, and derive-key modes, with extendable output.
- **HMAC and HKDF** — generic over any digest, and the two-stage key derivation built on them.
- **Legacy hashes** — `LegacyMd5` and `LegacySha1`, for reading what already exists. Both are broken; neither
  implements the digest interface, so nothing takes one by accident.
- **Digests** — an incremental contract every algorithm implements, with one-shot forms beside it.
- **Constant-time comparison** — `Equal`, which takes the same time whichever bytes differ, because the
  ordinary comparison leaks where the first difference is and that is enough to forge a tag one byte at a time.
- **Zeroization** — `Wipe`, written so it cannot be optimized away.

## Value and borrowing model

Digest, XOF, and HMAC states are structural `Copy` values: copying a state takes an independent snapshot and owns no
external resource. Safe mutation uses `&var` receivers, and read-only queries use `&`; calls borrow automatically.
Construct default states with type calls such as `Sha256()`, `Sha512()`, and `Blake3()`. Descriptive variants such as
`Sha256::New224()`, `Shake::New128()`, and `Blake3::NewKeyed()` retain their names. Raw pointers remain only inside
compression helpers and the optimizer barrier, where contiguous word access or an opaque address is intentional.

## Documentation

<https://rux-lang.dev/docs/api/crypto>

## License

Licensed under the [MIT License](LICENSE.md).
