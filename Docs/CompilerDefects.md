# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### System V x86-64 loses the tail of a 9–15-byte aggregate

*Silent.* A by-value named struct whose runtime size is 9–15 bytes travels as a single register on System V x86-64. Caller and callee agree on that classification, but the tail beyond the first eight bytes is lost. Field padding makes these sizes uncommon; it does not make dropping their data correct. Argument placement, callee spills, affected returns, and textual assembly must preserve the complete value, including when argument registers are exhausted.

### A root nominal type can collide with a dependency's same-named type

*Reported failure; reproduction pending.* Declaring `ParseError` in the root package breaks a dependency's own `ParseError`, including Format's transitive parsing type. Renaming the root declaration avoids the failure. Package import and function identity repairs do not establish declaration ownership for nominal type lookup, extensions, generic substitutions, or layouts.

### An inline text ternary passed as a call argument can lose its slice

*Reported silent failure; reproduction pending.* Passing a conditional text expression directly to a call can produce an empty slice, and repeated inline arguments or calls can crash. Binding the selected text to a local works. The report includes variadic `Io::PrintLine`; direct, interface, nested, empty, and non-text slice cases still need verification.
