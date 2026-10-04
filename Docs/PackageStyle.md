# First-Party Package Style

This page is the house style for the 25 first-party packages under `Packages/`. It is narrower than [Comments and Documentation](Comments.md), which defines the _language_ contract — what comment syntax exists, how documentation attaches, and which tags the compiler understands. Nothing here changes that contract. Both `//` and `///` remain supported everywhere in the language; this page says which one first-party package sources use, and what a documented declaration in those packages must contain.

The rules are enforced by `Tests/Unit/PackageDocumentationStyleTests.cpp`, which reads the same lexer trivia, AST declarations and `Syntax::Documentation` metadata the compiler does. It does not rescan Rux with regular expressions. Run it on its own with:

```sh
./Bin/Tests/Unit/rux-tests --source-file=*PackageDocumentationStyleTests.cpp
```

## What the existing tools already cover

Three tools run before this checker and own rules it does not repeat.

| Tool       | What it owns                                                                            |
| ---------- | --------------------------------------------------------------------------------------- |
| `rux fmt`  | Supported comment spelling, tag spacing, and manifest canonical form                    |
| `rux lint` | Documentation attachment, public-declaration documentation, and structured-tag validity |
| `rux doc`  | Included documentation, generated routes, duplicate routes, and unsafe links            |

`rux fmt` preserves authored tag order and blank lines, and `rux lint` does not require complete parameter or return coverage or an API URL. Those additional rules belong to the checker described here.

## File headers and spacing

- Every maintained package source begins with an ordinary `//` header, placed before imports and attributes.
- The header explains the file's responsibility and any non-obvious ownership, representation or platform constraint. It is not a restatement of the file name.
- Extended design discussion belongs in the package README, not in a routine source file. Do not write migration history into a file that will outlive the migration.
- One blank line separates the header from the imports, and the imports from the first declaration.
- Declaration documentation attaches directly to its declaration, with no blank line between them.
- Files use LF line endings.
- Generated files keep their generated-data notice and provenance. When header output changes, change the generator that produces it; never hand-edit a generated file and never reflow its data tables.

## Imports

A multi-name import list is laid out by one mechanical rule, so that any two authors produce the same text:

1. Names are in ASCII order: upper case before lower case, and a `#` name such as `#target` before both.
2. The list stays on one line when the whole line fits in 120 columns.
3. Otherwise each line takes as many names as fit in 120 columns, breaking only after a comma, and every continuation line starts in the column just after `{ `.
4. The closing ` }` follows the last name on its line, never on a line of its own.
5. Break points are not chosen by hand; the rule decides them.

```rux
when #target.os {
    .Linux => import Linux::{ AtFdCwd, Close, Getdents64, MkdirAt, OpenAt, OpenDirectory, OpenReadOnly,
                              RemoveDirectory, UnlinkAt },
    .Windows => import Windows::{ CreateDirectoryW, DeleteFileW, ERROR_NO_MORE_FILES, FindClose, FindFirstFileW,
                                  FindNextFileW, GetLastError, InvalidHandleValue, RemoveDirectoryW }
}
```

The packed form keeps a platform `when` block short enough to compare its arms at a glance. Arms with module names of different lengths can still break at different names, which is the cost of a rule that needs no judgment. `rux fmt` does not reflow import lists yet, and no checker enforces this rule, so it is applied by hand until the formatter owns it.

## Extend blocks

A type's own members live in one `extend Type { ... }` block per file, ordered by topic. A block boundary is kept only where it means something: each interface conformance is its own block holding the methods that carry that interface, so `Next` sits in `extend MyCursor : Iterator { ... }` even though `Iterator` declares no members, and a block with a different header, such as a constrained one, stays separate. Members that belong together are grouped by order and, where a group needs naming, by a `//` comment inside the block rather than by a closing and reopening of the block.

## Integer literals

A local declared from a lone integer literal takes its type from an annotation rather than from a literal suffix: `var value: uint32 = 0;`, not `var value = 0u32;`. The type then sits after the name, where every other declaration, parameter and field puts it, and the compiler still checks that the literal fits it.

A literal beside a typed operand needs no suffix either, because it takes that operand's type: `year % 4 != 0` with an `int32` year. A suffix stays only where a literal has nothing else to take its type from, such as an argument that decides a generic type parameter. `Tests/Language` is exempt, since its tests exercise literal typing itself.

## Declaration documentation

Maintained package APIs use `///` throughout.

| Declaration kind                 | Required content                                                                                           |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| Public function                  | Summary, every named parameter, return meaning, relevant failure and safety behavior, canonical `@see` URL |
| Public method                    | The function requirements, excluding an `@param self`; plus state mutation or consumption                  |
| Constructor or factory           | Result invariant, allocation and failure behavior, and named ownership transfers                           |
| Struct, variant, enum, interface | Purpose, invariants, ownership and copy behavior, and relevant representation promises                     |
| Public field                     | Meaning, units, valid range, and ownership or lifetime where relevant                                      |
| Enum member or variant case      | Meaning, and payload semantics for a payload case                                                          |
| Interface requirement            | The parameter and return contract, and the obligations it places on implementations                        |
| Public `asm` function            | ABI, effects, clobber assumptions and raw-memory safety obligations                                        |
| Public `extern`                  | Foreign API meaning, linked runtime, sentinels, error source and pointer requirements                      |
| Public `const` or type alias     | Meaning and units or value contract, or the reason the alias exists                                        |
| Destructor                       | Cleanup behavior and retained unsafe assumptions; never `@returns`                                         |

A summary must say something. A restatement of the declaration's own name is not a summary: `/// Reads a byte.` on `func ReadByte()` conveys nothing the signature did not.

### Structured tags

Tags form one terminal block at the end of the documentation, in this order:

1. `@typeParam`
2. `@param`
3. `@returns`
4. `@see`

The block follows the prose directly, with no empty `///` line before it: a tag is recognized by its leading `@`, so a separator would only spend a line. The one empty line that matters comes after the summary when more prose follows, because the summary is the first paragraph and a Markdown paragraph ends only at an empty line. Within the tag block, prose does not resume; anything after the first tag is part of it.

```rux
/// The clock, read now.
///
/// Cannot fail on any supported system: the monotonic clock exists from boot and needs no permission.
/// @returns the monotonic clock, read now
/// @see https://rux-lang.dev/docs/api/time/instant/now
```

- `@typeParam` is required for every declared type parameter.
- `@param` is required for every named callable parameter except the `self` receiver. An unnamed variadic tail has no tag.
- `@returns` is required for every value-returning callable, excluding destructors and functions marked `#NoReturn()`.
- `@see` carries the canonical API URL. A field or a case may rely on the URL of the type that contains it rather than repeating one.

A bare URL written as prose is not a structured reference. `@see` is what gives a URL a defined documentation role, and it is what the generator renders into a See Also section. An unresolved external `@see` target may still render as code text rather than a live link; adopting the tag does not promise otherwise.

### Sections

Three prose headings have a fixed meaning inside declaration documentation.

- `# Safety` — obligations the language does not enforce: stored-address lifetime, alignment, nullability and invalidation.
- `# Failures` — recoverable failures, and the partial-output or partial-state guarantee that accompanies each.
- `# Panics` — used only where termination is actually possible.

## Prose conventions

These two rules apply to Markdown files, `//` file headers, `///` documentation and ordinary comments alike.

### The em dash

Use the em dash `—` for a parenthetical or appositive break in prose. The ASCII double hyphen `--` is not a dash in this repository. It appears only where it is literal syntax: a command-line option such as `--jobs 16`, `--release` or `--manifest-only`, or a path-separator argument such as `git checkout -- Path`.

This does not apply inside a fenced code block, an inline code span or an example command line, where `--` is the text being shown rather than punctuation.

### Markdown paragraphs are not wrapped

A Markdown paragraph is one line, however long. Tables, list items, block quotes and fenced code keep their own line structure, and a hard line break inside a paragraph is written only where it is meant.

Wrapping costs review clarity: reflowing a paragraph rewrites every line after the edit, which buries the sentence that actually changed under a diff of unchanged text.

Rux source comments are a separate case and remain wrapped to the source line limit, because they sit inside code that is wrapped.

## The legacy baseline

Applying the rules above to sources written before them would fail thousands of declarations at once, so the checker carries an explicit baseline of the violations that already existed when it was introduced: `Tests/Unit/PackageDocumentationStyleBaseline.txt`.

Each entry names one violation by the declaration's identity — package, source path, declaration kind and name — together with a fingerprint of the exact documentation and signature it was recorded against, and the rule it violates.

The baseline may only shrink. Three properties follow, and the checker enforces all three:

- A violation in the baseline is reported as accepted legacy debt rather than a failure.
- A violation **not** in the baseline fails immediately. New code meets the style.
- A baseline entry whose fingerprint no longer matches is treated as absent, so editing a declaration's documentation or signature makes it meet the style rather than inheriting its old exemption. An entry that matches nothing at all also fails, so entries are deleted as their declarations are fixed or removed.

Phase G requires the retained-source baseline to be empty. Until then, its size is the measure of remaining documentation work.
