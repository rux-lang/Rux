# C

C standard library bindings, for interoperating with existing native code.

Nothing else in the standard packages depends on this one. [`Rux/Io`](../Io), [`Rux/Memory`](../Memory), and [`Rux/Math`](../Math) each reach the platform directly instead, so a Rux program links no C runtime unless it asks for one by importing this package.

## Installation

```sh
rux add Rux/C
```

## What it provides

| Module   | Covers                                                                     |
| -------- | -------------------------------------------------------------------------- |
| `Types`  | the C ABI's own types, `errno` access, and the opaque handles              |
| `String` | `<string.h>` — counted memory operations and terminated byte strings       |
| `StdIo`  | `<stdio.h>` — streams, `printf` family, `fopen`/`fclose`, positioning       |
| `StdLib` | `<stdlib.h>` — allocation, conversion, process control, sorting, searching |
| `Math`   | `<math.h>` — the elementary functions in both `double` and `float` forms   |
| `Time`   | `<time.h>` — clocks, calendar time, and `timespec`                          |

A declaration that has to match a C interface is written against the `Types` aliases rather than Rux's own widths:
`c_int`, `c_long` and `c_ulong` (which the two data models disagree about), `c_char` and its two explicit-signedness
siblings, `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t` and `wchar_t`. A stream is a `*FILE` and a stream
position is a `*fpos_t`, both declared with no fields, so one handle cannot be passed where another was meant.
`ErrnoLocation` returns the address of the calling thread's `errno` through whichever accessor the platform exports,
`Errno` reads it, and `EDOM`, `ERANGE` and `EILSEQ` are the three values C itself mandates.

> **On Windows this package spans two C runtimes.** `StdLib` and `Math` bind the Universal CRT (`ucrtbase.dll`), while
> `StdIo` and `Time` are still bound to the legacy `msvcrt.dll`, which is the only one exporting the formatted-output
> family. The two keep separate state, so an `errno` set by a stream call is not the `errno` this package reads. Do
> not read `errno` after a call made through `StdIo` or `Time` on Windows.

Both `StdLib` and `StdIo` are curated rather than complete, and what is left out is left out on purpose.
`gets` is not declared at all: it reads a line into a buffer whose size it is never told, and C11 removed it.
The `ato*` conversions are declared but each one says to prefer the `strto*` family beside it, which can
report both where it stopped and a value out of range. `sprintf` and the `scanf` family are marked as writing
as much as the format says to; `tmpnam` as naming a file it does not create; `system` as running whatever the
shell decides its argument means; `rand` as guaranteeing nothing about its sequence and never to be used for
anything an adversary would like to predict.

`String` holds two families that are not alike. The `mem*` calls take a length and touch exactly that many
bytes; the `str*` calls take none and stop at the first zero byte, which means the caller has promised there
is one. Where both forms exist the counted one is the one to reach for, and `Rux/Memory` and `Rux/Text` do
these jobs with lengths that are checked. Each declaration records what it requires of its arguments,
including the traps worth naming: `strncpy` does not terminate a source that fills the count, `strcat` walks
the destination from its start on every call, and `strtok` keeps its position in storage shared by the whole
program.

Each module selects the right declarations for the target at compile time, so the same import works across the supported platforms. `rux check --target <triple>` resolves every declaration here on all eight supported cells, and the `Layout` test checks the sizes and the runtime-filled structures on whichever one it runs.

Two things are absent from `Math` on purpose. `fabsf`, `frexpf`, `ldexpf` and `hypotf` are header inlines in the Universal CRT rather than exported symbols, so declaring them would link on Unix and fail on Windows. The `nexttoward` pair takes a `long double`, which is a different type on every target — 64 bits under the Universal CRT, an 80-bit x87 value on x86-64 Unix, a 128-bit quad on AArch64 — so no one declaration is right everywhere. `Rux/Math` computes at every width without a C runtime and is the portable answer.

The module names above describe where each declaration comes from; they are not part of an import path.

## Example

Declarations are flat under the package, so an import names the function rather than the header it came from:

```rux
import C::{ malloc, free, puts };

func Main() -> int32 {
    var buffer = malloc(1024);
    puts("allocated");
    free(buffer);
    return 0;
}
```

## Documentation

<https://rux-lang.dev/docs/api/c>

## License

Licensed under the [MIT License](LICENSE.md).
