# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### An inline text ternary passed as a call argument can lose its slice

*Reported silent failure; reproduction pending.* Passing a conditional text expression directly to a call can produce an empty slice, and repeated inline arguments or calls can crash. Binding the selected text to a local works. The report includes variadic `Io::PrintLine`; direct, interface, nested, empty, and non-text slice cases still need verification.
