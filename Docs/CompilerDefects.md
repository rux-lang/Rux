# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### An interface value holds its own copy of what was coerced into it

*Silent.* `let source: I = value;` takes a copy; a call that mutates through `source` leaves `value` untouched. The copy is stable — repeated calls through the interface accumulate on it, and passing the interface value to a function keeps mutating the same copy — so this is coherent value semantics rather than a broken one. What makes it a hazard is that it is silent: the call reports success, nothing warns, and code written expecting reference semantics simply loses every mutation. Found in `Rux/Entropy`, where a test source that counted its own calls counted them where nobody could see. Confirmed identical in workspace and standalone builds.

The workaround is the one `ArenaHandle`, `PoolHandle` and `FixedBufferHandle` already use: keep everything mutable behind a pointer inside the implementing struct, so copying the struct copies a pointer and the state stays where its owner put it. **Every implementor of an interface in these packages must follow it.**

A generic bound is the other way out and the better one where it fits: `func F<H: Hasher>(hasher: *var H)` takes the implementor by pointer, resolves at instantiation, and mutates the caller's own value. Prefer a bound over an interface value wherever the type is known at the call site.

### Coercing a move-only value to an interface moves it

*Loud.* An `Arena` handed to something expecting an `Allocator` can no longer be reset or released by its owner. Found in `Rux/Allocator`. Diagnosed at compile time rather than silently, so it is a limitation rather than a hazard, and the handle pattern above is the answer to it as well.

### A `Drop` method in a plain `extend` is silently an ordinary method

*Silent.* Writing `extend T { func Drop(self: *var T) { ... } }` compiles, lints and reads exactly like a destructor, and is never called; only `extend T : Drop { ... }` registers one. Found in `Rux/Storage`, after `File` and `DirectoryIterator` had already shipped with safety nets that did not exist — and the failure is invisible, because the type still behaves correctly in every path that calls the method explicitly.

Worth a diagnostic: a method named `Drop` taking `*var Self` outside a `Drop` implementation is a mistake essentially every time. Note also that fixing it changes the type's semantics, since a droppable type becomes move-only, which is how the second half of that bug surfaced.

### Compiler-synthesized drop glue for a recursive type crashes

*Silent.* A struct holding a `Vector` of itself — `JsonValue` with `elements: Vector<JsonValue>` — with no explicit `Drop` implementation produced an access violation once four such values were live in one frame; two were fine. Adding an explicit `extend JsonValue : Drop` made it go away entirely, which locates the fault in the glue the compiler generates rather than in the ownership itself.

Narrowed and worked around in `Rux/Json`, and again in `Rux/Toml` (the explicit destructor is better design anyway, so the workaround is not a loss). Ruled out along the way: missing stack probes for large frames, and both ascending and far-end-first access patterns, which all behave correctly. Worth a proper fix, since the failure is silent, non-local and depends on how many values happen to be live.

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

*Loud, and merely verbose.* Every type parameter must be written at the call site: `MulWrapping(a, b)` fails where `MulWrapping<uint64>(a, b)` succeeds, and a type parameter behind a pointer (`hasher: *var H`) is not deduced from `*var Counter` either.

### A method with its own type parameter on a non-generic type does not resolve at the call site

*Loud.* Found in `Rux/Memory`; `Layout::ForValue<T>` had to become the free function `LayoutOf<T>`.

### A captured-output unit test is load-dependent

`CliProcessTests` "test keeps failed rows, reasons, diagnostics, and captured output together" fails roughly one run in six of the *full* unit suite, while the same case run alone passed 20 times out of 20 — so it is load-dependent, not logic-dependent. The child it captures panics and traps on `ud2`, and the panic's three `WriteFile` calls to the inherited pipe happen before the trap, so the bytes should already be buffered; `RunCaptured` in `Compiler/System/Process.cpp` then closes its write end and reads to EOF, which also looks right. The compiler binary was byte-identical across a clean full-suite run and a failing one. Worth chasing before it costs someone a red CI run they cannot reproduce.

## Fixed

Kept because they explain why some packages are written the way they are, and because two of them are the shape of the worst defect this code can surface: silent, wrong, and invisible to any test that does not check the data itself.

### A `const` array crossing a file boundary read garbage — fixed in `f090653`

A `const` array declared in one file of a package and read from another was not matched across objects at link time, so the reference pointed wherever the relocation landed and read plausible garbage — neither its values nor zeroes, and with nothing reported. Scalars were unaffected because they are folded into their use, which is why this survived until `Rux/Hash` published a lookup table.

### A generic container did not consume what was stored into it — fixed in `32d6d48b`

Storing a `T` into a generic container's storage was not recognized as consuming it, so the container kept a copy and the caller's value was destroyed where it stood — a use-after-free for any element owning memory. Consumption is now recorded as a question where it is asked and answered at each instantiation, and methods and associated functions of generic types are queued as instantiations at all, which they never were.

What is still not answered per instantiation is whether the move was *legal*: a container taking a value out of storage it owns and a caller moving out of a borrowed slice are indistinguishable, so the check is left where a concrete type makes it decidable. Saying which is which needs a way to name an owned pointer.

### A match-arm payload was destroyed twice, then not at all — fixed in `30d464a8` and `85d5d402`

A payload bound by a match arm now owns what it took and is destroyed when the arm ends, and the subject it came out of is not destroyed as well. The two halves landed together: before them, taking a value out of an option destroyed it twice; between them, not at all. Ownership follows the subject — an arm binding is registered for cleanup only where the subject was handed over, so matching a borrowed option still copies nothing and destroys nothing. Reading an aggregate subject straight out of its slot also had to clear the consumption, which was the one path that skipped it.

### A generic's interface bound resolved in the instantiator's scope — fixed in `4a96861`

A call from a package that had not imported the interface recorded no conformance — silently, since a use site does not report — and lowering then aborted the compiler with no output at all in a release build. This is why `Rux/Hash` is written with bounded generics, and why that was not possible before it was fixed.

### AArch64 could not open a 512-byte frame or write an aggregate constant — fixed in `64175ef4`

Two backend defects that `rux check --target` cannot see, because the frontend accepts the programs and only code generation fails. The frame record's immediate spans 512 bytes below the stack pointer but only 504 above it, and the limit was taken from the negative reach alone, so a frame of exactly 512 bytes encoded a prologue its epilogue could not close. Separately, an aggregate whose whole value is a literal — an enum variant carrying no payload, a zeroed structure — had no lowering at all. Found by cross-building the language suite for all seven non-host target cells, which is worth repeating whenever the backend changes.
