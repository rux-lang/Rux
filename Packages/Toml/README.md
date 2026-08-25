# Toml

TOML parsing, serialization and streaming.

## Installation

```sh
rux add Rux/Toml
```

## What it provides

- **Lexing** — every token TOML 1.1 defines, with source spans: bare and quoted keys, the four string forms,
  integers in four bases, floats, booleans, and the date and time types.
- **Parsing** — dotted keys, tables, arrays of tables, and the duplicate-definition rules that make TOML's
  table syntax unambiguous.
- **A semantic DOM** — ordered, move-only tables and arrays with explicit `<-` transfers and deterministic
  `~Type` cleanup, and dates and times as `Rux/Time` values rather than strings.
- **Writing** — deterministic output that borrows the document without copying and chooses a valid key and
  string form for whatever it is given.

## What it does not do

**This is not an editing library, and semantic parsing is not lossless.** A document is read into values, and
everything that was not a value is gone by the time you have one. Reading a file and writing it back produces
an equivalent document, not the same file.

Specifically, what a read-and-write loses:

- **Comments.** They are skipped by the lexer and never reach the parser, so nothing downstream could keep
  them even in principle.
- **Blank lines, indentation and spacing.** The writer produces its own layout: one blank line before each
  header, one space either side of an equals sign, and nothing else.
- **How a value was spelled.** `0xFF`, `0o377` and `255` are the same integer and all come back as `255`;
  `1e3` comes back as `1000.0`. A float is written at the fewest digits that read back as the same value, not
  at the digits it was written with.
- **Which string form was used.** Literal and multi-line strings come back as basic strings with escapes,
  because the value is the same and one form is one form fewer to reason about.
- **Whether a table was written inline.** `a = { b = 1 }` comes back as `[a]` with `b = 1` under it.
- **Where keys sat.** Entry order within a table is preserved; a dotted key that reached into a table is
  written as a header instead.

What is preserved is the tree: every key, every value, and the order entries were seen in. That is enough to
read configuration, to generate it, and to check one document against another — and not enough to edit a
file someone else wrote without churning it. For that, a format-preserving parser is a different piece of
software, and this is not it.

## Documentation

<https://rux-lang.dev/docs/api/toml>

## License

Licensed under the [MIT License](LICENSE.md).
