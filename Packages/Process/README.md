# Process

The running process: its command-line arguments, environment, identifier and exit.

## Installation

```sh
rux add Rux/Process
```

## What it provides

| Module        | Covers                                                                                    |
| ------------- | ----------------------------------------------------------------------------------------- |
| `Arguments`   | `ArgumentCount`, `Argument`, and `Arguments` for walking them in order                    |
| `Environment` | `Variable` for one value by name, and `Environment` for walking every `EnvironmentVariable` |
| `Exit`        | `Exit`, which ends the process with a status, and `CurrentId`                             |

The module names describe where each declaration comes from; they are not part of an import path.

## Example

```rux
import Path::OsStringView;
import Process::{ ArgumentCount, Arguments, Variable };

func CountFlags() -> uint {
    var flags: uint = 0;
    var walk = Arguments();
    for argument in walk.Skip(1) {
        let units = argument.Units();
        if units.length > 0 && units[0] as uint32 == 0x2D {
            flags = flags + 1;
        }
    }
    return flags;
}

func HasHome() -> bool {
    let home = Variable("HOME") ?? OsStringView();
    return !home.IsEmpty() && ArgumentCount() > 0;
}
```

## Design

Arguments and environment values are native text, `Path::OsStringView`: bytes on the Unix systems and UTF-16 code units on Windows, exactly as the system handed them over. Nothing obliges either to be valid UTF-8 or UTF-16, so a program that needs text converts with `ToText` and decides what an unconvertible argument means. The first argument is the program as it was invoked, a name or a path the parent chose.

Every view borrows storage the process owns for its whole life, so nothing here allocates and an argument can be kept anywhere. Linux and FreeBSD keep the vectors on the stack the kernel built, which the entry stub records for `Core::ProcessStartFrame`; macOS has dyld record them in libSystem; Windows hands over one command line, which is split once with `CommandLineToArgvW` so that the arguments are the ones the C runtime would give, and one environment block, which is read once. The first read happens on first use and is kept, so a program that starts threads should read what it needs before it does.

Names are matched the way each system matches them: exactly on the Unix systems and without regard to ASCII case on Windows. The environment is the one the process started with; this package does not change it, and a change made through another interface after the first read is not seen here.

`Exit` ends every thread at once and reports its status to the parent, which sees the low eight bits on the Unix systems and all thirty-two on Windows. Nothing is flushed or destroyed on the way out, so returning from `Main` remains the ordinary way to finish.

## Documentation

<https://rux-lang.dev/docs/api/process>

## License

Licensed under the [MIT License](LICENSE.md).
