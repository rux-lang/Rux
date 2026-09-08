# Toml

TOML parsing, serialization and streaming.

## Installation

```sh
rux add Rux/Toml
```

## What it provides

- **Lexing** — every token TOML 1.1 defines, with source spans: bare and quoted keys, the four string forms, integers in four bases, floats, booleans, and the date and time types.
- **Parsing** — dotted keys, tables, arrays of tables, and the duplicate-definition rules that make TOML's table syntax unambiguous. `TomlParse` returns `Result<TomlValue, TomlParseFailure>`: a document or a reason, never both. A refusal releases whatever had been read rather than handing it back, because a program configured from half a file runs with some settings applied and some defaulted, which is a state nobody wrote down.

  ```rux
  match TomlParse(allocator, bytes) {
      .Success(document) => Configure(document),
      .Error(failure) => Report(failure)
  }
  ```

- **Failures that name a rule and a place** — `TomlParseError` keeps all four table-definition cases apart, because `DuplicateKey`, `RedefinedTable`, `ExtendedInlineTable` and `ExtendedDottedTable` are four different mistakes with four different fixes. `Lexical` carries the lexer's reason and `BadValue` carries the scalar reader's, so a caller holding a failure cannot be holding half of one. `TomlParseFailure` adds the position three ways — a byte `offset` for a program to slice with, and `line` and `column` for a person — all naming the same byte, and `Debug` renders the lot: `TomlParseError::BadValue(TomlScalarError::Malformed(4)) at 1:12`. The position is where parsing *stopped*, which for a duplicate key is the value rather than the key, since the pair has to be read before the repeat is known.

  `TomlEventReader::Failure` answers the same `TomlParseFailure`, so the streaming reader and the tree parser report a refusal in one vocabulary rather than two.
- **A semantic DOM** — ordered, move-only tables and arrays with explicit `<-` transfers and deterministic `~Type` cleanup, and dates and times as `Rux/Time` values rather than strings.
- **Scalars that say where they went wrong** — `TomlParseInteger`, `TomlParseFloat` and `TomlParseDateTime` return `Result<T, TomlScalarError>`, and every failure carries the byte offset into the scalar's own text, reachable through `Text::ParseFailure`. The offsets are into the text as the caller wrote it, underscores included, so `0b1_0_2` points at the `2` where checking it happened four characters earlier. `TomlParseDateTime` answers a `TomlDateTime` carrying which of the four shapes it is, rather than a shape beside three fields the shape may or may not fill.

  The parser adds the two positions together — the token's place in the document, and the failure's place inside the token — so `port = 0b102` is reported at line 1, column 12, and `TomlParseOutcome::Scalar` hands the caller the scalar reader's own reason. Neither half is any use alone: the token's place points at the value rather than at what is wrong with it, and the offset is into a few characters nobody can find without it.
- **Writing** — deterministic output that borrows the document without copying and chooses a valid key and string form for whatever it is given.

## What it does not do

**This is not an editing library, and semantic parsing is not lossless.** A document is read into values, and everything that was not a value is gone by the time you have one. Reading a file and writing it back produces an equivalent document, not the same file.

Specifically, what a read-and-write loses:

- **Comments.** They are skipped by the lexer and never reach the parser, so nothing downstream could keep them even in principle.
- **Blank lines, indentation and spacing.** The writer produces its own layout: one blank line before each header, one space either side of an equals sign, and nothing else.
- **How a value was spelled.** `0xFF`, `0o377` and `255` are the same integer and all come back as `255`; `1e3` comes back as `1000.0`. A float is written at the fewest digits that read back as the same value, not at the digits it was written with.
- **Which string form was used.** Literal and multi-line strings come back as basic strings with escapes, because the value is the same and one form is one form fewer to reason about.
- **Whether a table was written inline.** `a = { b = 1 }` comes back as `[a]` with `b = 1` under it.
- **Where keys sat.** Entry order within a table is preserved; a dotted key that reached into a table is written as a header instead.

What is preserved is the tree: every key, every value, and the order entries were seen in. That is enough to read configuration, to generate it, and to check one document against another — and not enough to edit a file someone else wrote without churning it. For that, a format-preserving parser is a different piece of software, and this is not it.

## Documentation

<https://rux-lang.dev/docs/api/toml>

## License

Licensed under the [MIT License](LICENSE.md).
