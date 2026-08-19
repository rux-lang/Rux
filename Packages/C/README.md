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

Each module selects the right declarations for the target at compile time, so the same import works across the supported platforms.

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
