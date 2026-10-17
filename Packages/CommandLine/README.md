# CommandLine

Declarative command-line parsing: options, positional arguments, subcommands and generated help.

## Installation

```sh
rux add Rux/CommandLine
```

## What it provides

| Module   | Covers                                                                                                   |
| -------- | -------------------------------------------------------------------------------------------------------- |
| `Spec`   | `CommandSpec`, `OptionSpec`, `PositionalSpec`, and the `Command`, `Flag`, `ValueOption`, `Required`, `Optional` and `Remaining` builders |
| `Parsed` | `Parse` and `ParseProcess`, and the `Parsed` queries: `Has`, `Count`, `Value`, `Values`, `Positional`, `Positionals`, `Integer`, `Subcommand` |
| `Help`   | `WriteHelp`, `WriteUsage` and `WriteVersion`                                                              |
| `Error`  | `CommandLineError`, which describes itself through `Display`                                              |
| `Scan`   | `ArgumentSource`, the arguments a parse reads                                                             |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import CommandLine::{ Command, Flag, OptionSpec, ParseProcess, PositionalSpec, Required, ValueOption };

func Run() -> int {
    let options: OptionSpec[2] = [Flag("verbose", 'v', "Say more"), ValueOption("output", 'o', "FILE", "Where to write")];
    let positionals: PositionalSpec[1] = [Required("input", "What to read")];
    let spec = Command("tool", "Converts files").WithVersion("1.0.0").WithOptions(options[..]).WithPositionals(positionals[..]);
    let parsed = ParseProcess(@spec) catch { else => return 2 };
    if parsed.HelpRequested() || parsed.VersionRequested() { return 0; }
    return parsed.Count("verbose") as int;
}
```

## Design

A command is described as values: its name, summary and version, its options, its positional arguments and its subcommands. The description is what the arguments are checked against and what the help text is written from, so the two cannot disagree. Every part borrows its text and slices, so a whole command line can be described in locals and nothing is allocated.

The grammar is the conventional one. `--name` and `--name=value` are long options, and a value may also be the next argument. `-abc` is three short flags, and a short option that takes a value takes the rest of the cluster or the next argument, so `-o out`, `-oout`, `--output out` and `--output=out` mean the same. `--` ends options, `-` alone is an argument, and a repeated option keeps every value, with `Value` answering the last so that a later argument overrides an earlier one. A command with subcommands takes its first positional argument as the subcommand's name, and everything after it belongs to the subcommand. Every command understands `--help` and `-h`, and `--version` and `-V` when it has a version, unless it declares its own options of those names.

A parse checks the arguments once and keeps nothing but where they are. Each question walks them again with the same scanner, so an answer can never come from a reading different from the one that was validated, and parsing needs no storage. Values are `Path::OsStringView`s borrowing the arguments, lossless on every system; `Integer` reads a value as a decimal `int64`.

A mismatch is a `CommandLineError` that names the argument as it was typed, or the option or positional by its specification's name, and describes itself through `Display`, such as `option '--output' needs a value`. Asking for help or the version succeeds whatever else is missing.

## Documentation

<https://rux-lang.dev/docs/api/commandline>

## License

Licensed under the [MIT License](LICENSE.md).
