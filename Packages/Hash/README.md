# Hash

Named non-cryptographic hashes and checksums.

## Installation

```sh
rux add Rux/Hash
```

## What it provides

| Module   | Covers                                                                        |
| -------- | ----------------------------------------------------------------------------- |
| `Hasher` | the `Hasher` contract, and the writers every caller of one needs               |
| `Fnv`    | FNV-1a over 32 and 64 bits, incremental and one-shot                          |
| `Crc`    | CRC-32 and CRC-32C, their tables and their polynomials                         |
| `XxHash` | xxHash64, seeded, streaming and one-shot                                       |
| `SipHash`| keyed SipHash-2-4, for tables whose keys someone else chooses                  |

## Design

Every hash here is built the same way: make one, write bytes into it in order, and ask for the answer. That shape is
what lets a value be hashed without first being turned into a contiguous buffer — a struct writes each of its fields
in turn, and a collection each of its elements, with nothing copied anywhere.

Take a hasher by generic bound rather than as an interface value. `func Digest<H: Hasher>(hasher: *var H)` takes the
caller's own hasher by pointer and resolves at instantiation; an interface value would silently take a copy and leave
the caller's hasher exactly where it started. The interface exists to be the bound.

`WriteUint32` and `WriteUint64` use a fixed byte order rather than the machine's, so a value hashes the same on every
target. That is what makes the hash of a struct mean the same thing everywhere, and it costs nothing on a
little-endian machine.

## Speed

`XxHash64` is the one to reach for when there is a lot of data. Where FNV-1a takes a byte at a time, it takes
thirty-two: four independent accumulators, so a wide machine has four chains to run at once rather than one
dependency chain to wait on. The mixing is arithmetic rather than table lookup, which is what keeps it fast on data
that does not fit in cache — there is no table to evict.

A seed is part of the algorithm rather than an afterthought: two hashers with different seeds give unrelated answers
for the same input, which keeps accidental collisions from following data between tables. It is not enough to stop a
deliberate one.

Streaming is exact. Any division of the same byte stream into writes gives the same answer, including one byte at a
time and including a write that lands one byte short of a block boundary.

## Checksums

`Crc32` and `Crc32c` are cyclic redundancy checks, and what they buy over a sum is a guarantee rather than a hope: a
32-bit CRC detects every burst error up to 32 bits long, every single-bit and double-bit error, and every error of odd
weight — none of which a general-purpose hash promises about anything.

Both are the reflected, table-driven forms every implementation uses, so a value computed here matches gzip, PNG and
zip for `Crc32`, and iSCSI, ext4, Btrfs and SCTP for `Crc32c`. Prefer `Crc32c` for new work: it detects more of the
double-bit errors that appear at storage-record lengths, and both supported architectures implement it in hardware.
This implementation is the table rather than the instruction — correct everywhere, and about a byte per cycle.

A CRC detects accident, never intent. Given any message and any target value, producing a message with that CRC is
arithmetic a beginner can do. They implement `Hasher` because feeding them bytes has the same shape, not because they
substitute for one.

## Keying

Every other hash here has a fixed, public structure, so anyone can compute inputs that collide. Feed a few thousand
of those to a hash table and every one lands in the same bucket: lookups that should be constant become linear, and a
service doing fine falls over on traffic that looks perfectly ordinary. `SipHash24` is what stops that.

It stops it by being keyed. Without the key, finding two colliding inputs is as hard as breaking the function, and
the key is a secret the program draws from `Rux/Entropy` at startup and never publishes. A key compiled into a
program is a published key, and a published key is no key at all.

The price is speed — two rounds per eight bytes and four more at the end, against xxHash64's one round across four
independent lanes. For a table's short keys that is small and worth paying. For hashing a large file it is not.

Sixty-four bits is enough to make collisions unfindable at hash-table scale and not enough to authenticate a message.
`Rux/Crypto` has HMAC for that.

## What this is not

None of these are cryptographic. They are fast, they are named, and they are documented — but an adversary who gets
to choose the input can find collisions in any of them except `SipHash`, and even `SipHash` promises only that doing
so is hard without the key. Anything that must resist an adversary belongs in `Rux/Crypto`, which is also where
SHA-2, SHA-3, BLAKE3 and the explicitly named legacy MD5 and SHA-1 live.

A hash from this package is stable: FNV-1a here will agree with any other correct implementation, which is what makes
it worth naming an algorithm rather than promising only "some hash". `Core::Hashable` makes no such promise — its
answers are not stable across runs, versions or targets, and nothing that outlives the process should be keyed on
one.

## License

Licensed under the [MIT License](LICENSE.md).
