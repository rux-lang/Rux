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
| `String` | `<char8[..].h>` — counted memory operations and terminated byte strings       |
| `StdIo`  | `<stdio.h>` — streams, `printf` family, `fopen`/`fclose`, positioning       |
| `StdLib` | `<stdlib.h>` — allocation, conversion, process control, sorting, searching |
| `Math`   | `<math.h>` — the elementary functions in both `double` and `float` forms   |
| `Time`   | `<time.h>` — clocks, calendar time, and `timespec`                          |

A declaration that has to match a C interface is written against the `Types` aliases rather than Rux's own widths: `c_int`, `c_long` and `c_ulong` (which the two data models disagree about), `c_char` and its two explicit-signedness siblings, `size_t`, `ssize_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t` and `wchar_t`. A stream is a `*FILE` and a stream position is a `*fpos_t`, both declared with no fields, so one handle cannot be passed where another was meant. `ErrnoLocation` returns the address of the calling thread's `errno` through whichever accessor the platform exports, `Errno` reads it, `ErrnoLocationFor` and `ErrnoFor` name which of the package's runtimes to read, and `EDOM`, `ERANGE` and `EILSEQ` are the three values C itself mandates.

## Two C runtimes on Windows

`StdLib` and `Math` bind the Universal CRT (`ucrtbase.dll`); `StdIo` and `Time` bind the legacy `msvcrt.dll`, which is the only one exporting the formatted-output family. They are two loaded modules with two distinct `_errno` entry points, and `CRuntime` and `StreamRuntime` name them. `SeparateStreamRuntime` is true here and false everywhere else, where both constants name the one runtime.

`ErrnoFor(CRuntimeArea::Streams)` reads the runtime `StdIo` and `Time` are bound to and `ErrnoFor(CRuntimeArea::General)` the one `StdLib` and `Math` are bound to, so a caller reads the errno belonging to the call it actually made. Plain `Errno` is the `General` case. On the three Unix systems both areas name the same storage, so passing the area matching the module called is right on all four targets and needs no branch in caller code.

**What was measured, on Windows 11 with the current system DLLs.** The two `_errno` functions are different exports at different addresses, and both return *the same* per-thread storage: today's `msvcrt.dll` defers to the Universal CRT rather than keeping an `errno` of its own. Separately, a failing `fopen` through `msvcrt.dll` sets no `errno` at all, in either runtime — its failure is the null `FILE*` and nothing else. So the rule to follow after a `StdIo` or `Time` call is to read the return value, not `errno`; and where `errno` is consulted anyway, `ErrnoFor` is what names the runtime rather than leaving it to whichever accessor happened to be linked. A different Windows may resolve the two differently, which is exactly why the area is passed rather than assumed.

Both `StdLib` and `StdIo` are curated rather than complete, and what is left out is left out on purpose. `gets` is not declared at all: it reads a line into a buffer whose size it is never told, and C11 removed it. The `ato*` conversions are declared but each one says to prefer the `strto*` family beside it, which can report both where it stopped and a value out of range. `sprintf` and the `scanf` family are marked as writing as much as the format says to; `tmpnam` as naming a file it does not create; `system` as running whatever the shell decides its argument means; `rand` as guaranteeing nothing about its sequence and never to be used for anything an adversary would like to predict.

`String` holds two families that are not alike. The `mem*` calls take a length and touch exactly that many bytes; the `str*` calls take none and stop at the first zero byte, which means the caller has promised there is one. Where both forms exist the counted one is the one to reach for, and `Rux/Memory` and `Rux/Text` do these jobs with lengths that are checked. Each declaration records what it requires of its arguments, including the traps worth naming: `strncpy` does not terminate a source that fills the count, `strcat` walks the destination from its start on every call, and `strtok` keeps its position in storage shared by the whole program.

Each module selects the right declarations for the target at compile time, so the same import works across the supported platforms. `rux check --target <triple>` resolves every declaration here on all eight supported cells, and the `Layout` test checks the sizes and the runtime-filled structures on whichever one it runs.

## Values and pointers

C ABI records such as `timespec` are ordinary structural values and copy by value. Safe package helpers borrow them: `TimespecGet(value, TIME_UTC)` takes a mutable reference, so callers pass the value itself. C declarations keep raw pointers where the ABI permits null, returns an address, accepts an untyped buffer, or relies on pointer arithmetic. Those calls inherit C's validity, lifetime, alignment, and overlap requirements; use `Rux/Memory`, `Rux/Text`, or `Rux/Io` when a checked interface exists.

Two things are absent from `Math` on purpose. `fabsf`, `frexpf`, `ldexpf` and `hypotf` are header inlines in the Universal CRT rather than exported symbols, so declaring them would link on Unix and fail on Windows. The `nexttoward` pair takes a `long double`, which is a different type on every target — 64 bits under the Universal CRT, an 80-bit x87 value on x86-64 Unix, a 128-bit quad on AArch64 — so no one declaration is right everywhere. `Rux/Math` computes at every width without a C runtime and is the portable answer.

The module names above describe where each declaration comes from; they are not part of an import path.

## Example

Declarations are flat under the package, so an import names the function rather than the header it came from:

```rux
import C::{ CRuntimeArea, ErrnoFor, ErrnoLocationFor, free, malloc, puts };

func Main() -> int {
    // A Rux literal is a `char8[..]` and a C declaration takes a `*char8`, so the address is passed explicitly.
    var buffer = malloc(1024);
    if buffer == null {
        puts("allocation refused".data);
        return 1;
    }

    puts("allocated".data);
    free(buffer);

    // Clearing before the call is how C's `errno = 0` is spelled; nothing clears it on success. `malloc` comes from
    // `StdLib`, so its failure is readable in the `General` area.
    *ErrnoLocationFor(CRuntimeArea::General) = 0i32;
    let refused = malloc(18446744073709551615u64);
    if refused == null && ErrnoFor(CRuntimeArea::General) != 0i32 {
        puts("and the refusal reported a reason".data);
    }
    return 0;
}
```

This one compiles and runs on all eight target cells; the previous version of it did not compile at all, because it passed a literal where a `*char8` was wanted.

## Documentation

<https://rux-lang.dev/docs/api/c>

## License

Licensed under the [MIT License](LICENSE.md).
