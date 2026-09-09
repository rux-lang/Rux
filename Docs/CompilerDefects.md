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

### A method with its own type parameter on a non-generic type does not resolve at the call site

*Loud.* Found in `Rux/Memory`; `Layout::ForValue<T>` had to become the free function `LayoutOf<T>`.

### Equality on a multiword struct or tuple compares only its leading doubleword

*Silent.* `==` between two aggregate values wider than one register still loads the first eight bytes of each side and compares those unless the frontend supplies a structural operation. The AArch64 backend refuses a tuple comparison rather than quietly answering from its first element, while x86-64 silently does the latter. Variants already use case-aware structural equality.

### System V x86-64 loses the tail of a 9–15-byte aggregate

*Silent.* A by-value named struct whose runtime size is 9–15 bytes travels as a single register on System V x86-64. Caller and callee agree on that classification, but the tail beyond the first eight bytes is lost. Field padding makes these sizes uncommon; it does not make dropping their data correct. Argument placement, callee spills, affected returns, and textual assembly must preserve the complete value, including when argument registers are exhausted.
