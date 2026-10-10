# Test

Unit tests: functions marked `#Test()`, checks whose failures say what was expected and what was found, and the runner a test build gathers them into.

## Installation

```sh
rux add Rux/Test
```

## What it provides

| Module    | Covers                                                                                                     |
| --------- | ---------------------------------------------------------------------------------------------------------- |
| `Expect`  | `Expect`, `ExpectEqual`, `ExpectNotEqual`, `ExpectNear`, `ExpectText`, `ExpectSome`, `ExpectNone`, `ExpectSuccess` and `ExpectFailure` |
| `Failure` | `TestFailure`, a failure with its message or a skip with its reason                                          |
| `Runner`  | `TestCase`, `TestOptions`, `RunTests`, which reads its filters from the command line, and `RunTestsWith`      |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Test::{ ExpectEqual, ExpectText, TestFailure };

func Greeting(name: char8[..]) -> char8[..] {
    return name.length == 0 ? "hello" : "hello, friend";
}

#Test()
func GreetsAnyone() -> ! TestFailure {
    ExpectText(Greeting(""), "hello")?;
    ExpectEqual(Greeting("ann").length, 13u)?;
}
```

`rux test` builds the package in test mode and runs it, reporting each test with its time, and `rux test -- Greets` runs only the tests whose names hold `Greets`.

## Design

A test is a function marked `#Test()` that takes nothing and returns nothing or `! TestFailure`. It is declared at the top level of a source file or inside a `module` block, which then prefixes its name, as in `Parser::ReadsNumbers`. Each check returns `! TestFailure`, so a test passes a failed check on with `?` and stops there. The failure says what was expected and what was found: `ExpectText` names the byte where two texts part and shows each around it. A test ends with `fail TestFailure::Skip("reason")` to be counted as skipped rather than failed. A failure keeps its message inline, so failing never allocates and never fails itself.

A test build of a package with `#Test()` functions generates its `Main`: a table of `TestCase` values handed to `RunTests`. This works for libraries too, so a `SourceLibrary` can hold its own tests beside its code. The package must depend on `Test` and must not declare a `Main` of its own. `rux test` runs every executable package under `Tests/` and every package whose sources mark tests, and passes the arguments after `--` to each. The runner reads a plain argument as a filter on test names, with `--exact` to match whole names, `--skip <text>` to leave some out, `--list` to list them and `--quiet` to report only failures.

Tests run one after another in one process. A failed check ends only its own test. A panic or a failed `Assert` ends the whole run, which `rux test` then reports with the output up to that point.

## Documentation

<https://rux-lang.dev/docs/api/test>

## License

Licensed under the [MIT License](LICENSE.md).
