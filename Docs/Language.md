# Language Ownership and Lifecycle Contract

This page is the settled contract for Rux values, borrowing, copying, moving, construction, and destruction. The compiler, first-party packages, and positive language examples use this model. Removed ownership spellings are accepted only in negative diagnostic fixtures.

The design favors locally visible ownership effects, explicit signatures, and separate syntax for safe borrowing and raw addresses. Declarations and members remain private by default and use `pub` for public API. A `struct` declares layout, while functions, operators, constructors, destructors, and interface implementations live in `extend` blocks.

## Contextual Expressions

A typed integer initializer, assignment, return, call argument, or aggregate element supplies the required type to both arms of `condition ? first : second`. Unsuffixed integer literals must fit that type; nested conditional arms receive the same context. Each selected arm is materialized at the required width before the values merge, including integers wider than one machine word. The condition runs once and only the selected arm is evaluated.

A `match` expression passes a native expected type to its arms the same way. `none`, `.Success(value)`, and `.Failure(error)` leave part of their type open, so an arm written with one of them takes the optional or fallible type of the annotation, return type, or parameter the match is used for, whichever arm comes first. With no expected type, the first arm whose type is complete decides the match and the open arms before it are checked against that type; a match whose arms are all open needs an annotation. Arms whose types are complete must still agree with each other.

An unsuffixed integer literal used as an operand of an arithmetic, bitwise, or comparison operator takes the integer type of the other operand, on either side: `year % 4 != 0` needs no suffix for an `int32` year. The literal must fit that type, so `count == -1` with an unsigned `count` and `narrow + 300` with a `uint8` are errors rather than a comparison of bits or a wrapped value. A shift amount keeps its own type and is not checked against the shifted operand.

Generic function calls may omit type arguments when the argument types determine them, including parameters behind references, pointers, slices, and named generic types. Explicit type arguments remain available and required when no argument determines a parameter. Inference does not relax mutable-borrow or argument-type requirements.

Struct literals can appear inside parenthesized expressions and call arguments within conditions. An ordinary unparenthesized condition still leaves its following brace to the statement body.

## Return and Deferred Cleanup

A `return expr;` evaluates and preserves `expr` exactly once before running deferred statements. The accepted copy or move into the return value happens during this evaluation. Later changes to source locals do not change the captured value, including fields of an aggregate; a returned pointer still points to its original storage.

On return, registered defers run in reverse registration order, from the innermost active scope outward, followed by the existing ownership cleanup. A transferred value is destroyed by its eventual owner, and a copied source remains subject to normal cleanup. A void return runs the same deferred statements without capturing a value. Each function has its own defer stack, including instantiated generic functions.

## Package Visibility

Every source declaration is package-private unless it starts with `pub`. Package-private means that every file and module in the defining package can use it; it does not mean file-private or module-private. A dependent package can import or name only effectively public API:

```rux
func Helper() {}          // visible throughout this package
pub func Parse() {}       // visible to dependent packages

pub module Text::Utf8 {
    pub func Validate() {}
    func DecodeUnit() {}  // still package-private
}
```

A public item is effectively public only when every module containing it is public. `pub module Text::Utf8` publishes both synthesized path segments. A `pub` declaration below a private module remains package-private. Named, multi-item, and qualified imports report an attempt to import a private declaration and point back to its declaration; a glob import simply omits private declarations. When a function or method name has public and private overloads, cross-package overload resolution sees only the public set, and diagnostics do not reveal the private candidates.

Struct and union fields, methods, associated functions, constructors, and source-level operators also default to package-private:

```rux
pub struct Buffer {
    pub length: uint;
    data: *var char8;
}

pub union Word {
    pub unsigned: uint32,
    signed: int32
}

extend Buffer {
    pub func Buffer() -> Buffer { /* ... */ }
    pub func Length(self: &Buffer) -> uint { return self.length; }
    func Capacity(self: &Buffer) -> uint { /* package helper */ }
}
```

An external struct initializer must be able to name every field, so a public struct with private representation fields is constructed through a public constructor or factory. Enum members and variant cases inherit their type's effective visibility; variant payload fields do not take individual `pub` markers. Interface requirements likewise inherit the interface's visibility. A concrete method may remain private while satisfying a public interface: dispatch through the public interface is allowed, but a direct call on the concrete type is not.

Compiler-generated copy and move operations are available wherever their type is available. A custom copy or move implementation, like any other source operator, must be `pub` for cross-package source use. A canonical bodyless copy or move declaration still prohibits the capability everywhere regardless of visibility. Destructors are invoked by compiler glue regardless of visibility and normally remain private.

Public API signatures must close over public types. An effectively public function, method, constructor, alias, constant, extern, generic bound, field, variant payload, or interface requirement cannot expose a private type through a parameter, return, inferred type, nested generic argument, or bound. Publishing an item does not implicitly re-export the private declarations used by its signature; the compiler rejects the leak instead.

## Enums, Variants, and Unions

`enum` is a named scalar integer type. It declares a closed set of unit members, may choose an integer base type, and may assign integer discriminants. It has no type parameters or payload fields:

```rux
enum Month: uint8 {
    January = 1,
    February,
    March
}
```

Members are values, written `Month::January` or `.January` where context already fixes `Month`. Their discriminants participate in the existing explicit enum/integer conversions and scalar comparisons. Matching an enum selects members and cannot bind data:

```rux
func Days(month: Month) -> int32 {
    return match month {
        .January => 31i32,
        .February => 28i32,
        .March => 31i32
    };
}
```

`variant` is a closed tagged union. A case may be unit-like, carry positional values, or carry named fields, and the declaration may be generic. Its tag is private: source cannot choose a base type or assign case discriminants.

```rux
variant Lookup<T> {
    Found(T),
    Missing
}

variant Shape {
    Point,
    Circle { radius: float64; },
    Rectangle { width: float64; height: float64; }
}
```

Cases are constructed with their declaration name. A unit case may omit `()` where it is already a value; positional and named payloads use the corresponding call or initializer syntax. A match tests the active case before reading its payload, and an exhaustive match covers every declared case:

```rux
func Area(shape: Shape) -> float64 {
    return match shape {
        .Point => 0.0,
        .Circle { radius } => radius * radius * 3.0,
        .Rectangle { width, height } => width * height
    };
}
```

Variant equality is structural. Values with different active cases are unequal without inspecting inactive storage. Values with the same case compare each active payload in declaration order, recursively using that payload type's equality; a variant is not equality-comparable when one of its reachable payloads is not. This applies to wide, nested, generic, tuple, and named payloads and does not compare padding or stale bytes.

Tuples also have structural equality. Both operands are evaluated once, and matching tuple types compare elements from left to right until a mismatch. Element comparisons preserve floating-point equality and declared equality operators; nested tuples, variants, and fixed arrays compare recursively. Every element must support equality, even when an earlier element could differ. `!=` negates the same comparison, and tuples have no built-in ordering.

Structs without declared equality operators compare their fields in declaration order using the same rules, excluding padding. Both operands must have the same concrete type, and every field must support equality. Empty structs compare equal. Declared operators take precedence, including on nested fields; a declared `==` supplies `!=` when no explicit `!=` exists. Structural equality does not supply an ordering. Operator reference parameters borrow direct values and preserve already borrowed arguments.

Copy, move, and destruction are also case-aware. A variant is copyable or movable only when every reachable payload supports the operation. Moving transfers the active payload and invalidates the source. Destruction reads the tag once and destroys only the active payload, in reverse field order, exactly once; partially constructed cases roll back only the payloads already initialized.

The runtime representation is a private tag followed by storage aligned for the widest case. Calls and returns use the aggregate ABI selected for that complete layout; a variant is never passed as only its tag. The compiler keeps the tag width, payload offsets, construction, matching, equality, moves, and drop glue on one layout contract for every target. This representation is compiler-managed, not a substitute for a C ABI declaration.

Both forms are closed, so adding a member or case makes exhaustive matches fail until callers handle it. For an enum, an explicitly assigned discriminant is part of the source-visible numeric contract; append or assign members deliberately when those values are serialized or cross an ABI. A variant's tag numbers are not a source contract and must never be serialized directly. Stable files, protocols, and foreign interfaces should encode an explicit scalar enum and then translate to or from the in-memory variant.

Removing or renaming an enum member or variant case is likewise a source-breaking change. Changing a variant payload changes its value layout and the operations required from that payload, including equality, copying, moving, and destruction. Public library evolution should therefore treat case lists and payload types as part of the API even though the private tag representation is not.

`union` remains the explicit overlapping-storage type. It has fields rather than cases, stores no tag, performs no active-case check, and is appropriate for foreign or manually tracked layouts. Use `variant` when the compiler must know which alternative is active; use `union` only when the program or an external ABI owns that invariant.

The source cutover is intentionally narrow. Scalar enums retain their syntax, discriminants, conversions, and representation. First-party payload types that used the compiler's default enum layout migrate directly to `variant`. External payload enums that exposed an integer base or assigned discriminants are not source-compatible: because a variant's tag is private, those declarations require an explicit redesign, usually a scalar wire enum plus a separate payload type or a manually controlled union.

## Native Sums, Optionals, and Fallibles

Four compiler-owned type forms describe outcomes without a library declaration. Each has one purpose; the design decisions behind them, and the migration from the retired `Result`/`Option` protocols, are recorded in [Native Sums, Optionals, and Fallibles](NativeOutcomes.md):

| Form     | Meaning                                                                                        | Inspection                                                                    |
| -------- | ---------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| `A \| B` | A _sum_: one value of a set of distinct types.                                                 | Typed patterns, qualified case patterns, `is`.                                |
| `T?`     | An _optional_: a present `T` or absence. `T??` keeps both levels.                              | `value?`, `.Some(pattern)`, `none`, typed presence patterns, `is`, `?`, `??`. |
| `T ! E`  | A _fallible_: a successful `T` or a failed `E`, even when `T` and `E` are the same type.       | `.Success(pattern)`, `.Failure(pattern)`, `catch`, `?`, `? else`.             |
| `()`     | The _unit_, the empty tuple, whose only value is also written `()`. `! E` is exactly `() ! E`. | The pattern `()` or a binding.                                                |

`variant` remains the form for alternatives distinguished by case names, and `union` the untagged overlapping storage. In a type, the postfix `?` binds tightest, then `|`, then `!`, so `int32 | bool ! IoError` is `(int32 | bool) ! IoError`. A suffix never reaches across `!` or `|`: an optional error is written `T ! (E?)` and an optional result `(T ! E)?`, an optional member `A | (B?)` and an optional sum `(A | B)?`, and a nested fallible `(T ! E1) ! E2`. As for every postfix suffix, `*T?` is a pointer to an optional and `(*T)?` an optional pointer, and `int32?[..]` is a slice of optionals while `int32[..]?` is an optional slice.

A sum is a set of resolved types: aliases are resolved, nested sums flattened, duplicates removed, and members ordered canonically by their fully qualified identity, so `A | B`, `B | A`, and `A | A | B` are one type, and `A | A` is `A`. Flattening never crosses an optional or fallible level. A sum over type parameters is normalized again for each instantiation, so `T | U` at `T = U = int32` is `int32`. Optionals and fallibles never collapse: `int32??` distinguishes absence, a present absence, and a present value, and an inner failure held as a success is data, not a failure of the outer level. Error payloads need no base type; a failure holding `none` is still a failure.

Native forms are ordinary values in parameters, locals, fields, containers, and each other. They cannot refer to themselves, so recursive data uses a named `variant` or `struct` holding a pointer, which may be an optional pointer `(*Node)?`. They are not `extend` targets and implement no interface, so `int32?` satisfies no `Display` bound; reusable operations are generic functions that spell the forms, such as `func ValueOr<T, E>(value: T ! E, fallback: T) -> T`, and inference descends through `T?`, `T ! E`, and `&(T ! E)` as through named generic types. An alias names its resolved type and hides nothing: a public `type ReadError = ParseError | IoError;` exposes both members to exhaustive callers, while a named error variant keeps an explicit boundary and can carry the original cause.

### Construction and conversion

`.Success(value)`, `.Failure(error)`, and `.Some(value)` construct a level directly, and `none` is the absence of the expected optional's outer level, so every nested state is written without temporaries: `let stored: int32?? = .Some(none);`. A native constructor needs an expected type for every part it does not fix, so `let value = none;` and `let result = .Success(1i32);` are rejected with the annotation to write; `.Some(value)` alone takes its payload's type.

Where a value meets an expected native type, identity wins; otherwise exactly one route must apply, made of member injection, subset widening of a sum, success or presence construction, and the same widening applied within one fallible channel or one optional payload. `A?` widens to `(A | B)?` and `T ! A` to `T ! (A | B)`, but no route converts a payload, extracts a value, or constructs a level inside an existing one. When two routes give different meanings, the conversion is ambiguous and must be written out. With `R = int32 ! ParseError`, `return value;` in a function returning `(int32 | R) ! ParseError` could keep a failed `R` as successful data or fail the outer level with it, so it is rejected: `return .Success(value);` keeps it and `return value?;` forwards it. An unsuffixed integer or float literal targets a sum only when exactly one member is of its kind, independent of its value, so both `5` and `300` are ambiguous for `uint8 | int32`; every other literal is injected only when its own type is a member.

`fail error;` returns the enclosing function's failure with `error` converted to its error channel and is rejected outside a fallible function. `return;` and falling off the end produce `.Success(())` only when the success type is exactly `()`; a type that merely contains the unit, such as `()?` or `() | X`, needs a value. An omitted return type still declares a void function, which is not a unit-returning one, and a library struct named `Unit` would be an ordinary type unrelated to `()`. `Main` may return `! E` or `int ! E`: a success exits with its integer payload, or 0 for the unit, and a failure runs ordinary cleanup and exits with status 1 without printing the error.

### Matching

An ordinary `match` inspects native values one level at a time:

```rux
func Describe(outcome: (Options | Defaults) ! (DecodeError | IoError)) -> int32 {
    return match outcome {
        .Success(options: Options) => options.verbose ? 2i32 : 1i32,
        .Success(_: Defaults) => 0i32,
        .Failure(DecodeError::InvalidDigit(position)) => position,
        else => -1i32
    };
}
```

`.Success(p)` and `.Failure(p)` select a channel, and `.Some(p)` and `none` an optional level. The presence suffix `p?` is exact shorthand for `.Some(p)`, so `value??` binds the payload of two levels and `none?` matches a present absence. A typed pattern `v: T` selects the sum member `T` and binds it, a subset `v: A | B` binds the matched members as a smaller sum, and on an optional subject the typed presence pattern selects one present level whose payload has that type; on a fallible subject it must name the whole type. `_: T` selects without binding. A qualified case pattern `Type::Case(...)` selects the variant member and its case at once, and is ambiguous when two members are instantiations of the same variant; an unqualified `.Case` on a sum subject is rejected in favour of `Type::Case`. Guards and the unit pattern `()` work as elsewhere.

An identifier pattern binds only a free name. A name that already resolves to a type, alias, generic parameter, enum, variant, interface, constant, function, or module, or that spells a case of the subject or of one of its members, is rejected rather than silently binding the whole subject: `Options =>` on `Options | Defaults` and `Missing =>` on a `DecodeError` channel are errors whose help names `options: Options`, `Options { ... }`, or `DecodeError::Missing`. Shadowing a local variable or parameter remains legal.

Coverage is checked over every level, so a missing `.Failure(_: IoError)` or `.Some(none)` is named in the diagnostic. An arm covered entirely by earlier unguarded arms is unreachable, but an `else` arm never is: it covers whatever remains, possibly nothing, which keeps `x: T => ..., else => ...` valid when a generic `T | U` collapses at `T = U`. Patterns that depend on type parameters are rechecked for each instantiation and report conflicts there, with both locations.

A match consumes an owned subject, so arms that bind payloads own them and the payload of an arm that binds nothing is destroyed there. A borrowed subject is matched in place: bindings alias the payloads where they lie and a subset binding reads the original tag through the borrow. Such a view can be read and matched but not borrowed again, passed by reference, stored, or moved, and only a single-member binding writes through. `==` and `!=` compare native values structurally: the tags of each level first, then the active payloads.

### Discarded results

A fallible that is dropped loses its failure, so an expression statement producing a fallible, a bare-expression arm of a match statement that produces one, and `let _ = outcome;` are errors, and a fallible local that nothing reads is a warning. These are practical checks rather than a proof of handling: reading, passing, storing, or overwriting a fallible counts as a use, and nothing is followed through fields, containers, or later writes. A match covering both channels with `.Success(_) => {}` and `.Failure(_) => {}` discards a fallible deliberately, and `catch { else => {} }` does so for a unit success only.

## Membership Tests

`value is Type` answers a question about the operand's type and never consumes, binds, or narrows it: after the test, `value` keeps its static type, and extracting a payload still takes a pattern. The tested type is a postfix type, so a sum or fallible target is grouped, as in `value is (A | B)`; an ungrouped `value is A | B` whose right operand names a type is rejected with that grouping as the fix. The right-hand type is resolved first, and its form and the subject's select the mode:

| Subject                             | Meaning                                                                                                                                     |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| A sum `A \| B \| C`                 | The active member is the tested type, or one of the members of a tested subset sum. A type that is not a member or subset is rejected.      |
| An optional `T?`                    | The value is present and its payload is the tested type, or a member or subset of a sum payload. Only one optional level is ever inspected. |
| A fallible `T ! E`                  | Rejected; a channel is matched with `.Success(...)` or `.Failure(...)`.                                                                     |
| Any other type                      | Exact type identity after alias resolution. A test that can never be true is a compile error, never a constant `false`.                     |
| An interface on the right-hand side | Reported as unavailable: interface implementation tests are not implemented, and no native form implements an interface.                    |

A reference subject is tested through the reference without being consumed. A test whose subject or tested type mentions a type parameter is checked again for each instantiation, so `x is T` over `T | U` stays valid when `T` and `U` are the same type and the subject collapses to that type.

Membership is not interface implementation: knowing that every member of a sum implements an interface, or that a test succeeded, makes no shared member available on the sum.

## Bindings and Parameters

Bindings have three forms:

```rux
const MaxSize = 1024;
let size = 10;
var index = 0;
```

`const` is a compile-time value, `let` is an immutable runtime binding, and `var` is a mutable runtime binding. Parameters always use `name: Type`; binding mutability is not written before a parameter name. A method is a function whose first parameter is named `self`, and that receiver has the same explicit type syntax as every other parameter:

```rux
func Read(self: &Buffer)
func Clear(self: &var Buffer)
func Consume(self: Buffer)
```

The removed `var value: T` parameter form is an error. A function that needs mutable local storage moves the parameter into a local with `var local <- value`; a function that mutates caller-owned storage instead accepts `value: &var T`.

A parameter's default value is evaluated where the call is written, in the caller's scope, each time the argument is omitted. It therefore cannot read any parameter of its own function, `self` included: `func C(x: int, y: int = x)` is an error. An overload that passes the value, `func C(x: int) -> int { return C(x, x); }`, gives the same call.

When several overloads accept a call, declaration order never decides it. An overload that is no worse for any argument and better for one wins: an argument of exactly the parameter's type is better than one differing only in a view's writability, which is better than one reached through a borrow or a scalar read, which is better than any other conversion. Among overloads still tied, the one the arguments fill without a default value wins, so `D(2)` calls `func D(x: int)` rather than `func D(x: int, y: int = 1)`, and then a function that is not generic wins over a generic one. Overloads still tied after that make the call ambiguous, which is an error.

Members are always named through their value, such as `self.length`; a receiver never enables implicit field lookup. `Self` remains available only where an interface must name the unknown concrete implementing type.

### Attributes

An attribute is a `#Name(...)` call written before what it describes. Before a declaration, `#Abi(.Win64)` selects a calling convention, `#Link("library")` describes how an extern declaration is imported, `#NoReturn()` marks a function that never returns to its caller, `#Allow("naming.type")` suppresses one lint rule for that declaration, and `#Error("message")` and `#Warn("message")` report at each use. `#Format()` is the one attribute written before a parameter:

```rux
pub func PrintLine(#Format() format: char8[..], args: Display...) -> IoError?
```

It marks a format string: a `char8[..]` parameter followed by a variadic parameter whose arguments fill its placeholders. When a call passes a string literal for it, the compiler counts the literal's placeholders — `{}` and `{:spec}`, with `{{` and `}}` as literal braces — against the variadic arguments, and a mismatch is an error such as `format string has 2 placeholders, but 1 argument was provided`. A format that is not a literal, a spread argument, and a pattern that is itself malformed are left to the formatter's run-time check. The attribute is all the compiler knows: it recognizes no package or function by name, so any function taking a format string can declare one. `#Format()` takes no arguments, appears at most once per parameter list, and cannot be applied to the receiver or to the variadic parameter itself.

## Values, References, and Raw Pointers

The five storage-facing type forms are:

```text
T        owned value
&T       immutable reference
&var T   mutable reference
*T       raw read-only pointer
*var T   raw writable pointer
```

References are non-owning, non-null, automatically dereferenced for member and index access, and cannot perform pointer arithmetic. They may be parameters, receivers, or local aliases. They cannot be fields, returned values, or otherwise stored beyond the current call. Any number of immutable borrows may coexist, or one mutable borrow may exist exclusively; a borrow ends after its last use. A reference cannot destroy its referent or move ownership out of it. A concrete value may be borrowed directly as an immutable or mutable interface view without copying or consuming the value.

A reference to a Copy primitive scalar can supply its value in a typed assignment, return, call, cast, arithmetic expression, comparison, or boolean condition. The compiler loads the scalar without moving ownership or changing the reference's type. Passing the same expression to a reference parameter preserves the borrow, and mutable borrows keep their usual exclusivity rules. Arrays and tuples can hold scalar copies read from references, but aggregate referents are never implicitly copied. Generic scalar reads are checked for every instantiated type.

A name bound to a reference to a Copy primitive scalar — a parameter, a receiver, or a `let` alias — is also a place to write: through `count: &var int`, `count = value`, `count += 1`, `count++`, and `--count` store into the caller's storage, so `func Bump(count: &var int) { count += 1; }` increments the argument. The binding itself is never rebound by such a write, and a write is an exclusive access under the usual borrow rules. Through `&T` the same write is an error naming the reference. A `var` binding of reference type is the one exception for plain `=`, which points it at other storage; compound assignment and `++`/`--` still write through it. Aggregates are written through their fields and elements, and a reference is never written with `*`, which stays a raw-pointer operator.

Interface method arguments follow the same conversions as ordinary function arguments. A concrete local or concrete reference can supply a borrowed interface argument directly; an existing interface view retains its data and vtable. Mutable interface arguments require writable storage. Interface requirements may provide typed default arguments and variadic interface arguments.

An interface requirement states through its receiver what it does with the implementing value. A requirement with no receiver, or with `self: &Self`, only reads it; a requirement that writes it declares `self: &var Self`, and a requirement receiver naming any other type, such as the interface itself, is an error. An implementation may read where its requirement permits a write, but a method declaring `self: &var T` cannot satisfy a read-only requirement, because a read-only view would then modify storage its caller only lent for reading. A call of a writing requirement needs a writable receiver exactly as a concrete method with `self: &var T` does: it is accepted through `&var Gauge`, a `var` interface value, or a `&var T` bounded by the interface, and rejected through `&Gauge`, a `let` interface value or parameter, or a `&T`. An interface value holds its own copy, so writing through it leaves the value it was made from unchanged. The receiver is the data half of the view, so naming `Self` there does not restrict where the requirement can be called.

Inside `func Render<T: Display>(value: T)`, the declared bound allows `value` to be passed to a `Display` parameter, including a variadic one. Bounds also permit borrowed interface arguments with the usual mutability restrictions. A primitive satisfies a nonempty interface bound only when it has a matching implementation; built-in comparison operators do not implicitly implement `Core::Comparable`.

Raw pointers remain the deliberate mechanism for FFI, nullable or sentinel values, stored addresses, pointer arithmetic, and unsafe memory APIs. Address-taking is explicit with `@value`. Moving a value out through a raw pointer is rejected because the pointer does not own the storage it addresses.

### First-party raw-pointer boundaries

The package surface keeps raw pointers only where the address itself is part of the contract:

- `C` and the platform binding packages mirror foreign ABIs, including nullable arguments, buffers, handles, and operating-system-owned storage.
- `Memory` and `Allocator` traffic in untyped blocks, alignment, address arithmetic, and ownership transfers that begin or end at an allocation boundary.
- `Core` slices store an address and a length, and nullable `Try*` output slots use null to mean that no destination was supplied.
- `Collections` stores backing blocks, node links, and iterator positions. It uses references for safe call-time access and raw addresses only for stored or ownership-aware internal places.
- `Io`, `Storage`, `Json`, and `Toml` retain raw handles when a stream must be stored in a field; a non-escaping interface reference cannot represent that lifetime. Ordinary calls borrow streams and values through references.

Package READMEs identify these retained boundaries beside the APIs that expose them. A new safe parameter or receiver uses a reference unless it has one of the address-level contracts above.

## Strings and Text

### Intrinsic declarations

An intrinsic declaration binds a source API to a compiler representation. It does not load a package or give its declaring package special privileges. Core supplies the standard declarations; another package can supply its own.

```rux
pub intrinsic type int8;

extend int8 {
    pub const Min: int8 = -128i8;
    pub const Max: int8 = 127i8;
}
```

Scalar representation and arithmetic exist independently of these declarations. Associated constants are source members: `import Core::int8;` exposes Core's public constants, while merely depending on Core does not. Constants retain normal package visibility. Their initializers are checked in the declaring package, not the caller's scope.

`intrinsic type` is reserved for implemented scalar types. Slices and ranges are native types, with no source declaration controlling their representation. An ordinary struct never acquires compiler-owned behavior from its name.

Extension blocks may contain ordinary typed constants as well as methods. The only compiler-supplied associated constant values are `intrinsic const Infinity: float32;` and `intrinsic const NaN: float32;`, and their `float64` counterparts, declared inside extensions of the corresponding floating-point type. Other limits and metadata belong in ordinary source initializers.

### Writable sequence views

`T[..]` is a read-only slice and `var T[..]` is a writable slice. Both contain a data pointer and an element count. The storage must outlive the view. Copying a view copies its descriptor, not its elements.

```rux
var values: int[3] = [1, 2, 3];
let writable = values[..];
writable[1] = 4;
let shared: int[..] = writable;
let empty: var int[..] = [];
```

Writability belongs to the view: a `let` binding can write through a writable view, while a `var` binding of a read-only view cannot write its elements. Writable slices implicitly weaken to read-only slices; the reverse is rejected. Array views inherit the writability of the array place, and sub-slices preserve their parent's writability.

A pointer can form a slice with `p[..n]`, `p[a..b]`, or an inclusive end such as `p[..=n]`. Pointer views inherit pointee writability. A pointer has no stored length, so `p[..]` and `p[a..]` are rejected. Slice indexing and iteration use element counts, including when elements occupy more than one byte. Empty slice literals have null data.

### Literal encodings

String literals are read-only character slices. They need no package import, and `.data` and `.length` are compiler-owned.

```text
"Hello"       char8[..]    UTF-8
c8"Hello"     char8[..]    UTF-8
c16"Hello"    char16[..]   UTF-16
c32"Hello"    char32[..]   UTF-32
```

Each descriptor occupies sixteen bytes, aligned to eight, with `data` at offset zero and `length` at offset eight. Slices are passed by address on every supported ABI. Literal storage includes a trailing NUL code unit; its length excludes that terminator. External callees taking a raw pointer receive `"literal".data` explicitly.

Lengths and indexing count code units. A supplementary Unicode scalar occupies four UTF-8 units, two UTF-16 units, or one UTF-32 unit. Slicing and iteration operate on those units, so a sub-slice can split an encoded scalar. Use the Text and Unicode packages when an operation requires validated text, scalar iteration, or grapheme boundaries. An arbitrary character slice does not promise valid Unicode. Literal bytes are read-only and cannot become a writable character slice.

```rux
func First(text: char8[..]) -> char8 {
    return text[0];
}

func Prefix(text: char8[..], count: uint) -> char8[..] {
    return text[..count];
}
```

### Character literals

Character literals hold one value. An unprefixed `'A'` has type `char32`; `c8'A'`, `c16'A'`, `c32'A'`, and `c64'A'` select their respective character widths. `char64` stores the same Unicode scalar values as `char32` in eight bytes, so `c64'😀'` and `c64'\u{1F600}'` have the same value. Prefixes work in constants, generic arguments, and match patterns. An identifier named `c64` remains valid; there is no `c64` string encoding.

Character literals accept the existing simple escapes and `\u{...}` with one to eight hexadecimal digits. Empty literals, multiple characters, invalid escapes, surrogate scalars, and scalars above U+10FFFF are rejected. Character patterns compare decoded values, so a Unicode escape matches its literal spelling and equivalent unguarded arms are duplicate patterns.

### Range types

Range expressions and annotations use the same punctuation family. Bounds in a two-sided range have one type.

| Expression | Type        | Members          |
| ---------- | ----------- | ---------------- |
| `1..4`     | `int..int`  | `.start`, `.end` |
| `1..=4`    | `int..=int` | `.start`, `.end` |
| `1..`      | `int..`     | `.start`         |
| `..4`      | `..int`     | `.end`           |
| `..=4`     | `..=int`    | `.end`           |
| `..`       | `..`        | none             |

Range members are compiler-owned and need no import. Inclusive ranges include their final bound; ordinary two-sided ranges exclude it. The full range selects an entire array or slice. A pointer requires an explicit end.

Concrete extensions such as `extend int[..]` and `extend char8[..]` have separate method sets. Generic slice extensions such as `extend T[..]` are rejected; generic reusable algorithms remain ordinary functions.

## Copy and Move

Copy and move are different operations:

```text
=     copy or copy assignment
<-    move or move assignment
```

A named source is copied unless the source expression is prefixed by `<-`. A named move-only source in any by-value context is therefore an error without `<-`; the diagnostic shows the transfer syntax for that binding, argument, return, assignment, aggregate, or conditional arm. A fresh temporary transfers directly because no visible source remains afterwards:

```rux
let copy = value;
let moved <- value;

Consume(value);          // copy
Consume(<- value);       // move
Consume(MakeValue());    // direct temporary transfer
```

Copying never invalidates its source. Moving invalidates the source, suppresses its later destruction, and makes every subsequent read a compile error. Assignment to a live destination releases that destination's old state at the operation's defined replacement point. Initializing previously uninitialized storage performs no prior destruction.

Copy and move capabilities are structural. An absent special operation asks the compiler to generate the operation when every field supports it. A declaration with a body supplies a custom implementation. A canonical declaration without a body prohibits that compiler-generated operation:

```rux
extend File {
    func =(self: &var File, other: &File);
}

extend Pinned {
    func =(self: &var Pinned, other: &Pinned);
    func <-(self: &var Pinned, other: Pinned);
}
```

The canonical copy prohibition is `func =(self: &var T, other: &T);`. The canonical move prohibition is `func <-(self: &var T, other: T);`. Bodyless ordinary functions in interfaces remain interface requirements rather than prohibitions.

A custom `=` writes a new state into compiler-provided scratch storage and cannot consume its source. Copy assignment first produces that new state, then destroys the old destination and installs the result. A custom `<-` consumes its source under the same source-invalidating rule as a generated move. Resource-owning types must explicitly prohibit copying unless they implement a real independent copy.

## Construction and Initialization

An associated or instance method may declare its own type parameters, including on a non-generic type. For example,
`Layout::ForValue<int32>()` selects a method type argument; a method with a value parameter can infer it from the
argument. Instance calls such as `holder.Convert<int64>(value)` take receiver parameters from `holder` independently.
For associated calls on a generic type, write the receiver arguments first and then the method arguments, as in
`Holder::OtherWidth<int32, int64>()`. Method bounds are checked at the call and concrete instantiations retain their
own parameter, result, and symbol identities.

Inside `extend T`, a receiverless function named `T` that returns exactly `T` is a constructor candidate:

```rux
extend String {
    func String() -> String {
        return String { data: null, length: 0 };
    }

    func String(value: char8[..]) -> String {
        // allocate and copy
    }
}
```

Constructors are called as `String(...)` or `Vector<int32>(...)`, not through an implicit conversion. Infallible same-type `New` factories migrate to this form. Fallible factories returning `T?` or `T ! E` and descriptive factories such as `NewKeyed` or `New128` are not constructors and may keep their names.

`var value: T;` invokes `T()` when an accessible default constructor exists. With no `T()`, the declaration remains legal but denotes compiler-tracked uninitialized storage. Reading or destroying that storage before definite initialization is an error. Construction and assignment are distinct: a constructor creates a value, while `=` replaces or initializes storage according to its current state. Constructor lookup is never a general hidden conversion rule.

### Repeated fixed-array initialization

`[value; count]` constructs a fixed inline array by evaluating `value` exactly once and copying the result into every element. `count` must be a non-negative compile-time integer. The expression has type `T[N]`, so both the element type and extent can be inferred, while an explicit destination type contextually types the repeated value:

```rux
let inferred = [0u8; 16];
let contextual: uint8[16] = [0; 16];
```

The element type must be copyable. Construct move-only elements explicitly or initialize mutable array storage in a loop instead. A zero count still evaluates the value once and destroys the temporary when its type requires cleanup. Repeated arrays use the same fixed-array-to-`T[..]` coercion as ordinary array literals.

## Failure Propagation

`outcome?` evaluates its operand once, extracts its success payload, or returns its failure from the enclosing function. It processes exactly one outer level: whatever the success holds continues unchanged, including an inner fallible, sum, optional, or the unit.

On a native fallible `T ! E`, the enclosing function must itself be fallible, `-> U ! F`, and the error leaves as its outer failure. The error enters `F` only by identity, by injection as a member of a sum `F`, or by widening a sum `E` whose members are all members of `F`; `?` never converts an error, so a different error type is mapped explicitly or matched. The failure exit chooses the outer failure channel directly, so it never competes with the success construction a plain `return` might perform: in a function returning `(int32 | R) ! E` where `R = int32 ! E`, `return value?;` forwards an inner failure while `return .Success(value);` keeps it as data. An enclosing optional or a nested level is not a propagation target; deeper handling is written with `match`.

A failure propagates only from a native fallible and absence only from a native optional. A variant with `Success` and `Error` cases, or with `Some` and `None` cases, is an ordinary variant whatever its declaration is called: `?` rejects it with a note naming the native form, and its cases are constructed and matched like any other.

Propagation consumes its evaluated outcome. A named copyable outcome is copied and remains usable; a named move-only outcome requires `(<-outcome)?`, and a borrowed fallible is never an operand. The continuing payload and the outgoing error are transferred using their move operations. Reference payloads cannot preserve hidden borrow provenance, and payloads that prohibit moving are rejected, including when instantiated in a generic. Use an explicit match when the payload needs to be borrowed or copied instead.

A propagated failure follows ordinary return behavior: it captures the failure before running registered defers in LIFO order and then performs ownership cleanup. The returned payload belongs to the caller; live locals and completed components of interrupted aggregate construction retain their normal cleanup.

## Contextual Error Mapping

`value? else (e => mapper)` propagates like `value?` but converts the error first. It applies only to a native fallible in a fallible function. On success the mapper never runs and the success continues unchanged. On failure `e` owns the complete error payload, including every member of an error sum, and the mapper runs once; its value must convert to the enclosing function's error channel, and the operation then fails with it:

```rux
func LoadConfig(path: char8[..]) -> Options ! ConfigError {
    let value = ReadAt(path)? else (e => ToConfigError(e, path));
    return value;
}
```

The parentheses delimit one binder and one expression. The binder may be `_` only when the error is copyable; a move-only error is bound and transferred, as in `(e => Wrap(<-e))`. The body is a single expression: a `match` maps different members differently, and `fail`, `return`, or a call to `Panic` leaves directly. A mapped value is never propagated implicitly, so a mapper that returns a fallible is a type error rather than a second propagation, and the mapper's own `fail` or `?` leaves the function without being mapped again.

The mapper is not a closure: it reads and moves surrounding locals under the ordinary rules, on a path that always leaves through the failure exit. A local moved only inside the mapper therefore stays owned on the continuing path; the move only keeps that local from being destroyed again on the exit the mapper takes.

## Postfix Recovery

`outcome catch { arms }` recovers from the failure of exactly one outer fallible level. The subject is evaluated once; a success passes through unchanged, inner levels and all, and a failure is matched by the arms, whose patterns see only the error payload:

```rux
func LoadWithDefaults() -> Options | Defaults ! IoError {
    return Read() catch {
        _: ParseError => Defaults {},
        e: IoError => fail e
    };
}
```

The arms use ordinary match-arm grammar, including guards and an `else` arm, and must cover every error value. Each arm either produces a value that converts to the success type or leaves through `fail`, `return`, `break`, `continue`, a call to `Panic`, or a no-return function; a leaving arm never decides the result type. A block arm completes with `()`, so `Close(file) catch { else => {} };` is valid only for a unit success; a non-unit success never acquires an invented fallback value. An arm's own `fail` or `?` leaves the enclosing function normally and is not caught again.

`catch` binds tightly to the postfix expression before it, so `a + F() catch { ... }` recovers `F()` alone, and a chain continues after the closing brace. It consumes its subject: a named copyable subject is copied, a named move-only subject is transferred as `(<-outcome) catch { ... }`, and a borrowed fallible is never a subject. An arm that binds the error owns it, so an error payload that prohibits moving cannot be bound by value. A `match` expression may be the subject, as in `let value = match key { ... } catch { ... };`; a match statement takes no postfix operator.

## Option Coalescing

### Optional propagation

On a native optional `T?`, `value?` removes exactly one presence level: a present value continues as its payload, which may itself be an optional or a fallible, and absence returns from the enclosing function. The enclosing function returns either an optional `U?`, which then returns `none`, or a fallible whose success is an optional, `U? ! F`, which then succeeds with `none`. Absence never becomes an invented error, and no deeper level is a target. As with fallible propagation, the operand is consumed, a move-only operand is transferred as `(<-value)?`, and a borrowed optional is never an operand. The complete active payload continues, including nested and generic payloads and zero-sized values.

### Coalescing

`option ?? fallback` takes the payload of one native optional level, or evaluates `fallback` when it is absent. The fallback is evaluated lazily and must convert to the payload type, or leave: `candidate ?? fail NotFound {}` turns absence into a failure, and `?? return`, `?? break`, `?? continue`, and a call to `Panic` are accepted the same way. A present absence is a value: with `nested: int32??`, `nested ?? fallback` yields the inner `int32?`. `??` operates only on an optional; a fallible operand is rejected because coalescing would silently discard its error, and a borrowed optional is never an operand.

### Evaluation and ownership

The left operand is evaluated exactly once. The fallback is checked at compile time but evaluated only for absence, so it may perform expensive work or visible side effects without paying that cost when a value is present. A variant with `Some` and `None` cases is not an optional and is rejected as a left operand, as is a raw pointer; a raw pointer may still be the payload of an optional. Reference payloads are not supported because extracting one would lose its borrow provenance.

Coalescing consumes its evaluated optional. A named copyable optional is copied and remains usable, while a named move-only optional must make the transfer visible:

```rux
let value = (<-ownedOption) ?? <-ownedFallback;
```

The fallback transfer is conditional, so `ownedFallback` is possibly moved after this expression. `??` associates to the right. Logical `||` binds more tightly and `?:` binds less tightly, so `first ?? second ?? fallback` means `first ?? (second ?? fallback)`. The operator is compiler-owned control flow: it cannot be declared in an `extend` block or overloaded, and there is no `??=` form. Because `??` is a single maximal-munch token, two adjacent postfix propagations must be written `(nested?)?` instead of `nested??`.

## Iteration

`for item in subject` iterates an array, a slice, or a range directly. Any other subject is driven by the iterator convention: an iterator declares `func Next(self: &var Iterator) -> Item?`, and a container declares a parameterless `Iterate` returning such an iterator, so two loops over one container advance independent iterators. Each iteration calls `Next` once; a present result continues the loop with its payload, and only the outer absence ends it. An item may itself be an optional, a fallible, a sum, or the unit: an absent or failed item is an ordinary value of the loop, and no error channel is added to iteration. A `Next` returning a variant with `Some` and `None` cases is rejected where it is declared, because no loop reads such a variant.

## Indexing

A type indexes itself by declaring the indexing operators in an `extend` block, the same way it declares `==` or `<`. `[]` reads one element and `[]=` writes one:

```rux
struct Vect {
    data: int[10];
}

extend Vect {
    func [](self: &Vect, index: uint) -> int {
        return self.data[index];
    }

    func []=(self: &var Vect, index: uint, value: int) {
        self.data[index] = value;
    }
}
```

`v[i]` on such a type is a call to `[]`, and `v[i] = x` is a call to `[]=`. Each has exactly one shape, checked where it is declared: a read takes `self: &T` and one index and returns the element; a write takes `self: &var T`, the index, and the new value, and returns nothing. Neither takes type parameters, variadics, or default arguments. Arrays, slices, and pointers keep their built-in indexing, which no `extend` block can displace or redefine; the operators are reached only for a type the language does not index on its own.

Nothing fixes the index type. Overloads of either operator are separated by it, so one type may answer several index forms, including a range:

```rux
extend Vect {
    func [](self: &Vect, span: int..int) -> int[..] {
        return self.data[span];
    }
}
```

The two are independent. A type may declare only `[]` and be read but not written, only `[]=` and be written but not read, or both. Declaring both does not make the pair act as one: neither operator reads and writes on its own, and combining them would evaluate the receiver and the index twice, so `v[i] += x`, `v[i]++`, and `v[i]--` are rejected in favour of writing the read and the write out. A field of a read result is not a place either — `[]` returns a value, and a reference cannot be returned, so there is no place-returning indexer — which leaves `v[i].field = x` and borrowing `v[i]` as a `&T` rejected as well.

Either call borrows its receiver whole for the duration of the call, exactly as a method call does: `[]` reads it, and `[]=` needs a receiver it may write, so a `let` binding or an access through `&T` is rejected. Both therefore conflict with an exclusive borrow of any part of the receiver, and a read never counts as a partial move out of it — the result is a fresh value that transfers on its own. The value written by `v[i] = x` reaches `[]=` as an ordinary by-value argument, so it copies or, with `v[i] <- x`, moves.

Only the assignment's own target is written. In `v[keys[j]] = x`, the subscript on `keys` is a read through `[]` and only the outer index writes.

## Destruction

A destructor is a body-bearing special function named after its type with a leading `~`:

```rux
extend String {
    func ~String(self: &var String) {
        Free(self.data);
    }
}
```

The compiler invokes it exactly once for each initialized value that still owns its state, then destroys contained fields, elements, and payloads in reverse construction order. Destruction runs at ordinary scope exit, replacement, `return`, `break`, `continue`, and failure propagation through `?`. A moved-from or never-initialized value is not destroyed. Panic and process termination do not unwind.

`Core::Drop` is not part of the language or Core package. Lifecycle cleanup is expressed only by `~Type`, and an ordinary interface or method named `Drop` has no compiler-defined ownership meaning.

## Final Ownership Boundary

Implicit consumption of named move-only values, mutable-parameter prefixes, exact-type forwarding `New` wrappers, and `Core::Drop` are removed. Parser, semantic, golden and integration tests preserve rejection of these removed forms; source conventions are documented rather than scanned. Fallible and descriptive `New*` factories, interface `Self`, and deliberately raw pointer APIs are permanent parts of the language and packages rather than compatibility syntax.
