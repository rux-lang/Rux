# Cryptographic Review Checklist

`Rux/Crypto` is **not published, and must not be published until an independent review is recorded here.** This page is what that reviewer needs: what is already established, what vectors cannot establish, and where to look first.

The requirement is that the reviewer is **not the author of the code**. A self-review satisfies the letter of a review while providing none of the thing it exists for.

## What is already established

- Every algorithm matches its published test vectors: FIPS 180-4 for SHA-2, FIPS 202 for SHA-3 and SHAKE, RFC 1321 for MD5, RFC 2202 and RFC 4231 for HMAC, and RFC 5869 for HKDF.
- Uniform coverage across all ten algorithms: avalanche over 512 flipped bits, boundary lengths at every block, rate and chunk size with the one-byte-shorter comparison beside each, and cross-algorithm distinctness.
- Incremental and one-shot forms agree, at every split point tested.

Passing vectors is what the code does when nothing is adversarial. None of the items below would fail a vector.

## What to look at first

1. **`Equal`, and whether the compiler preserves it.** It is written to take the same time whichever bytes differ, because the ordinary comparison leaks where the first difference is, and that is enough to forge a tag one byte at a time. Whether the *emitted code* still has that property, at each optimization level and on both back ends, is not something the source can promise on its own.

2. **`Wipe`, and whether its barrier survives optimization.** Rux has no `volatile`; zeroization relies on an opaque `asm func` barrier to stop the store being removed as dead. Confirm this on every target rather than on one.

3. **`Hmac`'s prototype copying.** It copies a digest state rather than re-initializing, which assumes digest states are trivially copyable — true of every current implementation, and an assumption a future algorithm could break silently.

4. **BLAKE3's tree flags.** The empty-message vector does not exercise the multi-chunk tree at all. The structural checks in place are not the same as official multi-chunk known-answer vectors, which are still worth fetching from upstream and adding.

5. **Secrets in stack buffers that are never wiped.** Every place a key, a block of key material or an intermediate state lives in a local and the function returns without clearing it.

6. **The legacy hashes.** `LegacyMd5` and `LegacySha1` are deliberately named and deliberately do not implement the `Digest` interface, so no generic function can take one by accident. Confirm nothing has since given them one.

## Recording the outcome

Add the reviewer, the date and the findings to this page, and remove the unaudited warnings from [`Packages/Crypto/README.md`](../Packages/Crypto/README.md) and `Packages/Crypto/Src/Digest.rux` only once the findings are closed.

Return to the [main README](../README.md) for the complete documentation index.
