# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### An untyped `const` imported from another package fails inside a generic that a third package instantiates

*Loud, but only from a distance.* `Math::Tau` used in a generic function of `Rux/Random` reports "cannot determine the type of this expression" at the constant — but only when a test package instantiates that generic, so `rux check` passes and `rux test` fails. Narrowed by probe: a local untyped const works, an imported one works, a nested generic call works, and the generic living in a dependency package rather than the root is what breaks it, so the instantiation appears not to carry the imported-constant scope. Worked around in `Rux/Random` by declaring a typed constant inside the package. Annotating the `let` does not help, since the constant itself is what fails to resolve.

### A generic iterator reporting `Option<*var T>` does not survive lowering

*Loud.* The instantiation is named one way where its layout is recorded and another where it is looked up, the two disagreeing over whether the pointee's `var` belongs in the name, and lowering fails with "enum type `Option<*int32>` reached lowering without a layout marker". It rules out the obvious writable iterator, so `Rux/Collections` has none.

The non-generic form works since `19beafa`, which fixed the two layers above this one: a type read back from its name lost the `var` entirely, and substituting a type argument dropped the mark `*var T` puts on its `T` slot. What remains is the instantiation name itself. `alignof(T)` on a type parameter returning the size was a third defect in the same area, fixed in `4a83949`.

### A ternary does not take its type from context

*Loud.* `return condition ? 0 : 1;` in a function returning `uint` fails with "found 'int'", and so does an untyped literal passed to a `uint` parameter through one. Write the literal with its suffix, or use an `if`.

### A struct literal cannot appear anywhere inside an `if` condition

*Loud.* Its opening brace is taken as the start of the body, even when the literal is nested inside a call: `if !AllOf<int32>(Slice<int32> { ... }, IsEven) { ... }` is a parse error. Name the value in a `let` first.

### A generic argument is never inferred

*Loud, and merely verbose.* Every type parameter must be written at the call site: `MulWrapping(a, b)` fails where `MulWrapping<uint64>(a, b)` succeeds, and a type parameter behind a reference (`hasher: &var H`) is not deduced from `&var Counter` either.

### A method with its own type parameter on a non-generic type does not resolve at the call site

*Loud.* Found in `Rux/Memory`; `Layout::ForValue<T>` had to become the free function `LayoutOf<T>`.

### Equality on a multiword aggregate compares only its leading doubleword

*Silent.* `==` between two values wider than one register loads the first eight bytes of each side and compares those. For an enum whose payload does not pack beside its tag that is the discriminant alone: `ParseError::InvalidCharacter(3) == ParseError::InvalidCharacter(4)` is true. The x86-64 backend has always done this; the AArch64 backend now does the same for such enums deliberately so the two targets agree, and still refuses a tuple comparison rather than quietly answering from its first element — x86-64 silently does the latter. Compact payload enums whose tag and payload share one word compare fully, which hides the defect in most code. A correct comparison cannot be a byte comparison either, because a variant that carries no payload leaves those bytes unwritten; it has to branch on the variant, which is frontend work. Until then, a test that compares payload-carrying variants is only comparing which variant it is.

### A captured-output unit test is load-dependent

`CliProcessTests` "test keeps failed rows, reasons, diagnostics, and captured output together" fails roughly one run in six of the *full* unit suite, while the same case run alone passed 20 times out of 20 — so it is load-dependent, not logic-dependent. The child it captures panics and traps on `ud2`, and the panic's three `WriteFile` calls to the inherited pipe happen before the trap, so the bytes should already be buffered; `RunCaptured` in `Compiler/System/Process.cpp` then closes its write end and reads to EOF, which also looks right. The compiler binary was byte-identical across a clean full-suite run and a failing one. Worth chasing before it costs someone a red CI run they cannot reproduce.

## Fixed

Kept because they explain why some packages are written the way they are, and because two of them are the shape of the worst defect this code can surface: silent, wrong, and invisible to any test that does not check the data itself.

### A System V callee spilled one register of a two-register aggregate — fixed with the cross-platform bring-up

*Silent, and the single cause of over a hundred test failures apiece on Linux and macOS x86-64.* The caller measures an argument as the running program lays it out and passes a 16-byte named struct in two integer registers; the callee prologue classified the same parameter by the LIR-level `SizeOf`, which answers eight for a named struct it holds no layout for, so it spilled only the first register and read the second half of the value from a slot nothing had written. Every by-value 16-byte struct — `Layout` at every allocator call, interface values, `Vector`-shaped pairs — arrived half garbage on System V targets, while Win64 passes the same aggregates by reference and never touched the defective branch. Confirmed by disassembling the emitted ELF: `Vector::Scaled` spilled `rdi` but not `rsi`. The prologue now classifies by the same runtime size the caller uses. A related gap remains open: a by-value named struct whose runtime size is 9–15 bytes still travels as a single register on System V — both sides agree, so it is consistent, but the tail bytes are lost; field padding makes such sizes rare.

### One instantiation's copy or move operation leaked into every other — fixed with the windows-aarch64 bring-up

*Silent on x86-64, loud on AArch64.* The record saying "this generic store copies through a custom `=`" is keyed by the expression in the generic body, which every instantiation shares, and each instantiation's validation overwrote it with its own resolution — the last one won for all of them. Instantiating `Filled<Tracked>` and `Filled<int32>` from one program made `Filled_int32` call `Tracked::=` on an `int32` slot and patch the type mismatch with a cast; the AArch64 backend refused that cast ("cannot generate a cast from 'Tracked' to 'int32'"), which is how the first windows-aarch64 CI run surfaced it, while x86-64 emitted the same wrong LIR and happened to produce the right value because the cast read back the field the copy had just written. The record now keeps the unsubstituted type and no operation, and each instantiation substitutes its own type argument and resolves its own operation when its plan is built.

### An AArch64 store or load of a zero-sized value moved eight bytes — fixed with the windows-aarch64 bring-up

The x86-64 guard from `ace30585` never reached the AArch64 backend: a zero-sized store fell through to the same eight-byte fallback an unknown width gets and wrote over whatever followed the field, and a zero-sized load read past what was allocated. `Tests/Language/ZeroSizedField` fails at its first assertion on any AArch64 target without the guard. Both backends now skip the move entirely when a known width is zero.

### Interface coercion could copy or consume the implementor — fixed in `2f8ec825`

An ordinary by-value interface still has value semantics, but callers that need the original object can now borrow a concrete value directly as `&Interface` or `&var Interface`. The resulting fat reference points at the original data and its vtable without copying or consuming the implementor. `Rux/Entropy`, `Rux/Random`, allocator call sites, and stream helpers use borrowed interface views; stored handles remain raw only where a non-escaping reference cannot be a field.

### Lifecycle declarations depended on `Core::Drop` placement — fixed in `bc3202e5`

The canonical destructor is `func ~T(self: &var T)` inside `extend T`. It is a distinct special operation rather than an ordinary method whose meaning depends on an implemented interface, so the old silent `func Drop` mistake has no equivalent in the final syntax. First-party resource owners use type destructors, and the temporary `Core::Drop` compatibility path has been removed.

### Recursive and partial drop glue could miss or corrupt cleanup — fixed in `4ab7a38d`

Drop planning now handles recursive owners, partially constructed aggregates, non-generic enum payloads, generic destructors, and control-flow exits through branches, loops, returns, and `?`. The explicit `JsonValue` and `TomlValue` destructors remain because they state ownership clearly, not because synthesized recursive glue needs a workaround.

### A `const` array crossing a file boundary read garbage — fixed in `f090653`

A `const` array declared in one file of a package and read from another was not matched across objects at link time, so the reference pointed wherever the relocation landed and read plausible garbage — neither its values nor zeroes, and with nothing reported. Scalars were unaffected because they are folded into their use, which is why this survived until `Rux/Hash` published a lookup table.

### A generic container did not consume what was stored into it — fixed in `32d6d48b`

Storing a `T` into a generic container's storage was not recognized as consuming it, so the container kept a copy and the caller's value was destroyed where it stood — a use-after-free for any element owning memory. Consumption is now recorded as a question where it is asked and answered at each instantiation, and methods and associated functions of generic types are queued as instantiations at all, which they never were.

Reference provenance and explicit move operands now answer the public legality question: ownership cannot move through a reference or caller-visible raw pointer merely because the pointee type is movable. Named transfers use `<-`, so source invalidation is visible where it occurs. Generic owning containers have a narrow internal raw-storage transfer rule because the type system cannot yet distinguish their owned allocation from a borrowed raw pointer; the explicit `<-` remains required, while their public borrowed views never transfer ownership.

### A match-arm payload was destroyed twice, then not at all — fixed in `30d464a8` and `85d5d402`

A payload bound by a match arm now owns what it took and is destroyed when the arm ends, and the subject it came out of is not destroyed as well. The two halves landed together: before them, taking a value out of an option destroyed it twice; between them, not at all. Ownership follows the subject — an arm binding is registered for cleanup only where the subject was handed over, so matching a borrowed option still copies nothing and destroys nothing. Reading an aggregate subject straight out of its slot also had to clear the consumption, which was the one path that skipped it.

### A generic's interface bound resolved in the instantiator's scope — fixed in `4a96861`

A call from a package that had not imported the interface recorded no conformance — silently, since a use site does not report — and lowering then aborted the compiler with no output at all in a release build. This is why `Rux/Hash` is written with bounded generics, and why that was not possible before it was fixed.

### AArch64 could not open a 512-byte frame or write an aggregate constant — fixed in `64175ef4`

Two backend defects that `rux check --target` cannot see, because the frontend accepts the programs and only code generation fails. The frame record's immediate spans 512 bytes below the stack pointer but only 504 above it, and the limit was taken from the negative reach alone, so a frame of exactly 512 bytes encoded a prologue its epilogue could not close. Separately, an aggregate whose whole value is a literal — an enum variant carrying no payload, a zeroed structure — had no lowering at all. Found by cross-building the language suite for all seven non-host target cells, which is worth repeating whenever the backend changes.
