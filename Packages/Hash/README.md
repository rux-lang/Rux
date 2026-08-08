# Hash

Hash functions and checksums.

> **Not implemented yet.** The package is published so its name and identity are reserved and its place in the standard set is fixed. `Src/Md5.rux`, `Src/Sha256.rux`, and `Src/Sha512.rux` are placeholders that declare nothing, so importing from this package is an error rather than a silent no-op.

## Installation

```sh
rux add Rux/Hash
```

## Planned surface

MD5, SHA-256, and SHA-512, each as an incremental digest that can be fed in chunks as well as in one call.

These are checksum and integrity primitives. Password hashing and message authentication are deliberately out of scope for this package.

## License

Licensed under the [MIT License](LICENSE.md).
