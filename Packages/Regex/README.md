# Regex

Regular expressions matched in linear time: Unicode classes and case folding, numbered and named groups, every match in turn, splitting and replacement.

## Installation

```sh
rux add Rux/Regex
```

## What it provides

| Module    | Covers                                                                                                     |
| --------- | ---------------------------------------------------------------------------------------------------------- |
| `Regex`   | `Regex` with `New`, `IsMatch`, `Find`, `FindAt`, `Capture`, `CaptureAt`, `FindAll`, `Split`, `Replace` and `ReplaceFirst`; `Match`, `Captures`, `MatchIterator`, `SplitIterator` and `Escape` |
| `Error`   | `RegexError` and `RegexErrorKind`, with the offset in the pattern where it went wrong                        |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Allocator::SystemAllocator;
import Regex::{ Regex, RegexError };

func Year(text: char8[..]) -> uint ! RegexError {
    let date = Regex::New(SystemAllocator(), "(?<year>\\d{4})-(?<month>\\d{2})-(?<day>\\d{2})")?;
    match date.Capture(text)? {
        .Some(groups) => return (groups.Named("year") ?? return 0).start,
        none => return 0
    }
}
```

## Design

The syntax is RE2's. A pattern holds literals, `.`, classes such as `[a-z]`, `[^0-9]` and `[[:alpha:]]`, the class escapes `\d`, `\w` and `\s`, Unicode general categories and properties such as `\p{L}`, `\p{Lu}`, `\p{Alphabetic}` and `\P{White_Space}`, and scripts such as `\p{Greek}`, `\p{sc=Han}` or `\p{Script=Cyrillic}`, which, as in RE2, test the Script property rather than Script_Extensions. It also holds the anchors `^`, `$`, `\A`, `\z`, `\b` and `\B`, the groups `(...)`, `(?:...)`, `(?<name>...)` and `(?P<name>...)`, alternation, and the repetitions `*`, `+`, `?` and `{n,m}` with lazy forms. The flags are `i` for case-insensitive, `m` for `^` and `$` at line ends, `s` for `.` matching `\n` and `x` for spaces and `#` comments, set as `(?i)` or for a group as `(?i:...)`. `\d`, `\w` and `\s` are Unicode-aware. Case-insensitive matching compares simple case foldings, so `k` matches the Kelvin sign but `ß` does not match `ss`, and a class matches every value its members fold with: `(?i)[\u{212A}]` matches `k`, and `(?i)[ς]` matches `Σ`. `$` without `m` matches only at the very end of the text.

Backreferences and lookaround are refused, because without them a search never needs to backtrack. The pattern compiles to a Thompson automaton that Pike's matcher runs over the text once, moving every thread of the match a character at a time. A search therefore takes time proportional to the length of the text times the size of the pattern, whatever either holds: `(a*)*b` over a long run of `a`s finishes as quickly as anything else. Threads are kept in order of preference, so a match is the leftmost one and, among those starting there, the one a backtracking matcher would choose: `foo|foobar` matches `foo`. When every match must start with the same text, or with one of a few bytes, a search skips straight to the places it can.

A text of 64 bytes or more is searched by two lazy DFAs instead. Each set of threads the matcher can be in becomes one state the first time a search meets it, and the state each character leads to is remembered, so a character costs one table lookup once a text's states are built. The forward DFA finds where the leftmost-first match ends, or that there is none; a second DFA, over the pattern reversed, reads backwards from there to where the match starts; and the Pike matcher runs over just that match when its groups are wanted. A pattern heavy with classes, such as `\w+\d\w+`, is searched about ten times as fast this way. The states are kept per search, or per iterator across its matches, and are dropped and rebuilt if a pattern makes too many, so memory stays bounded. A pattern too large to compile twice is searched by the Pike matcher alone.

Positions are byte offsets into the UTF-8 text searched, always at character boundaries. Each search allocates its working storage from the regex's allocator, which is why searches can fail with `OutOfMemory`. A compiled `Regex` never changes, so one can be searched from several threads at once. A counted repetition is written out in full when compiled, so a pattern whose compiled form would exceed 100,000 instructions is refused as `TooLarge`.

## Documentation

<https://rux-lang.dev/docs/api/regex>

## License

Licensed under the [MIT License](LICENSE.md).
