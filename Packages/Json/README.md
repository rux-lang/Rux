# Json

JSON parsing and serialization.

## Installation

```sh
rux add Rux/Json
```

## What it provides

- **A document model** — move-only `JsonValue` trees with explicit `<-` transfers and deterministic `~Type` cleanup, covering the six kinds RFC 8259 defines. Objects keep arrival order, and numbers keep their original text, so an identifier past 2^53 comes back as itself rather than as the nearest double.
- **A parser** — strict RFC 8259, refusing every JavaScript-only form. Duplicate object names are rejected by default, and the other three policies are chosen per call.
- **A document or a reason, never both** — `Parse` and `ParseWith` return `Result<JsonValue, JsonParseError>`. A refusal hands back no value at all, which is the distinction the old shape could not make: it answered a refused parse with `null`, and `null` is a perfectly good document, so a caller who did not read the outcome beside it could not tell one from the other. Whatever the parser had built when it stopped is released rather than handed over, so a caller is never left holding a partial tree.

  ```rux
  match Parse(allocator, bytes) {
      .Success(document) => Use(document),
      .Error(reason) => Report(reason.Offset(), reason)
  }
  ```

- **Failures carry where, and the lexer's reason with them** — every case of `JsonParseError` holds the zero-based byte offset where parsing stopped, reachable through `Text::ParseFailure`, and `Lexical` nests the `JsonLexError` inside itself so a caller holding the failure cannot be holding half of it. `Display` gives the description and `Debug` names the case with its position: `JsonParseError::Lexical(MalformedNumber, 1)`.
- **Limits** — depth, bytes, string length and container size, each refused under its own positioned reason, so a document written to exhaust the stack or the heap is refused while there is still room to refuse it. A limit the parser measures arrives as `TooDeep` or `TooManyEntries`; one the lexer measures arrives nested inside `Lexical`.
- **Streaming** — events in document order from a stored `Rux/Io` reader handle, with a move-only owned buffer bounded by the largest token rather than by the document and safe operations borrowed through references. `JsonEventReader::Failure` answers the same positioned `JsonParseError` the tree parser does, as an `Option`, so the two report a refusal the same way.
- **Writers** — compact and pretty, borrowing values without copying, escaping exactly what the grammar requires and no more, and refusing to write a NaN or an infinity because JSON cannot spell one. They report `Result<Unit, FormatError>`, the same contract every writer in the workspace uses, so a document goes into anything a `TextWriter` reaches with no translation at the seam. What the destination said travels unchanged: a buffer that is merely full reports `InsufficientCapacity` and a stream that has died reports `WriterFailure`, where `JsonWriteError` had one case standing for both. A non-finite number, and a document nested past `MaximumWriteDepth`, are `ValueOutOfRange` — the value is good and the requested form cannot hold it.

## Documentation

<https://rux-lang.dev/docs/api/json>

## License

Licensed under the [MIT License](LICENSE.md).
