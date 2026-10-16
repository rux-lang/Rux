# Compress

Deflate compression in the raw, zlib and gzip formats, as streams and for whole buffers.

## Installation

```sh
rux add Rux/Compress
```

## What it provides

| Module     | Covers                                                                                       |
| ---------- | -------------------------------------------------------------------------------------------- |
| `Inflate`  | `DecompressReader`, an `Io::Reader` that decompresses another reader                          |
| `Deflate`  | `CompressWriter`, an `Io::Writer` that compresses into another writer, with `Flush` and `Finish` |
| `Buffers`  | `Compress` and `Decompress` for data already in memory, as bytes or as the `char8` text the Io streams carry |
| `Format`   | `Format` (`Raw`, `Zlib`, `Gzip`) and `Level` (`Store`, `Fastest`, `Default`, `Best`)          |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Allocator::SystemAllocator;
import Compress::{ Compress, Decompress, Format };
import Io::IoError;

func RoundTrip(data: byte[..]) -> bool ! IoError {
    let packed = Compress(SystemAllocator(), data, Format::Gzip)?;
    let unpacked = Decompress(SystemAllocator(), packed.AsSlice(), Format::Gzip)?;
    return unpacked.Length() == data.length;
}
```

## Design

A Deflate stream is RFC 1951; the zlib format of RFC 1950 adds a two-byte header and an Adler-32 of the data, and the gzip format of RFC 1952 a header and a CRC-32 and length. The checksums come from `Rux/Hash`.

`DecompressReader` pulls compressed bytes from its source as it needs them and stops only when the caller's buffer is full, keeping the last 32 KiB it produced for the matches that reach back into them. It checks the container's header and trailer, reports the end of the data only after the checksum agreed, and reads a gzip file of several members as their data in order. Damaged or truncated input fails with `InvalidInput`, and so does every read after it.

`CompressWriter` gathers data until its buffer is full and encodes it as one block, keeping 32 KiB of history so matches reach into earlier blocks. Repeats are found through hash chains on three-byte prefixes, as zlib finds them; `Default` and `Best` follow longer chains and look one byte ahead before taking a match. Each block is written whichever way is smallest: stored, with the fixed codes, or with codes built from that block's frequencies, limited to fifteen bits the way zlib limits them. `Best` compresses about as well as zlib's default level. Nothing is complete until `Finish` writes the final block and the trailer; `Flush` makes everything so far readable without ending the stream, as zlib's sync flush does.

## Documentation

<https://rux-lang.dev/docs/api/compress>

## License

Licensed under the [MIT License](LICENSE.md).
