# Known Compiler Defects

Defects in the Rux compiler that change how the first-party packages under `Packages/` are written, rather than merely what compiles. Each entry says what goes wrong, how it was found, and what the packages do instead.

An entry is **loud** when the compiler reports it, and **silent** when the program builds and misbehaves. The silent ones are the reason this page exists: a workaround nobody knows about gets removed by the next person who tidies the code.

Return to the [main README](../README.md) for the complete documentation index.

## Open

### `?` discards a payload-carrying error's payload

*Silent, and it empties the part of an error a caller reads.* Propagating a failure with `?` delivers the right variant case with its payload zeroed. Matching the same result directly delivers it intact, so the value exists and is lost in transit.

```rux
variant Reason { First(uint), Second(uint) }

func Step(at: uint) -> Result<Unit, Reason> {
    return Result::Error<Unit, Reason>(Reason::First(at));
}

func Outer(at: uint) -> Result<uint, Reason> {
    Step(at)?;                                   // the payload does not survive this
    return Result::Success<uint, Reason>(0u);
}
```

`Step(10)` matched directly yields `First(10)`. `Outer(10)` yields `First(0)`. Narrowed by probe: it happens whether or not the success types differ, and whether or not the source's success type is `Unit`, so it is `?` itself rather than anything about the payload being zero-sized. A payload-less error propagates correctly, which is why nothing had noticed.

This is why `Rux/Time`'s calendar parsers report failure through an output slot and re-wrap it at the entry point rather than using `?` throughout. Assigning a variant through a `*var` slot preserves the payload, and so does returning `Result::Error` built from it, so the workaround is complete:

```rux
var error = TimeParseError::InvalidSyntax(0);
if !ReadDate(reader, @date, @error) { return Result::Error<Date, TimeParseError>(error); }
```

Found while giving the temporal parsers byte offsets: every failure reported offset 0, and the offsets were correct right up to the point they crossed a `?`. Nothing earlier in this workspace had caught it because the errors `?` had been used with — `FormatError` at the writer boundary, `AllocError` — either carry no payload or are returned directly rather than propagated. `Rux/Format`'s `ParseFormatSpec` is unaffected for the same reason: it returns its positioned failure rather than propagating one.

### A package cannot have a transitive dependency on a different package that shares its name

*Loud, and the diagnostic points somewhere else entirely.* A package named `Text` whose dependency graph reaches `Rux/Text` further down does not resolve that package: the imports fail with `name 'Display' was not found in package 'Text'`, reported against the *dependency's* source rather than against the root that collides, and nothing in the output mentions a name collision.

Found when `Rux/Time` gained its `Rux/Text` dependency. `Tests/Packages/Uuid/Text` — an executable test whose package was named `Text` — depends on `Uuid`, which depends on `Time`, which now depends on `Text`. Every import inside `Packages/Time/Src/CalendarText.rux` then failed, and the failure looked like `Rux/Text` being stale or half-loaded. It is neither: renaming the test package to anything else fixes it with no other change, which is how it was narrowed. The test is now `Tests/Packages/Uuid/Formatting`.

The bare name appears to be the resolution key, so the root's own name shadows a package of that name anywhere in its graph. Two things would each help: resolving by `Namespace/Name` rather than by name, and reporting the collision where it happens rather than as a missing declaration three packages away.

### Passing a concrete value as an interface argument *of an interface method call* crashes

*Silent, and it is a segmentation fault.* A concrete local coerces to an interface parameter correctly when the callee is an ordinary function, and incorrectly when the callee is reached through an interface. The second form builds without a diagnostic and faults at the call.

```rux
var writer = BufferWriter((@storage[0])[..32]);
let value: Display = StringView::FromValidated("abc");

WriteBytes(writer, "abc");                          // free function: correct
value.WriteDisplay(writer, FormatSpec::Plain());    // interface method: faults
```

Binding the argument to its interface type first is a complete workaround, and is what the first-party code already does:

```rux
let sink: &var TextWriter = writer;
value.WriteDisplay(sink, FormatSpec::Plain());      // correct
```

Narrowed by probe to exactly that difference: three functions doing the same write, differing only in whether the callee is a free function, an interface method given the concrete local, or an interface method given a bound reference. The first and third return 3; the second faults. Both the argument and the receiver being interfaces appears to be what does it — the argument alone is fine.

Found while writing `Tests/Packages/Text/Presentation`, whose helper called `value.WriteDisplay(writer, spec)` directly. It is written with the bound reference now, and says why. Existing first-party callers were unaffected because they already hold a `&var TextWriter` parameter rather than a concrete writer, so the coercion has happened before the interface call.

### Building one package test directly resolves its dependency's dependencies from the install cache, not the workspace

*Loud, once the two disagree, and silently wrong until then.* `rux test` from the repository root resolves everything locally and is unaffected. Building a single package test by its own manifest is not:

```sh
rux --manifest Tests/Packages/Format/Uint8/Rux.toml build --release
```

That test declares `Format` and `Text` as path dependencies, but `Format`'s own manifest declares `Text` by version, as a publishable manifest must. The version requirement is satisfied from the user-level install cache (`%LOCALAPPDATA%/rux/Packages` on Windows) rather than from the path the root already supplies for the same package identity, so the build compiles the workspace's `Format` against a cached `Text`. Where the two agree, everything passes and nothing indicates which was used; where they differ, the failure names declarations that plainly exist — `name 'Display' was not found in package 'Text'` against a `Text` whose sources declare it.

Found while moving the formatting contracts into `Text`: the workspace `rux check` and `rux test` passed while the standalone build of a Format test failed on every new declaration. Confirmed by adding a declaration to an existing `Text` source and watching a single-test build fail to see it, and by moving the cache aside, after which the same build cannot resolve `Rux/Allocator` at all — so the cache is genuinely what satisfies a dependency's own dependencies here.

This contradicts what CONTRIBUTING promises: "Repository tests resolve every dependency from the local workspace, so no registry installation is needed." Use `rux test` from the root, which is the documented workflow and is correct. Note that no first-party package is published, so whatever populated that cache was local; it is a stale copy of an older working tree rather than anything the registry served.

### A `return` expression is evaluated after the function's `defer` statements run

*Silent, and it changes the value a function returns.* `return expr;` should read `expr` and then run the deferred statements on the way out. It does the opposite: the defers run first and `expr` is evaluated afterwards, so a `defer` that mutates a local is visible in the value the caller receives.

```rux
func Doubled() -> int {
    var a = 1;
    defer a += 10;
    return a * 2;   // 22, where 2 is correct
}
```

Narrowed by probe. `return a;` gives 11 rather than 1, and `return a * 2;` gives 22 rather than 2 — the multiplication is applied to the mutated value, so this is the expression being evaluated late rather than only the return slot aliasing the variable. Two defers compound the same way, in the correct LIFO order: `defer a += 100;` then `defer a *= 3;` over `var a = 1` returns 103. Copying first is a complete workaround, because the copy is a separate binding no defer touches:

```rux
let snapshot = a;
return snapshot;    // 1, correct
```

A `return` of a literal, or of anything no deferred statement mutates, is unaffected, which is why this has gone unnoticed: the existing `Tests/Language/Defer` case asserts the returned value is 11 and reads as though that were intended.

`Tests/Language/Defer` now pins the symptom deliberately, next to a comment naming this entry, so that fixing the defect fails that case and points at itself rather than at a mystery. Nothing in the release is affected: `defer` appears in no first-party package at all, so the blast radius today is language tests and whatever a user writes.

### A borrowed scalar cannot be read back out, so a primitive's interface implementation must take a raw pointer

*Loud, and it forces a raw pointer into an otherwise safe surface.* A `&T` over a scalar supports nothing but being passed on: `self as int64`, `self < 0i64` and `let copied: int32 = self;` are each rejected, because a reference is dereferenced automatically for member and index access and for nothing else. There is no deref operator for a reference — `*self` is raw-pointer syntax — so a borrowed `int32` cannot become an `int32` again.

That would be a curiosity if a primitive could implement an interface by value, but it cannot: an implementation whose receiver is `self: int32` reports "method 'WriteDisplay' cannot take its receiver by value because this block implements interface 'Display'". The two rules together leave exactly one form that compiles, and it is the raw pointer. So every primitive `Display` and `Debug` in `Rux/Format` is written `self: *int32`, dereferencing with `*self` — the one place in the first-party packages where a raw pointer receiver is not a documented unsafe boundary but the only thing available.

Found while trying to give the integer widths borrowed receivers, which the package style would otherwise require. Either half would fix it: letting a reference to a `Copy` scalar convert to its value where a value is wanted, or letting an interface implementation take a scalar receiver by value the way an ordinary method does. Until then the raw pointer is load-bearing, and replacing it with `&T` does not compile rather than compiling and misbehaving, which is the one comfort here.

### `char64` has no character literal prefix, and `c64'x'` fails as a parse error rather than as an unknown prefix

*Loud, and misleading about what is wrong.* `char64` is a fully implemented width — it has a catalog entry, `Min`, `Max`, `Bits` and `Bytes`, and the changelog announces it as the fourth character width — but `Lexer::ScanToken` recognizes only `c8`, `c16` and `c32` as literal prefixes. `c64` therefore lexes as an identifier followed by a separate character literal, and the parser reports something like `expected ',' between arguments before ''A''`, which points at the quote rather than at the prefix and does not mention `c64` at all.

Every other implemented character width can be written directly; this one is reached only by converting, as `c32'\u{1F600}' as char64`. `Tests/Language/Char64` already does exactly that throughout, and `Tests/Language/As` does the same where it needs a `char64` value, so the workaround is established rather than newly invented.

Found while writing the `as` conversion case for the language test matrix. Two things would each be an improvement on their own: accepting `c64` alongside the other three, or — if the omission is deliberate, since `char64` and `char32` carry the same thing — reporting an unknown character-literal prefix by name instead of letting it fall through to identifier scanning.

### An untyped `const` imported from another package fails inside a generic that a third package instantiates

*Loud, but only from a distance.* `Math::Tau` used in a generic function of `Rux/Random` reports "cannot determine the type of this expression" at the constant — but only when a test package instantiates that generic, so `rux check` passes and `rux test` fails. Narrowed by probe: a local untyped const works, an imported one works, a nested generic call works, and the generic living in a dependency package rather than the root is what breaks it, so the instantiation appears not to carry the imported-constant scope. Worked around in `Rux/Random` by declaring a typed constant inside the package. Annotating the `let` does not help, since the constant itself is what fails to resolve.

### A generic iterator reporting `Option<*var T>` does not survive lowering

*Loud.* The instantiation is named one way where its layout is recorded and another where it is looked up, the two disagreeing over whether the pointee's `var` belongs in the name, and lowering fails with "variant type `Option<*int32>` reached lowering without a layout marker". It rules out the obvious writable iterator, so `Rux/Collections` has none.

The non-generic form works since `19beafa`, which fixed the two layers above this one: a type read back from its name lost the `var` entirely, and substituting a type argument dropped the mark `*var T` puts on its `T` slot. What remains is the instantiation name itself. `alignof(T)` on a type parameter returning the size was a third defect in the same area, fixed in `4a83949`.

### A ternary does not take its type from context

*Loud.* `return condition ? 0 : 1;` in a function returning `uint` fails with "found 'int'", and so does an untyped literal passed to a `uint` parameter through one. Write the literal with its suffix, or use an `if`.

### A struct literal cannot appear anywhere inside an `if` condition

*Loud.* Its opening brace is taken as the start of the body, even when the literal is nested inside a call: `if !AllOf<int32>(Record { ... }, IsEven) { ... }` is a parse error. Name the value in a `let` first.

### A generic argument is never inferred

*Loud, and merely verbose.* Every type parameter must be written at the call site: `MulWrapping(a, b)` fails where `MulWrapping<uint64>(a, b)` succeeds, and a type parameter behind a reference (`hasher: &var H`) is not deduced from `&var Counter` either.

### A method with its own type parameter on a non-generic type does not resolve at the call site

*Loud.* Found in `Rux/Memory`; `Layout::ForValue<T>` had to become the free function `LayoutOf<T>`.

### Equality on a multiword struct or tuple compares only its leading doubleword

*Silent.* `==` between two aggregate values wider than one register still loads the first eight bytes of each side and compares those unless the frontend supplies a structural operation. The AArch64 backend refuses a tuple comparison rather than quietly answering from its first element, while x86-64 silently does the latter. Variants no longer take this path: their equality is case-aware and structural, as recorded below.

### A captured-output unit test is load-dependent

`CliProcessTests` "test keeps failed rows, reasons, diagnostics, and captured output together" fails roughly one run in six of the *full* unit suite, while the same case run alone passed 20 times out of 20 — so it is load-dependent, not logic-dependent. The child it captures panics and traps on `ud2`, and the panic's three `WriteFile` calls to the inherited pipe happen before the trap, so the bytes should already be buffered; `RunCaptured` in `Compiler/System/Process.cpp` then closes its write end and reads to EOF, which also looks right. The compiler binary was byte-identical across a clean full-suite run and a failing one. Worth chasing before it costs someone a red CI run they cannot reproduce.

## Fixed

Kept because they explain why some packages are written the way they are, and because two of them are the shape of the worst defect this code can surface: silent, wrong, and invisible to any test that does not check the data itself.

### A stack-passed aggregate parameter was spilled over the saved frame pointer — fixed with the first full Linux package-test run

*Silent, and it corrupted the caller's frame rather than the callee's own data.* On System V an aggregate wider than sixteen bytes is passed in the caller's stack argument slots, which the caller fills a whole word at a time, and the callee's prologue copies `AlignUp(size, 8)` bytes of it into the parameter's home slot for the same reason. The x86-64 frame planner sized that slot at the aggregate's exact width. A twenty-byte struct therefore got twenty bytes and the spill wrote twenty-four, and what sits four bytes above a parameter home is the low half of the saved `rbp`. `leave` handed the caller back a frame pointer with its low word zeroed — `0x00007fff00000000` where `0x00007fffffffbe70` belonged — so the fault landed in the caller's next store to a local, one frame and one call away from the code that did the damage.

`Rux/Time`'s `OffsetDateTime` is a `DateTime` and a `UtcOffset`: sixteen bytes and four, twenty in total, and the first by-value parameter in this tree whose width is not a whole number of words. `Tests/Packages/Time/Rfc3339` passes one to `Rendered` and was the only test in the workspace to segfault. Found by disassembling the emitted ELF — the binaries carry no symbols, so the faulting function was recovered by scanning for `55 48 89 e5` — and confirmed by reading the three stores of the prologue: `-0x14`, `-0xc`, `-0x4`, the last of which is not inside a twenty-byte slot ending at `-0x1`.

The AArch64 planner had padded every aggregate slot to a whole number of words since it was written; the x86-64 one padded only the widths no scalar move spells — three, five, six and seven — and left everything above eight alone. Both now round an aggregate slot up the same way, which also settles the matching read: the caller filled the outgoing argument slots by reading `AlignUp(size, 8)` bytes back out of that same under-sized slot.

### A wide string literal held its UTF-8 bytes one per code unit, at the byte count — fixed with the built-in string types

*Silent, and wrong in two ways at once, on every target.* `EncodeStringLiteral` widened a literal's UTF-8 bytes into the element's width by writing each byte followed by padding, so `c16"\u{20AC}"` held three UTF-16 code units — 0x00E2, 0x0082, 0x00AC — rather than the one the character is, and the length published beside them counted bytes rather than code units. Nothing in the tree wrote a `c16` or `c32` string, which is why no test saw it; the defect would have surfaced the moment one did, as text that decodes to different characters than it spells. Both are now answered by one transcoder in `Compiler/Unicode`, shared by lowering and by both back ends, so a length is in the code units of the encoding and the data is the text transcoded into it. `Tests/Language/StringLiterals` pins a character from each UTF-8 sequence width in all three encodings, including the surrogate pair.

### An interface call passed a slice by value where the method takes its address — fixed with the cross-platform bring-up

*Silent on Win64, a crash everywhere else, and the single cause of the Format, Io, Json, Toml and Storage failures on every System V and AArch64 target.* A slice parameter is lowered as a pointer to the `{data, length}` pair, and a direct call takes the argument's address to match. The call through a vtable lowered the same argument as the 16-byte value, which System V and AAPCS64 put in two registers — so `StringBuilder::Write` read `data` as the address of the pair and `Copy` walked off whatever bytes sat at the text's first character. Win64 passes a 16-byte value by reference to a copy, which is a pointer either way, so the one platform the compiler was developed on never saw it. Found by symbolizing the faulting frame of `Tests/Language/Format`: `WriteBytes` is the interface hop between `WriteAscii` and the builder. Interface and direct calls now lower arguments through one function.

### An array or tuple literal argument was passed as the address of its slot — fixed with the cross-platform bring-up

*Silent on Win64 and AArch64, garbage on System V x86-64, and the cause of the Crypto failures on Linux and macOS x86-64.* A named variable of array type reaches a call as a `load` of the whole value, but a literal — `Sha512::Start([0x6A09E667F3BCC908, ...], 64)`, or a `const` array, which lowers through its initializer — evaluated to the slot it was built in, and a slot is a pointer. The callee's parameter is the value, and on System V a value wider than sixteen bytes lives in the caller's stack slots: the callee read those slots, which nothing had written, and took the pointer for the scalar after the array, so `outputLength` was a stack address and `Finish` indexed the state by it. Win64 and AAPCS64 pass a wide aggregate by reference to a copy, and a pointer to the original is indistinguishable from that, which is why both were fine. The callee side had been right all along; the argument is now loaded as a value whatever expression produced it, and `Tests/Language/ByValueArguments` puts a scalar after every shape of literal.

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

Drop planning now handles recursive owners, partially constructed aggregates, non-generic variant payloads, generic destructors, and control-flow exits through branches, loops, returns, and `?`. The explicit `JsonValue` and `TomlValue` destructors remain because they state ownership clearly, not because synthesized recursive glue needs a workaround.

### A `const` array crossing a file boundary read garbage — fixed in `f090653`

A `const` array declared in one file of a package and read from another was not matched across objects at link time, so the reference pointed wherever the relocation landed and read plausible garbage — neither its values nor zeroes, and with nothing reported. Scalars were unaffected because they are folded into their use, which is why this survived until `Rux/Hash` published a lookup table.

### A generic container did not consume what was stored into it — fixed in `32d6d48b`

Storing a `T` into a generic container's storage was not recognized as consuming it, so the container kept a copy and the caller's value was destroyed where it stood — a use-after-free for any element owning memory. Consumption is now recorded as a question where it is asked and answered at each instantiation, and methods and associated functions of generic types are queued as instantiations at all, which they never were.

Reference provenance and explicit move operands now answer the public legality question: ownership cannot move through a reference or caller-visible raw pointer merely because the pointee type is movable. Named transfers use `<-`, so source invalidation is visible where it occurs. Generic owning containers have a narrow internal raw-storage transfer rule because the type system cannot yet distinguish their owned allocation from a borrowed raw pointer; the explicit `<-` remains required, while their public borrowed views never transfer ownership.

### A match-arm payload was destroyed twice, then not at all — fixed in `30d464a8` and `85d5d402`

A payload bound by a match arm now owns what it took and is destroyed when the arm ends, and the subject it came out of is not destroyed as well. The two halves landed together: before them, taking a value out of an option destroyed it twice; between them, not at all. Ownership follows the subject — an arm binding is registered for cleanup only where the subject was handed over, so matching a borrowed option still copies nothing and destroys nothing. Reading an aggregate subject straight out of its slot also had to clear the consumption, which was the one path that skipped it.

### Wide variant equality compared only the tag — fixed in `0b58cdd0` and `85dff083`

Variant equality now branches on the active case. Different cases are unequal without reading payload storage; equal cases compare only their active positional or named payloads, recursively and in declaration order. Wide payloads, nested variants, generic substitutions, inactive bytes, and padding therefore no longer depend on the backend's one-word aggregate comparison.

### A generic's interface bound resolved in the instantiator's scope — fixed in `4a96861`

A call from a package that had not imported the interface recorded no conformance — silently, since a use site does not report — and lowering then aborted the compiler with no output at all in a release build. This is why `Rux/Hash` is written with bounded generics, and why that was not possible before it was fixed.

### AArch64 could not open a 512-byte frame or write an aggregate constant — fixed in `64175ef4`

Two backend defects that `rux check --target` cannot see, because the frontend accepts the programs and only code generation fails. The frame record's immediate spans 512 bytes below the stack pointer but only 504 above it, and the limit was taken from the negative reach alone, so a frame of exactly 512 bytes encoded a prologue its epilogue could not close. Separately, an aggregate whose whole value is a literal — a unit variant case, a zeroed structure — had no lowering at all. Found by cross-building the language suite for all seven non-host target cells, which is worth repeating whenever the backend changes.
