# Process

The running process — its command-line arguments, environment, identifier and exit — and the child processes it starts.

## Installation

```sh
rux add Rux/Process
```

## What it provides

| Module        | Covers                                                                                    |
| ------------- | ----------------------------------------------------------------------------------------- |
| `Arguments`   | `ArgumentCount`, `Argument`, and `Arguments` for walking them in order                    |
| `Environment` | `Variable` for one value by name, `Environment` for walking every `EnvironmentVariable`, and `SetVariable` and `RemoveVariable` |
| `Exit`        | `Exit`, which ends the process with a status, and `CurrentId`                             |
| `Command`     | `Command`, which describes a program to start, and `Stdio` for its standard streams       |
| `Child`       | `Child` with `Wait`, `TryWait` and `Kill`, its `ExitStatus`, and the `PipeReader` and `PipeWriter` streams |
| `Capture`     | `Command::Capture`, which runs a program and keeps its output, error and status           |

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

```rux
import Allocator::SystemAllocator;
import Io::IoError;
import Process::Command;

func Version() -> bool ! IoError {
    var command = Command(SystemAllocator(), "git");
    command.Argument("--version");
    let captured = command.Capture(SystemAllocator())?;
    return captured.Status().Success() && captured.Output().length > 0;
}
```

## Design

Arguments and environment values are native text, `Path::OsStringView`: bytes on the Unix systems and UTF-16 code units on Windows, exactly as the system handed them over. Nothing obliges either to be valid UTF-8 or UTF-16, so a program that needs text converts with `ToText` and decides what an unconvertible argument means. The first argument is the program as it was invoked, a name or a path the parent chose.

Every view of an argument or variable borrows storage the process owns for its whole life, so reading them allocates nothing and a view can be kept anywhere. Linux and FreeBSD keep the vectors on the stack the kernel built, which the entry stub records for `Core::ProcessStartFrame`; macOS has dyld record them in libSystem; Windows hands over one command line, which is split once with `CommandLineToArgvW` so that the arguments are the ones the C runtime would give, and one environment block, which is read once. The first read happens on first use and is kept, so a program that starts threads should read what it needs before it does.

Names are matched the way each system matches them: exactly on the Unix systems and without regard to ASCII case on Windows. `SetVariable` and `RemoveVariable` keep their changes in a table beside the environment the process started with, which `Variable`, `Environment` and every child this package starts consult first. Their text is never freed, so every view handed out stays valid. Windows is told of each change as well; the C library's own environment on the Unix systems is not, so code outside Rux that reads it there does not see the change.

A `Command` collects a program, its arguments, a working directory, environment changes for the child, and what each standard stream is connected to: this process's own, nothing, or a new pipe. A part the system cannot take, such as text with a NUL in it, is remembered and reported when the command is started, so the parts can be given without checking each. On Linux, FreeBSD and macOS the child is started with the C library's `posix_spawnp`, which finds the program through `PATH` and reports a program that could not be started as `NotFound` or the system's reason rather than as a child that exits; a program that starts children is therefore linked against the C library. On Windows it is started with `CreateProcessW`, from a command line quoted the way the C runtime splits one, so each argument arrives exactly as it was given. On the Unix systems a relative program path is looked up after the child has changed to its working directory; on Windows, before.

A `Child`'s pipes are `Io` streams the caller takes and owns; closing the writer ends the child's input. `Wait` closes that writer first, so a child that reads to the end is not left waiting. `Capture` reads standard error on a second thread while it reads standard output, so a child that fills one pipe while the other is being read never deadlocks. A child that is dropped without waiting keeps running; on the Unix systems its status then stays with the system until this process ends.

`Exit` ends every thread at once and reports its status to the parent, which sees the low eight bits on the Unix systems and all thirty-two on Windows. Nothing is flushed or destroyed on the way out, so returning from `Main` remains the ordinary way to finish.

## Documentation

<https://rux-lang.dev/docs/api/process>

## License

Licensed under the [MIT License](LICENSE.md).
