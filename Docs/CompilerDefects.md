# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### A method with its own type parameter on a non-generic type does not resolve at the call site

*Loud.* Found in `Rux/Memory`; `Layout::ForValue<T>` had to become the free function `LayoutOf<T>`.

### Equality on a multiword struct or tuple compares only its leading doubleword

*Silent.* `==` between two aggregate values wider than one register still loads the first eight bytes of each side and compares those unless the frontend supplies a structural operation. The AArch64 backend refuses a tuple comparison rather than quietly answering from its first element, while x86-64 silently does the latter. Variants already use case-aware structural equality.

### System V x86-64 loses the tail of a 9–15-byte aggregate

*Silent.* A by-value named struct whose runtime size is 9–15 bytes travels as a single register on System V x86-64. Caller and callee agree on that classification, but the tail beyond the first eight bytes is lost. Field padding makes these sizes uncommon; it does not make dropping their data correct. Argument placement, callee spills, affected returns, and textual assembly must preserve the complete value, including when argument registers are exhausted.

### A user-defined operator with a reference operand can crash

*Reported silent failure; reproduction pending.* A struct operator such as `func ==(self: &Money, other: &Money) -> bool` type-checks but crashes when called; the corresponding by-value operand works. The report also covers `<`. Verify both direct and already borrowed operands, with an ordinary named method as a control, before changing operator dispatch.

### A root nominal type can collide with a dependency's same-named type

*Reported failure; reproduction pending.* Declaring `ParseError` in the root package breaks a dependency's own `ParseError`, including Format's transitive parsing type. Renaming the root declaration avoids the failure. Package import and function identity repairs do not establish declaration ownership for nominal type lookup, extensions, generic substitutions, or layouts.

### An inline text ternary passed as a call argument can lose its slice

*Reported silent failure; reproduction pending.* Passing a conditional text expression directly to a call can produce an empty slice, and repeated inline arguments or calls can crash. Binding the selected text to a local works. The report includes variadic `Io::PrintLine`; direct, interface, nested, empty, and non-text slice cases still need verification.
