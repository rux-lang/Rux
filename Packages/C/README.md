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
| `StdIo`  | `<stdio.h>` — streams, `printf` family, `fopen`/`fclose`, positioning       |
| `StdLib` | `<stdlib.h>` — allocation, conversion, process control, sorting, searching |
| `Math`   | `<math.h>` — the elementary functions in both `double` and `float` forms   |
| `Time`   | `<time.h>` — clocks, calendar time, and `timespec`                          |

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
