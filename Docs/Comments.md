# Comments and Documentation

Rux has two ordinary comment forms and two documentation forms. All four are part of the language syntax; documentation comments additionally feed linting, formatting, generated API pages, and future code completion. The compiler keeps the exact source range and spelling of every comment so each tool reads the same source facts.

## Syntax

| Form          | Purpose                | Extent                         |
| ------------- | ---------------------- | ------------------------------ |
| `// text`     | Ordinary line comment  | Through the line ending        |
| `/// text`    | Documentation line     | Through the line ending        |
| `/* text */`  | Ordinary block comment | Through the matching delimiter |
| `/** text */` | Documentation block    | Through the matching delimiter |

The marker must be exact. `////` is an ordinary decorative line, not documentation. `/**/`, `/***/`, and blocks whose opening delimiter has more than two stars are ordinary blocks. This keeps banner comments from unexpectedly becoming API documentation. Delimiter-looking text inside string and character literals is never a comment.

Block comments nest. Each inner `/*` must have its own `*/`, regardless of whether the outer block is ordinary or documentation:

```rux
/* Disable this region.
   /* The explanation may contain another block. */
*/
```

An unterminated block is a language error at its opening delimiter. The lexer still retains the comment through EOF so diagnostics and formatters do not lose the authored text.

## Writing Documentation

Use one documentation form consistently within a short API description. Adjacent line and block documentation may be mixed when a generated or migrated source file needs it; they normalize into one Markdown value.

```rux
/// Parses one value.
///
/// # Failures
/// Returns an error when `input` is malformed.
///
/// @param input Source text.
/// @returns The parsed value.
func Parse(input: String) -> Value;
```

Block documentation is equivalent. `rux fmt` indents multiline content by four spaces without a star margin:

```rux
/**
    Parses one value.
    @param input Source text.
    @returns The parsed value.
*/
func Parse(input: String) -> Value;
```

Documentation content is normalized to LF Markdown. A line comment loses `///` and one optional following space. A block loses boundary-only lines, common indentation, and one aligned `*` margin plus its optional space. Meaningful blank lines, authored wrapping, nested Markdown indentation, and fenced code are preserved.

The safe Markdown surface includes paragraphs, emphasis, strong text, code spans, lists, fenced code, links using safe schemes, and headings. Prefer item-local headings such as `# Safety`, `# Failures`, and `# Panics`; generated pages lower them beneath the declaration heading without changing their relationship.

## Attachment

A documentation group attaches to the nearest following item at the same syntactic scope when:

- every comment begins on a leading source line (indentation is allowed);
- no blank line occurs between the group and the item;
- no ordinary comment or source token intervenes; and
- the group precedes either the declaration or its first attribute.

Documentation attaches to declarations, nested declarations, interface and extension methods, extern members, struct and union fields, enum and variant cases, and named variant fields. Parameters and type parameters are described with structured tags instead of comments inside a parameter list.

A trailing `///` after code never attaches forward. Detached, interrupted, trailing, and end-of-scope groups remain available as tooling issues. They do not make ordinary compilation fail, but `rux lint` reports them.

## Structured Tags

Tags form a terminal block after prose. They are case-sensitive and recognized only outside fenced code. An authored empty documentation line before the first tag is preserved, but `rux fmt` does not insert one. Preserve the authored tag order.

| Tag                            | Valid use                                                              |
| ------------------------------ | ---------------------------------------------------------------------- |
| `@param <name> <Markdown>`     | Named callable parameter other than `self` or an unnamed variadic tail |
| `@typeParam <name> <Markdown>` | Type parameter introduced by this declaration                          |
| `@returns <Markdown>`          | One value-returning callable that is not no-return or a destructor     |
| `@see <target> [Markdown]`     | Repeatable HTTP URL, Rux path, or backtick-delimited symbol            |
| `@deprecated <Markdown>`       | Any named documentable item or member                                  |

A continuation line begins with two spaces after the documentation marker. Empty lines may continue a tag description. Unknown aliases such as `@return`, missing subjects or descriptions, duplicates, unsafe references, one-space continuations, and prose resumed after the tag block are tooling issues. Structured-tag mistakes intentionally remain non-fatal to parsing and compilation so editing and code completion can operate on incomplete documentation.

`@deprecated` is documentation metadata. It does not emit the compile-time behavior of `#Warn` and does not by itself change source or binary compatibility.

## Tool Behavior

`rux fmt` edits only lexer-recognized documentation ranges. It canonicalizes `/// text`, empty `///`, one-line `/** text */`, four-space multiline block indentation, and tag spacing. Ordinary comments, decorative banners, Markdown-sensitive whitespace, tag separators, literals containing delimiters, and unterminated blocks retain their content. Formatting the result again must produce the same bytes.

`rux lint` reports malformed structured tags, invalid tag context, detached documentation, and missing documentation on public declarations. Missing-documentation help shows both `///` and `/** ... */`. Documentation does not need to carry an API-page URL, and `docs.api-url` is not a lint rule.

`rux doc` renders public items by default and can include private items when requested. It fails for documentation issues only when the affected item is included in the output. Prose and structured tag text are HTML-escaped. Structured tags become dedicated Deprecated, Type Parameters, Parameters, Returns, and See Also sections. An unambiguous local `@see` target links to its generated route; ambiguous, private-filtered, and unresolved external targets remain code text.

## Compiler and Tooling Contract

The lexer exposes every comment as lossless trivia with its kind, raw spelling, half-open byte range, line-leading state, and termination state. Documentation also reaches the parser as tokens carrying end positions and attachment metadata. The syntax tree stores normalized prose, exact line ranges, ordered typed tags, and recoverable issues in one `Documentation` value on each documentable node.

New tooling should consume that shared model rather than rescan source text. In particular:

- never search and replace comment delimiters without lexer ranges;
- do not infer attachment from proximity after parsing;
- keep tag order and source ranges for completion edits and diagnostics;
- treat issue-bearing documentation as usable partial input; and
- filter visibility before turning documentation issues into generator failures.

This contract lets formatters, linters, generated documentation, and code completion agree even while a comment is being edited.
