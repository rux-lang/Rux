# Json

JSON parsing and serialization.

## Installation

```sh
rux add Rux/Json
```

## What it provides

- **A document model** — move-only `JsonValue` trees with explicit `<-` transfers and deterministic `~Type`
  cleanup, covering the six kinds RFC 8259 defines. Objects keep arrival order, and numbers keep their original
  text, so an identifier past 2^53 comes back as itself rather than as the nearest double.
- **A parser** — strict RFC 8259, refusing every JavaScript-only form, with the position of whatever stopped
  it. Duplicate object names are rejected by default, and the other three policies are chosen per call.
- **Limits** — depth, bytes, string length and container size, each refused under its own reason, so a
  document written to exhaust the stack or the heap is refused while there is still room to refuse it.
- **Streaming** — events in document order from a stored `Rux/Io` reader handle, with a move-only owned buffer
  bounded by the largest token rather than by the document and safe operations borrowed through references.
- **Writers** — compact and pretty, borrowing values without copying, escaping exactly what the grammar
  requires and no more, and refusing to write a NaN or an infinity because JSON cannot spell one.

## Documentation

<https://rux-lang.dev/docs/api/json>

## License

Licensed under the [MIT License](LICENSE.md).
