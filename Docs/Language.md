# Language Ownership and Lifecycle Contract

This page is the settled contract for Rux values, borrowing, copying, moving, construction, and destruction. The compiler, first-party packages, and positive language examples use this model. Removed ownership spellings are accepted only in negative diagnostic fixtures.

The design favors locally visible ownership effects, explicit signatures, and separate syntax for safe borrowing and raw addresses. Declarations and members remain private by default and use `pub` for public API. A `struct` declares layout, while functions, operators, constructors, destructors, and interface implementations live in `extend` blocks.

## Contextual Expressions

A typed integer initializer, assignment, return, call argument, or aggregate element supplies the required type to both branches of an `if` value, `if condition { first } else { second }`. Unsuffixed integer literals must fit that type; nested `if` values receive the same context. Each selected arm is materialized at the required width before the values merge, including integers wider than one machine word. The condition runs once and only the selected arm is evaluated.

A `match` expression passes a native expected type to its arms the same way. When the annotation or return type the match is used for is an optional or a sum, that type is the match's and every arm's: `none` and `.Some(value)` take it, and a plain arm converts to it as an initializer would, so `let r: int32? = match n { 0 => none, _ => n };` makes `n` the present value and `let s: int32 | bool = match n { 0 => false, _ => n };` injects each arm into its member; an arm that cannot convert is reported where it is. With no such expected type, the first arm whose type is complete decides the match and the open arms before it are checked against that type; a match whose arms are all open needs an annotation. Arms whose types are complete must still agree with each other.

An unsuffixed integer literal used as an operand of an arithmetic, bitwise, or comparison operator takes the integer type of the other operand, on either side: `year % 4 != 0` needs no suffix for an `int32` year. The literal must fit that type, so `count == -1` with an unsigned `count` and `narrow + 300` with a `uint8` are errors rather than a comparison of bits or a wrapped value. A shift amount keeps its own type rather than taking the shifted operand's, but it must lie between zero and one less than the shifted operand's bit width: a count known while compiling is an error outside that range (`shift count 32 is out of range for 'int32', which takes counts from 0 to 31`), and any other count stops the program with `Panic: shift count out of range`, on every target and in every build profile. A count masked below the width, as in `value << (n & 31)`, needs no check.

Otherwise two integer operands of one signedness widen to the wider type, exactly as an assignment would widen the narrower one, whichever side it is on: with `small: int32` and `big: int64`, both `small + big` and `big + small` are `int64`, and so is `small < big` compared. The result keeps that width, so `let t: int32 = small + big;` and `total += big` on an `int32` total are rejected as narrowing. Operands of different signedness, such as `uint64` and `int64`, have no common type, since either conversion could change a value, so `u + s`, `u < s`, and `f *= s` are rejected; convert one operand with `as` to choose the type of the operation. In a generic function, an operation keeps the type of its operand written in the type parameters, and an instantiation in which the other operand is wider is rejected rather than narrowed.

Generic function calls may omit type arguments when the argument types determine them, including parameters behind references, pointers, slices, and named generic types. Explicit type arguments remain available and required when no argument determines a parameter. Inference does not relax mutable-borrow or argument-type requirements.

Struct literals can appear inside parenthesized expressions and call arguments within conditions. An ordinary unparenthesized condition still leaves its following brace to the statement body.

## Integer Division

Integer and character `/` and `%`, and the compound `/=` and `%=`, stop the program when the divisor is zero, on every target and in every build profile. The report has the shape a `Panic` call prints — `Panic: division by zero` followed by the function, file, line, and column of the operator, then one `  from Name` line for each function that led to it, innermost first — and nothing is unwound. A signed division whose quotient does not fit its type, the type's minimum divided by `-1`, stops the same way with `Panic: division overflow`; the matching `%` is held to the same rule, because the machine instruction that computes it fails on the same operands. A divisor written as a nonzero literal needs no zero check, a release build removes a check whose operands it knows to pass, and constant folding never folds a division by zero away.

## Integer Overflow

Integer `+`, `-` and `*`, a negation, and the compound `+=`, `-=`, `*=`, `++` and `--` stop the program when the exact result does not fit the operation's type, at every width from `int8` to `uint512` including `int` and `uint`, on every target and in every build profile. The report has the shape a division by zero has — `Panic: integer overflow in '*'`, naming the operator as written (`'+='`, `'++'`, and `'-'` for a negation), then the function, file, line and column, then the `  from Name` lines — and nothing is unwound. A shift by the width or more stops the same way, as described under contextual expressions. Character arithmetic and pointer offsets are unchanged.

The wrapping operators `+%`, `-%` and `*%`, and their compound forms `+%=`, `-%=` and `*%=`, compute the two's-complement result at the operation's type and never stop: `(hash ^ byte) *% 16777619` is an FNV step, and `0 -% value` the negation that wraps at the minimum. They take integer operands only, typed as `+`, `-` and `*` are, so a float, character or pointer operand is an error that names the checked operator to use instead. Hashes, random number generators, checksums and ciphers write their arithmetic with them; everything else keeps the checked forms, so an overflow nobody meant is a report rather than a wrong value.

An operation whose operands are known while compiling and whose exact result does not fit is a compile error rather than a program that always stops: `const Sum: uint8 = 200u8 + 100u8;` reports `integer overflow in '+': the constant result does not fit 'uint8'`, with the result and the type's range as a note, while `200u8 +% 100u8` is the constant `44`.

Every integer type has methods, in Core, for the cases where overflow is an expected answer rather than a mistake: `a.CheckedAdd(b)`, `CheckedSub` and `CheckedMul` return `T?`, none when the result does not fit, and `SaturatingAdd`, `SaturatingSub` and `SaturatingMul` clamp to the type's minimum or maximum. As with associated constants, the type is imported to use them: `import Core::uint64;`.

An integer `as` to another integer type keeps the value or stops the program with `Panic: integer cast out of range`: `300 as uint8`, `-1 as uint64` and `uint64::Max as int64` all stop, and a constant like them is a compile error. A conversion every source value survives, such as `int32` to `int64` or `uint8` to `int16`, has no check. `value.Truncate<T>()` keeps the low bits on purpose — a `uint64` holding `0x1234` truncates to the `uint8` `0x34`, and an `int64` holding `-1` to `uint64::Max` — and a wider `T` extends by the source's signedness, as `as` does. Generic code, whose type parameters have no such method, converts with `Core::ConvertWrapping`.

The checks cost a branch on the flags the arithmetic already sets. Up to 64 bits each is one overflow-checked instruction in LIR, which the Release back ends test with `jo` or `jb` after `add`, `sub` or `imul` on x86-64, and with `b.vs`, `b.cs` or `b.cc` after `adds` or `subs` on AArch64, comparing the high half of a 64-bit product (`mul`, `smulh`, `umulh`) and narrowing a narrower one; the report sits out of line with the other failure paths. Wider types are checked by comparisons written out while lowering, a product through its operands' halves rather than a division. A check the operands' spelling settles is not emitted at all, in any profile: `(a as uint128) * (b as uint128) + (c as uint128)` of `uint64` limbs cannot overflow, and `(wide >> 64) as uint64` always fits. Release removes a check its operands settle — constants, values whose known range leaves room for the result, a value a dominating comparison or a passed bounds check bounds, and a counter stepped by one below a bound it was compared with or decreased by no more than it started at — so a loop counter costs nothing, and a loop whose index offset like `i * n` is invariant checks that offset once before the loop.

## Character Casts and Patterns

An explicit integer or character cast to a character type checks the original value before narrowing. Code-unit types accept values from zero through 255 (`char8`) or 65535 (`char16`); scalar character types accept zero through U+10FFFF, excluding U+D800 through U+DFFF. An invalid constant is diagnosed while compiling, however it is spelled — `-1i32 as char32`, `-(1i8) as char8`, a named constant, or an element of a constant array, whose data never passes a run-time check — and an invalid runtime value stops with `Panic: invalid character cast`. A float source first converts as it would to `uint64`, truncating toward zero and saturating, so a negative value or a NaN becomes `0` (U+0000) and a value beyond `uint64::Max` saturates there; the result is then checked, so `1e10 as char32` stops while `-1.0 as char32` is NUL. To extract a low byte deliberately, mask the integer before casting. `char16` still accepts surrogate code units. Implicit widening between scalar widths, such as a `char32` passed where a `char64` is expected, needs no check, since every valid narrower value is valid in the wider width. Reading the bits through a pointer, `*((@raw) as *char32)`, reinterprets storage without any check and is the deliberate escape hatch for code that must handle an unvalidated value.

Character range patterns work at every width and follow the same inclusive or exclusive endpoint rules as numeric ranges. Constant arrays accept inline scalar casts as elements and repeat values.

## Float to Integer Conversion

`value as T` from a float to an integer type truncates toward zero while the result fits `T`. A value beyond the range saturates: one above `T::Max`, including positive infinity, becomes `T::Max`, and one below `T::Min`, including negative infinity, becomes `T::Min`, so a negative value converts to `0` for an unsigned `T`. A NaN converts to `0`. The rule is the same for every float width from `float8` to `float512` and every integer width, on every target, and whether the compiler folds the conversion or it runs, so `1e20 as int32` is `int32::Max`, `1e20 as uint128` is exactly `100000000000000000000`, and `float512::Infinity as int512` is `int512::Max`. Integer-to-integer `as` keeps the value or stops the program, as described under integer overflow.

## Floating-Point Widths

There are eight floating-point widths. `float32` and `float64` are IEEE 754 binary32 and binary64, and `float` names `float64`. `float8` is the E4M3 eight-bit format — four exponent bits biased by 7 and three fraction bits — with IEEE-style infinities and NaNs in its all-ones exponent, so its largest finite value is 240. `float16` is IEEE binary16. `float80` is the x87 extended format, a 64-bit significand with its integer bit stored and a 15-bit exponent, held in sixteen bytes of storage, so `float80::Bits` is 80 while `float80::Bytes` is 16. `float128` and `float256` are IEEE binary128 and binary256, and `float512` continues the same pattern with a 23-bit exponent and 489 bits of precision. Literals take the suffixes `f8`, `f16`, `f32`, `f64`, `f80`, `f128`, `f256` and `f512` and are read exactly at their own width.

The only implicit conversion between widths is `float32` to `float64`. Every other conversion — between two float widths, or between a float and an integer — is written with `as`. A float-to-float conversion rounds to nearest with ties to even, and a cast to a boolean asks whether the value is nonzero, so `-0.0f16 as bool` is `false` and a NaN converts to `true`.

`float32` and `float64` use the machine's floating-point instructions for everything but `%`, which neither instruction set computes exactly: x86-64 runs the x87 `fprem` loop described below, and AArch64 calls the synthesized `__rux_float_f32_rem` and `__rux_float_f64_rem` helpers. Every other width is computed by helper functions the compiler synthesizes from integer operations while lowering the program, named `__rux_float_f<bits>_<operation>`, generated only for the operations a program uses, and private to it. They depend on no package, Core included, so arithmetic at every width works in a program with no dependencies. The exception is `float80` on x86-64, where the x87 unit computes in exactly that format: `+`, `-`, `*`, `/` and `%`, the six comparisons, conversion between `float80` and `float32` or `float64`, and conversion from a signed integer of at most 64 bits run on x87. Around each of them that rounds — the four arithmetic operators and narrowing to `float32` or `float64` — the compiler saves the x87 control word, sets 64-bit precision with round-to-nearest, and restores the caller's word afterwards, because Windows leaves the unit at 53-bit precision; the remainder, the comparisons and the widening conversions are exact and need no change. `--emit asm` prints this x87 code as it is emitted. Conversion from `float80` to an integer, from an unsigned or wider integer, and to or from the other compiler-computed widths goes through the helpers, as all of `float80` does on AArch64. At every width, `%` is the exact remainder of truncating division, with the dividend's sign, as C's `fmod` computes it.

For `float8`, `float16` and `float80` through `float512`, every result that is not a NaN is bit-identical on every target, and identical to what the compiler computes when it folds the same expression while compiling, whether x87 or a helper produced it at run time. A NaN result is only guaranteed to be a NaN: its sign and payload are unspecified and may differ between targets and between folding and running, so a program that needs a particular NaN writes `T::NaN` and tests for one with `x != x` rather than by its bits. The same latitude covers the `float80` bit patterns that are not canonical encodings, which a program reaches only by writing bits directly: an unnormal, a pseudo-infinity or a pseudo-NaN behaves as a NaN, and a pseudo-denormal may come back normalized or as it was.

The C ABI knows only `float32` and `float64`, as C's `float` and `double`. The other widths are passed by the compiler's own convention, which matches no C type — not `_Float16`, `long double` or `__float128` — so one of them as an extern parameter or return type, as a parameter or return type of a function declared with `#Abi(...)`, or as a C-variadic argument is an error: `extern parameter 'value' has type 'float80', which has no C ABI mapping`. Convert the value to `float32` or `float64` at the boundary, or pass a pointer to it. The check also traverses by-value structs, unions, tuples, fixed arrays and nested fields; a software-float field anywhere inside an aggregate is rejected at the same boundary.

## Size and Alignment

Primitive storage of 16 bytes or more has alignment 16 on every target, including `float80` and all 128-, 256- and 512-bit types. Smaller primitives align to their storage size. `sizeof(T)` and `alignof(T)` report the same layout used by generated code, and are compile-time integers wherever one is required: `uint8[sizeof(Header)]` is an array type, `[0; sizeof(int64)]` a repeat, and a constant computed from them serves as either. A query of a type parameter has no value until the function is instantiated, and a struct whose layout depends on its own size has none at all.

A struct keeps declaration order, places each field at the next multiple of its alignment, and rounds its total size to its largest field alignment. For example, a struct containing `uint8` followed by `int128` places the latter at offset 16, has alignment 16 and occupies 32 bytes. A union places every member at offset zero and rounds the largest member size to the largest alignment. Module constant tables preserve their element alignment through object emission and linking on every target.

## Return and Deferred Cleanup

A `return expr;` evaluates and preserves `expr` exactly once before running deferred statements. The accepted copy or move into the return value happens during this evaluation. Later changes to source locals do not change the captured value, including fields of an aggregate; a returned pointer still points to its original storage.

On return, registered defers run in reverse registration order, from the innermost active scope outward, followed by the existing ownership cleanup. A transferred value is destroyed by its eventual owner, and a copied source remains subject to normal cleanup. A void return runs the same deferred statements without capturing a value, and an error that leaves the function through its `throws` channel runs them exactly as a return does, after the error has been captured. A `break` or `continue` runs, the same way, the defers registered in the scopes it leaves, up to and including the body of the loop it targets, so a pass of a loop body runs its defers however it ends, and so does an error leaving a `try` block for its `catch` arms. A deferred statement is evaluated when its scope exits, not when the `defer` runs, as in Swift: after `var n = 1; defer Print(n); n = 2;` the deferred call prints `2`. A deferred statement cannot itself leave its scope: `return`, `throw`, `?`, and a `break` or `continue` out of it are errors (`'return' cannot leave a deferred statement`); a loop written inside the deferred statement may still be left. No error may leave it either, so a call in it that can throw is written `try?` or `try ... catch`, and a bare `try` there is `an error cannot leave a deferred statement` with `help: handle it with 'catch' or discard it with 'try?'`; the same holds in a destructor. Each function has its own defer stack, including instantiated generic functions.

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

An import belongs to the file that writes it. One written at the top of a file names its items in that file, including the `module` blocks in it, and one written inside a `module` block names them in that block of that file; the other files of the package see neither, and a use there is `name 'Assert' is not defined in this scope`, with a note naming the file that does import it. A package's own declarations are shared by all its files with no import, and a package never re-exports what it imports: `import Format::Display;` does not find the `Display` that Format imports from Text. An imported function joins the overload set of a same-named function the package declares, as before.

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
    pub func Length(&self) -> uint { return self.length; }
    func Capacity(&self) -> uint { /* package helper */ }
}
```

Field visibility is the same in a `when` condition as in an expression. A compiler-supplied value such as `#build` is declared with an ordinary struct, so `when #build.debugAssertions` reads the field only because Core declares it `pub`; a private field of another package's intrinsic struct is rejected in a condition just as it is when read as a value.

An external struct initializer must be able to name every field, so a public struct with private representation fields is constructed through a public constructor or factory. Enum members and variant cases inherit their type's effective visibility; variant payload fields do not take individual `pub` markers. Interface requirements likewise inherit the interface's visibility. A concrete method may remain private while satisfying a public interface: dispatch through the public interface is allowed, but a direct call on the concrete type is not.

Compiler-generated copy and move operations are available wherever their type is available; neither runs user code. `#NoCopy`, `#NoMove`, and a destructor prohibit the capability everywhere regardless of visibility. Destructors are invoked by compiler glue regardless of visibility and normally remain private.

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

Members are values, written `Month::January` or `.January` where context already fixes `Month` (see [Cases written without their type](#cases-written-without-their-type)). Their discriminants participate in the existing explicit enum/integer conversions and scalar comparisons. Matching an enum selects members and cannot bind data:

```rux
func Days(month: Month) -> int32 {
    return match month {
        .January => 31i32,
        .February => 28i32,
        .March => 31i32
    };
}
```

Every `match`, whether it produces a value or is a statement, covers every value of its subject: it names every member of an enum, both `true` and `false` of a bool, and, because the values of an integer, a character, a float, a string or a structure are not listed one by one, ends a match on one of them with a `_` arm, a binding, or a structure pattern whose fields all match anything; otherwise it is `match on 'char32' is not exhaustive; its arms do not cover every value`. A statement that means to ignore the values it does not name says so with `_ => {}`, so turning a statement into an expression never changes what it must cover. A tuple is covered by the combinations of its elements, so `match flags { (true, true) => 1, (false, _) => 2 }` is rejected with `missing (true, false)`, while the four `(bool, bool)` combinations cover it without `_`. A case is covered by its arms together, so `.Light(true)` and `.Light(false)` cover a `Light(bool)` case without a `.Light(_)` arm, while literals of an integer payload never cover theirs. Converting an integer to an enum with `as` is unchecked, so it can produce a value that is no member at all. An exhaustive match without a `_` arm that meets such a value stops the program with `Panic: no match arm matched value of 'Month'` and the location of the `match`, instead of taking an arm or falling through; check an untrusted integer before converting it.

One arm grammar serves `match`, `catch`, and the compile-time `when`:

```rux
func Family(os: OperatingSystem) -> int32 {
    return match os {
        .FreeBSD | .Linux | .macOS => 1i32,
        _ => 2i32,
    };
}
```

- `_` is the default arm. It takes every value the arms before it leave, so it is the last arm (`the default arm must be the last match arm`) and takes no guard (`the default arm '_' cannot have a guard`). `_` is the same wildcard at every level, as in `.Some(_)`. `else =>` is not an arm (`'else' is not a match arm`); the default is written `_ =>`.
- `a | b` joins alternatives: the arm is taken when any of them matches, tried in source order. Alternatives may nest inside a payload, as in `.Digit(0 | 1)`, and a guard after them, `.A | .B if ready`, covers all of them. An alternative binds no names (`pattern alternative cannot bind 'value'`); bind in an arm of its own instead. Coverage counts what the alternatives match together, so `true | false` covers a bool. In a typed pattern the annotation is a whole type, so `v: A | B` is still a sum annotation, not two alternatives.
- A comma separates arms and may follow the last one, and an arm whose body is a block, or a declaration in a declaration-level `when`, may leave it out. Enum and variant case lists also take a comma after their last case.
- A `when` arm's patterns are compile-time values compared with its subject, so `when #target.os { .FreeBSD | .Linux => { ... }, _ => { ... } }` takes the same `|` alternatives and `_` default; a comma between its alternatives is `alternatives in a 'when' arm are separated by '|'`, and `else =>` is `'else' is not a match arm`, as in a match.

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

Cases are constructed with their declaration name, or without it where the context fixes the variant, as described below. A unit case may omit `()` where it is already a value; positional and named payloads use the corresponding call or initializer syntax. A match tests the active case before reading its payload, and an exhaustive match covers every declared case:

```rux
func Area(shape: Shape) -> float64 {
    return match shape {
        .Point => 0.0,
        .Circle { radius } => radius * radius * 3.0,
        .Rectangle { width, height } => width * height
    };
}
```

### Cases written without their type

`.Case` and `.Case(...)` name an enum member or a variant case without its type wherever the type the expression must have is already known: an annotated `let` or `var`, a function or method parameter, a field initializer, a returned value, a `throw` operand, an assignment to a typed place, and an arm of a `match` whose type its context or its earlier arms fixed. An optional destination names the type of its present value, so `let color: Color? = .Red;` builds a present `Color::Red`. A generic variant takes its type arguments from the expected type, so `return .Nothing;` in a function returning `Maybe<T>` needs none written. The case payload is itself a destination, so `.Framed(.Circle(4))` resolves both levels, and so is the operand of `.Some(...)` where an optional is expected: `let shape: Shape? = .Some(.Circle(4));`. A parameter of a generic function is a destination once the type arguments written at the call and the arguments before it decide its type, so `ValueOr<int, Error>(.Success(4), 0)` and `Second(dot, .Circle(3))` resolve, and a `try` marking such a case passes the type on: `ValueOr<int, Error>(try .Success(Parse(text)) catch { e => .Failure(e) }, 0)`.

```rux
func Find(values: int32[..], wanted: int32) -> Lookup<int32> {
    for value in values {
        if value == wanted { return .Found(value); }
    }
    return .Missing;
}
```

The expected type is the only place a case is looked up; no other declaration is searched for its name. Without an expected type, `let mode = .Fast;` is `case '.Fast' needs an expected type to name its enum or variant`, with help naming `Mode::Fast` when exactly one type in scope declares the case; write the qualified form there. A destination that is not an enum or variant, or one without that case, is reported as such. A case with fields needs its values (`.Found` alone is an error where `Lookup<int32>` is expected), and an enum member takes none. A case with named fields is still built with its type, as in `Shape::Circle { radius: 1.0 }`.

A variant that is itself the expected type and declares a `Some` case builds that case from `.Some(value)`, in place of the native constructor of the same name; through an optional, or where no such variant is expected, `.Some(value)` remains the native constructor. `.Success(value)` and `.Failure(value)` are ordinary case spellings with no native meaning, so they build the cases of an expected variant that declares them, such as Core's `Result<T, E>`, and are an error anywhere else.

Variant equality is structural. Values with different active cases are unequal without inspecting inactive storage. Values with the same case compare each active payload in declaration order, recursively using that payload type's equality; a variant is not equality-comparable when one of its reachable payloads is not. This applies to wide, nested, generic, tuple, and named payloads and does not compare padding or stale bytes.

Tuples also have structural equality. Both operands are evaluated once, and matching tuple types compare elements from left to right until a mismatch. Element comparisons preserve floating-point equality and declared equality operators; nested tuples, variants, and fixed arrays compare recursively. Every element must support equality, even when an earlier element could differ. `!=` negates the same comparison, and tuples have no built-in ordering.

Fixed arrays compare the same way: two arrays of one type are equal when every element is, compared from the first with the element type's own equality, whatever the array's width, and an array literal on either side is built as the other operand's type, so `values == [1, 2]` compares an `int32[2]` with an `int32[2]`. Arrays of different lengths or element types are not comparable, and `<`, `<=`, `>` and `>=` are not defined for arrays (`operator '<' is not defined for array 'int32[2]'`).

Structs without declared equality operators compare their fields in declaration order using the same rules, excluding padding. Both operands must have the same concrete type, and every field must support equality. Empty structs compare equal. Declared operators take precedence, including on nested fields; a declared `==` supplies `!=` when no explicit `!=` exists. Structural equality does not supply an ordering. Operator reference parameters borrow direct values and preserve already borrowed arguments.

Tuple and structure patterns take a value apart in a match arm, in a case payload, and in `let`. `(0, y)` tests the first element and binds the second; `Point { x: 0, y: y }` does the same by field name, the fields may be written in any order, and a field the pattern leaves out matches anything. A field written alone binds a variable of its own name, so `Point { x: 0, y }` and `let Point { x, y } = p;` need no repetition. A case payload follows the same rules: written with field names, as in `.Rectangle { width }`, it may leave fields out, while a positional payload such as `.Found(value)` lists every field. A literal compares at the type of the element or field it stands in, so `Reading { code: 255 }` tests a `uint8` field. Every part is tested, left to right, before any binding is set: a pattern that fails binds nothing, and a guard sees the bindings of its own pattern. A tuple or structure pattern whose every part is a binding or `_` matches every value, so it covers the subject and makes a later arm unreachable. A structure pattern names only fields visible where it is written, as reading or initializing them would: a private field of another package's structure is refused, though the pattern may leave it out.

In `let` and `var`, no later arm could take a value the pattern refuses, so every part must match every value: a name, `_`, a tuple, or a structure pattern, nested to any depth. `let Point { x: a, y: b } = move p;` binds both fields, `var` makes every binding mutable, and a generic structure binds its fields at the subject's type arguments. A literal, range, or case pattern inside a `let` pattern is an error, `refutable pattern in 'let' binding`; test the value with `match` instead.

Copy, move, and destruction are also case-aware. A variant is copyable or movable only when every reachable payload supports the operation. Moving transfers the active payload and invalidates the source. Destruction reads the tag once and destroys only the active payload, in reverse field order, exactly once; partially constructed cases roll back only the payloads already initialized.

The runtime representation is a private tag followed by storage aligned for the widest case. Calls and returns use the aggregate ABI selected for that complete layout; a variant is never passed as only its tag. The compiler keeps the tag width, payload offsets, construction, matching, equality, moves, and drop glue on one layout contract for every target. This representation is compiler-managed, not a substitute for a C ABI declaration.

Both forms are closed, so adding a member or case makes exhaustive matches fail until callers handle it. For an enum, an explicitly assigned discriminant is part of the source-visible numeric contract; append or assign members deliberately when those values are serialized or cross an ABI. A variant's tag numbers are not a source contract and must never be serialized directly. Stable files, protocols, and foreign interfaces should encode an explicit scalar enum and then translate to or from the in-memory variant.

Removing or renaming an enum member or variant case is likewise a source-breaking change. Changing a variant payload changes its value layout and the operations required from that payload, including equality, copying, moving, and destruction. Public library evolution should therefore treat case lists and payload types as part of the API even though the private tag representation is not.

`union` remains the explicit overlapping-storage type. It has fields rather than cases, stores no tag, performs no active-case check, and is appropriate for foreign or manually tracked layouts. Use `variant` when the compiler must know which alternative is active; use `union` only when the program or an external ABI owns that invariant.

The source cutover is intentionally narrow. Scalar enums retain their syntax, discriminants, conversions, and representation. First-party payload types that used the compiler's default enum layout migrate directly to `variant`. External payload enums that exposed an integer base or assigned discriminants are not source-compatible: because a variant's tag is private, those declarations require an explicit redesign, usually a scalar wire enum plus a separate payload type or a manually controlled union.

## Native Sums and Optionals

Three compiler-owned type forms describe values without a library declaration. Each has one purpose. Failure is not one of them: a function that can fail declares the error it throws, as [Errors](#errors-throws-throw-and-try) describes, and no fallible type can be written. The design decisions behind these forms and behind errors, and the migration from the retired forms, are recorded in [Native Sums, Optionals, and Errors](NativeOutcomes.md):

| Form     | Meaning                                                                                                                            | Inspection                                                          |
| -------- | ---------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| `A \| B` | A _sum_: one value of a set of distinct types.                                                                                     | Typed patterns, qualified case patterns, `is`.                      |
| `T?`     | An _optional_: a present `T` or absence. `T??` keeps both levels.                                                                  | `.Some(pattern)`, `none`, typed presence patterns, `is`, `?`, `??`. |
| `()`     | The _unit_, the empty tuple, whose only value is also written `()`. A function that throws and returns nothing completes with it. | The pattern `()` or a binding.                                      |

`variant` remains the form for alternatives distinguished by case names, and `union` the untagged overlapping storage. An outcome that must be kept as a value, rather than handled at the call, is Core's ordinary `variant Result<T, E> { Success(T), Failure(E) }`, which has no operator support. In a type, the postfix `?` binds tightest, then `|`, and the error of a `throws` clause is the whole type after the keyword. A suffix never reaches across `|` or `throws`: an optional member is written `A | (B?)` and an optional sum `(A | B)?`, and an optional error `throws (E?)`, while the ungrouped `A | B?` and `throws E?` are errors that ask for the grouping. As for every postfix suffix, `*T?` is a pointer to an optional and `(*T)?` an optional pointer; a reference is the exception, `&T?` being the optional reference a function returning a borrow may return and `&(T?)` a reference to an optional. Likewise `int32?[..]` is a slice of optionals while `int32[..]?` is an optional slice.

A sum is a set of resolved types: aliases are resolved, nested sums flattened, duplicates removed, and members ordered canonically by their fully qualified identity, so `A | B`, `B | A`, and `A | A | B` are one type, and `A | A` is `A`. Flattening never crosses an optional level. A sum over type parameters is normalized again for each instantiation, so `T | U` at `T = U = int32` is `int32`. Optionals never collapse: `int32??` distinguishes absence, a present absence, and a present value.

Native forms are ordinary values in parameters, locals, fields, containers, and each other. They cannot refer to themselves, so recursive data uses a named `variant` or `struct` holding a pointer, which may be an optional pointer `(*Node)?`. They are not `extend` targets and implement no interface, so `int32?` satisfies no `Display` bound; reusable operations are generic functions that spell the forms, such as `func ValueOr<T>(value: T?, fallback: T) -> T`, and inference descends through `T?`, `A | B`, and `&(T?)` as through named generic types. An alias names its resolved type and hides nothing: a public `type ReadError = ParseError | IoError;` exposes both members to exhaustive callers, while a named error variant keeps an explicit boundary and can carry the original cause.

### Construction and conversion

`.Some(value)` constructs a present level directly, and `none` is the absence of the expected optional's outer level, so every nested state is written without temporaries: `let stored: int32?? = .Some(none);`. `none` needs an expected type, so `let value = none;` is rejected with the annotation to write; `.Some(value)` alone takes its payload's type.

Where a value meets an expected native type, identity wins; otherwise exactly one route must apply, made of member injection, subset widening of a sum, presence construction, and the same widening applied within one optional payload. `A?` widens to `(A | B)?`, but no route converts a payload, extracts a value, or constructs a level inside an existing one. When two routes give different meanings, the conversion is ambiguous and must be written out. An unsuffixed integer or float literal targets a sum only when exactly one member is of its kind, independent of its value, so both `5` and `300` are ambiguous for `uint8 | int32`; every other literal is injected only when its own type is a member.

A function that declares `throws E` and no return type completes with `()` when it returns or falls off its end. A return type that merely contains the unit, such as `()?` or `() | X`, needs a value. An omitted return type on a function that does not throw declares a void function, which is not a unit-returning one, and a library struct named `Unit` would be an ordinary type unrelated to `()`.

### Matching

An ordinary `match` inspects native values one level at a time:

```rux
func Describe(setting: (Options | Defaults | DecodeError)?) -> int32 {
    return match setting {
        .Some(options: Options) => if options.verbose { 2i32 } else { 1i32 },
        .Some(_: Defaults) => 0i32,
        .Some(DecodeError::InvalidDigit(position)) => position,
        _ => -1i32
    };
}
```

`.Some(p)` and `none` select an optional level, so `.Some(.Some(value))` binds the payload of two levels and `.Some(none)` matches a present absence. A presence suffix is not a pattern: `value?`, `value??` and `none?` are rejected (`a presence suffix is not a pattern`) with the help `write '.Some(p)' to match a present value`, or `.Some(.Some(p))` for two levels. Alternatives combine levels, as in `none | .Some(0)`. A typed pattern `v: T` selects the sum member `T` and binds it, a subset `v: A | B` binds the matched members as a smaller sum, and on an optional subject the typed presence pattern selects one present level whose payload has that type. `_: T` selects without binding. A qualified case pattern `Type::Case(...)` selects the variant member and its case at once, and is ambiguous when two members are instantiations of the same variant; an unqualified `.Case` on a sum subject is rejected in favour of `Type::Case`. Guards and the unit pattern `()` work as elsewhere. The same patterns serve the arms of a `catch`, whose subject is the error.

An identifier pattern binds only a free name. A name that already resolves to a type, alias, generic parameter, enum, variant, interface, constant, function, or module, or that spells a case of the subject or of one of its members, is rejected rather than silently binding the whole subject: `Options =>` on `Options | Defaults` and `Missing =>` in a `catch` over a `DecodeError` are errors whose help names `options: Options`, `Options { ... }`, or `DecodeError::Missing`. Shadowing a local variable or parameter remains legal.

Coverage is checked over every level, so a missing `.Some(_: Defaults)` or `.Some(none)` is named in the diagnostic. An arm covered entirely by earlier unguarded arms is unreachable, but a `_` arm never is: it covers whatever remains, possibly nothing, which keeps `x: T => ..., _ => ...` valid when a generic `T | U` collapses at `T = U`. Patterns that depend on type parameters are rechecked for each instantiation and report conflicts there, with both locations.

A match consumes an owned subject, so arms that bind payloads own them and the payload of an arm that binds nothing is destroyed there. A borrowed subject is matched in place: bindings alias the payloads where they lie and a subset binding reads the original tag through the borrow. Such a view can be read and matched but not borrowed again, passed by reference, stored, or moved, and only a single-member binding writes through. No binding over a borrowed subject is moved out of: `move token` there is `cannot move out of 'token', which a pattern bound to borrowed storage`, whose help points at matching an owned value with `match move subject`, or cloning the binding. `==` and `!=` compare native values structurally: the tags of each level first, then the active payloads.

## Membership Tests

`value is Type` answers a question about the operand's type and never consumes, binds, or narrows it: after the test, `value` keeps its static type, and extracting a payload takes a pattern, as below. The tested type is a postfix type, so a sum target is grouped, as in `value is (A | B)`; an ungrouped `value is A | B` whose right operand names a type is rejected with that grouping as the fix. The right-hand type is resolved first, and its form and the subject's select the mode:

| Subject                             | Meaning                                                                                                                                     |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| A sum `A \| B \| C`                 | The active member is the tested type, or one of the members of a tested subset sum. A type that is not a member or subset is rejected.      |
| An optional `T?`                    | The value is present and its payload is the tested type, or a member or subset of a sum payload. Only one optional level is ever inspected. |
| Any other type                      | Exact type identity after alias resolution. A test that can never be true is a compile error, never a constant `false`.                     |
| An interface on the right-hand side | Reported as unavailable: interface implementation tests are not implemented, and no native form implements an interface.                    |

A reference subject is tested through the reference without being consumed. A test whose subject or tested type mentions a type parameter is checked again for each instantiation, so `x is T` over `T | U` stays valid when `T` and `U` are the same type and the subject collapses to that type.

Membership is not interface implementation: knowing that every member of a sum implements an interface, or that a test succeeded, makes no shared member available on the sum.

### Binding `is`

`subject is pattern` tests the subject against any pattern a `match` arm takes and is a `bool`; `value is T` is the type test above, which reads as the pattern `_: T`. What follows `is` is a pattern when it starts as only a pattern can — `.Case`, `none`, `_`, a literal, `name: T`, `Type::Case(...)`, `Type { ... }`, or a tuple holding one of these — and a type otherwise, so `shape is Circle` and `entry is (A | B)` keep testing types. Alternatives are written with `|`, as in `color is .Red | .Green`, and a guard is not: a further test is written with `&&`.

```rux
if entry is .Some(value) { Use(value); }
while reader.Next() is .Some(event) { Handle(event); }
if error is .NotADigit(c) && c != ' ' { Report(c); }
if shape is circle: Circle { Draw(circle); }
if !(entry is .Some(_)) { return none; }
```

The names a pattern binds are in scope where the test is known to be true: the body of the `if` or `while` whose condition holds the test, and the operands after it in a chain of `&&`, inside a condition or anywhere else. They are not in scope in an `else` branch, after the statement, or after a `while` loop, and a `while` binds them afresh on every pass. A test that binds names under `||` or `!` is an error (`an 'is' test under '||' cannot bind names`), because there the names would be in scope where nothing was bound; `_` tests the same shape without binding. Where a brace would open the body of the `if` or `while`, a pattern with a braced payload is grouped, as in `if (shape is .Rect { width, height }) && width > 0 { ... }`.

The subject is evaluated once. A temporary, or a subject transferred with `move`, is owned by the test the way a `match` owns its subject: its bindings own what they take, and what they leave is destroyed. A named or borrowed subject stays with its owner: the bindings borrow from it, so the subject cannot be written or moved while a binding is in use, and it keeps its value after the test. The test never narrows the subject's type — only the bound names carry the payload — and it needs no coverage: a value it does not match makes it `false`.

## Bindings and Parameters

Bindings have three forms:

```rux
const MaxSize = 1024;
let size = 10;
var index = 0;
```

`const` is a compile-time value, `let` is an immutable runtime binding, and `var` is a mutable runtime binding. Parameters always use `name: Type`; binding mutability is not written before a parameter name. A method is a function whose first parameter is the receiver `self`. Inside `extend T` the receiver's type is already known, so it is written by how the method takes it:

```rux
extend Buffer {
    func Read(&self)                 // borrows the Buffer for reading
    func Clear(&var self)            // borrows the Buffer exclusively for writing
    func Consume(self)               // takes the Buffer by value
    func Handle(*var self)           // takes a raw *var Buffer; `*self` takes a *Buffer
    func ~Buffer(&var self)          // a destructor takes its receiver the same way
}
```

In `extend Vector<T>` each form names `Vector<T>` with its type arguments, and in an interface requirement it names the implementing type, so `func Next(&var self) -> T?` borrows the implementing value for writing. A receiver never names its type: `self: &Buffer` is `a receiver does not name its type`, with `help: write '&self'; the 'extend' block or interface gives the type`, and `self: &var Buffer`, `self: Buffer` and `self: *Buffer` are answered with `&var self`, `self` and `*self` the same way. The receiver comes first, and only a function inside `extend` or an interface takes one: `func Free(&self)` is `receiver '&self' is only allowed in an 'extend' block or an interface requirement`, and a receiver written after another parameter is `receiver 'self' must be the first parameter of method 'Late'`.

A `const` initializer, at module level or inside a function, is computed by the compiler once: it may use literals, operators, casts, `sizeof` and `alignof`, compile-time intrinsics such as `#target`, other constants, struct, array, tuple, and case values built from them, and calls of functions. A variable is a run-time value, so `const C = v;` is rejected with `'v' is not a compile-time constant`, and so is a call with such an argument; use `let` for a value computed at run time. An integer constant written without a call is a compile-time integer wherever one is required, so `var flags: bool[Limit] = [false; Limit];` sizes and fills an array from `const Limit: uint = 100;`.

### Constants computed by calls

A constant may call a function, and the compiler runs the call while it compiles, so a lookup table is computed from its definition rather than written out by hand or generated by a script:

```rux
func DivisionTable(polynomial: uint32) -> uint32[256] {
    var table: uint32[256] = [0u32; 256];
    for index in 0..256 {
        var register = index as uint32;
        for _ in 0..8 {
            register = if (register & 1) != 0 { (register >> 1) ^ polynomial } else { register >> 1 };
        }
        table[index] = register;
    }
    return table;
}

const Crc32Table: uint32[256] = DivisionTable(0xEDB88320);
const Squares: int[10] = Tabulate(Square);
```

The function needs no marker. What decides whether it may be called is its body and every function that body calls, read in full, including the paths a particular call never takes: none of them may use a raw pointer (a pointer-typed value, `@`, `*p`, `null`, or a raw pointer parameter), an `unsafe` block or `unsafe func`, an extern function or variable, an `asm func`, a thread-local or a module variable. Allocators are reached only through raw pointers and interfaces, so the same rules rule them out. A call that breaks one is `constant 'Table' cannot be computed at compile time: 'Seed' reads module variable 'Counter' (in 'Build' -> 'Seed')`, which names the first such operation and the calls that lead to it, and a note gives the operation's position.

Evaluation follows the program's own rules at the target's widths: integers of every width and floats of every width, `float80` and the narrow floats included, compute exactly what the target computes, and a character cast is checked as at run time. What would stop the program instead stops the compilation, at the constant: an integer `+`, `-`, `*` or negation whose result does not fit its type (`'*' overflows 'int32'`), a division or remainder by zero, a signed minimum divided by `-1`, a shift by a negative amount or by the width or more, an index past the end of an array, an invalid character, and an error that is thrown, even one a `catch` would handle at run time. Evaluation also stops after 4,000,000 steps (statements, loop iterations and calls), and when calls nest more than 64 deep; each reports the chain of calls it was in. Evaluation runs locals, parameters, `&` and `&var` parameters and receivers, loops with labels, `if`, `match` with every pattern over values of these kinds, methods, constructors, default arguments, generic functions, and functions passed as values. A value of a native optional or sum type, a string or slice, a compiler-supplied value such as `#target` inside a called function, a call through an interface or to a compiler-supplied function, `defer`, `?`, `try?`, and `try` blocks are not evaluated; reaching one is an error that says so. A call marked with `try` or `try ... catch` runs as any other, so the error it throws is the error at the constant described above.

The result must be a compile-time value, as for any constant: scalars, and arrays, structs, tuples and cases built from them. The compiler records it and lays it out exactly as the literal that spells it would be, so reading a computed constant at run time costs what reading a written one does, and the call is never run by the program. Such a constant can be read by other constants but cannot size an array or count a repeat, because array types are decided before any function is run. `when` stays conditional compilation over compile-time intrinsics and constants that call nothing; a computed constant provides values, not code.

Taking `@C` for a module constant of type `T` whose value is made of scalars — a number, a bool, a character, an enum without payloads, or a struct, tuple, or array of them — produces a read-only `*T` pointing to its aligned, read-only module storage. Repeated address expressions identify the same object, including through a public import from another package, and dereferencing it yields the complete constant value.

### Module variables

At module level, `var Name: Type = value;` declares a module variable: one storage location for the whole program, shared by every function that can name it. `pub var` publishes it like any other declaration, so another package can import it, read it and assign to it, and a module's variable is reached through its path, as in `Settings::Level = 5;`. The type is always written, because other functions and packages agree on the variable's layout, and an initial value is required. `let` has no module-level form: a value that never changes is a `const`.

```rux
var Requests: uint64 = 0;
pub var Verbosity: int32 = 1;

func Record() {
    Requests += 1;
}
```

The initial value follows the `const` initializer rules, except that it calls nothing: it is computed by the compiler from literals, operators, casts, constants, and struct, array and tuple values built from them, so a call is rejected with `call to 'Seed' is not a compile-time value` and another variable with the message a constant gets. A module variable that is not an array may start from a computed constant read by name, as in `var Running: int32 = Fib30;`. The variable starts the program holding exactly those bytes. A value whose bytes are all zero is placed in zero-fill storage and takes no space in the executable; any other value is placed in initialized data. `@Name` is a writable `*var T` to that storage, aligned as `alignof(T)` reports, and every `@Name` and every package names the same address.

Module storage is never destroyed, so a module variable cannot have a type that needs destruction, and it cannot hold a slice, which would borrow storage it does not own. Nothing moves out of a module variable, since every function shares it and it must always hold a value; `move Name` is rejected with `cannot move out of module variable 'Name'`, and a copy or a fresh assignment is the way to take its value. Module variables are named in PascalCase, like other module-level declarations. A `pub var` written inside a function body is rejected: a local is declared with `var` and no `pub`.

### Thread-local variables

`thread var Name: Type = value;` declares a thread-local variable: a module variable of which every thread has its own copy. Each thread's copy starts as the initial value, whether the thread is the main one or one started later, and keeps its address for the thread's life; `@Name` on two threads gives two addresses. Everything else follows the module-variable rules above: the type and a compile-time initial value are written out, `pub thread var` publishes it, a module's variable is reached through its path, a type that needs destruction or a slice is rejected, and nothing moves out of it. `thread` is not a keyword: it means this only before a module-level `var`, and stays an ordinary name everywhere else.

```rux
thread var Depth: int32 = 0;

func Enter() -> int32 {
    Depth += 1;
    return Depth;
}
```

A thread-local variable costs one or two instructions more than a module variable to reach. On Linux and FreeBSD the program's thread-local block sits at a fixed offset from the thread pointer, which the dynamic loader sets up, so a program with thread-local variables is linked against the C library. On Windows the image carries a TLS directory and the loader gives each thread a copy of the template; on macOS each variable has a descriptor dyld binds, and reading one calls through it. A shared library or a static library with thread-local variables is rejected for now.

The removed `var value: T` parameter form is an error. A function that needs mutable local storage moves the parameter into a local with `var local = move value`; a function that mutates caller-owned storage instead accepts `value: &var T`.

A parameter's default value is evaluated where the call is written, in the caller's scope, each time the argument is omitted. It therefore cannot read any parameter of its own function, `self` included: `func C(x: int, y: int = x)` is an error. An overload that passes the value, `func C(x: int) -> int { return C(x, x); }`, gives the same call.

When several overloads accept a call, declaration order never decides it. An overload that is no worse for any argument and better for one wins: an argument of exactly the parameter's type is better than one differing only in a view's writability, which is better than one reached through a borrow or a scalar read, which is better than any other conversion. Arity never decides before conversions do: `E(1i32)` calls `func E(x: int32, y: int32 = 0)` rather than `func E(x: int64)`, because its argument converts better. Among overloads still tied, the one the arguments fill exactly — every fixed parameter given, no default value and no variadic tail — wins, so `D(2)` calls `func D(x: int)` rather than `func D(x: int, y: int = 1)` or `func D(x: int, rest: int...)`, and then a function that is not generic wins over a generic one. Overloads still tied after that make the call ambiguous, which is an error. Method overloads, called through a value or as `Type::Method`, are ranked by the same rules, so declaration order never decides a method call either.

A function name used as a value, rather than called, is a function value of the function's type. A generic function has no one address until its type arguments are known, so a generic name used as a value takes them from the function type it is assigned, returned or stored as: `let double: func(int64) -> int64 = Double;` is the `int64` instance of `func Double<T>(value: T) -> T`, and inside a generic function the instance follows that function's own type arguments. A generic name with nothing to say what it is instantiated with, as in `let f = Double;`, is an error: `generic function 'Double' has no type arguments here`. An overloaded name used as a value is chosen the same way: it is the overload whose function type its destination expects, whether that is an annotated binding, an assignment, a function-typed parameter of a function with one declaration, or a return type, and a module path such as `Ops::Twice` chooses alike. With no function type to choose by, as in `let f = Scale;`, it is an error (`overloaded function 'Scale' has no function type here to choose an overload by`), and so is a type no overload has (`no overload of 'Scale' has type 'func(float64) -> float64'`); both list the overloads' types.

### Named arguments

An argument may be labeled with the name of the parameter it binds: `Open(path, create: true, truncate: false)`. Labels are optional and checked, not reordered: labeled arguments follow the positional ones, in the order of the parameters, and the arguments are evaluated in source order. A label may skip parameters that have default values, which take them: `Open(path, mode: 0o600)` passes the defaults of `create` and `truncate`. A call is resolved by the ordinary rules, and its labels are then checked against the declaration it selected, so labels never choose between overloads. A label that names no parameter lists the names there are (`'Open' has no parameter named 'append'`), a label before one of an earlier parameter is `argument label 'create' is out of order`, a label that skips a parameter without a default is `argument for parameter 'target' of 'Copy' is missing`, and a positional argument after a labeled one is `positional argument after a labeled one`. A variadic argument cannot be labeled, and a call through a function value, a case constructor or an extern function has no parameter names to match. Indexing and operators take no labels. A public function's parameter names are therefore part of its API (see [Package Style](PackageStyle.md#parameter-names)).

### Function literals

A function literal writes a function where a value is expected: `CountWhere(items, func(x: int) -> bool { return x > 100; })`. It captures nothing. Its body sees the module-level names of its file — constants, functions, types and module variables — but no local, parameter or `self` of the function it is written in, so naming one is an error: `function literals cannot capture 'limit'`, with `help: pass it as an argument or use a named function`. A literal is therefore an ordinary function with an address and no environment: it allocates nothing, has no new representation, and its value is a function value like a named function's, passed, stored, returned, called where it is written, or handed to a generic function, which deduces its type parameters from it. Parameter and result types are written as in a declaration; they may be left out only where the expected function type supplies them — an annotated binding, an assignment, a return value, or a function-typed parameter of a function with one declaration — as in `let double: func(int) -> int = func(x) { return x * 2; };`. A literal that leaves its parameter types out takes its result type from the same place. With no expected type, or one with another number of parameters, the parameter is an error: `function literal parameter 'x' has no type`. The body is checked as a function of its own: `break` and `continue` cannot leave it, `try` and `unsafe` around it do not reach into it, and a type parameter of the enclosing generic function is not in scope. The compiler emits each literal once, as a private function of its module.

Members are always named through their value, such as `self.length`; a receiver never enables implicit field lookup.

Inside `extend T`, `Self` names `T` with its type arguments wherever a type or the type's name is written: in a signature (`-> Self`), a binding's type, a struct literal (`Self { ... }`), a pattern (`Self { x, y }`, `Self::Case(value)`), a type-named constructor call (`Self(...)`), and a path such as `Self::Origin` or `Self::Of(first, second)`. A call through such a path passes the block's type arguments first, as the written-out `Pair::Of<T>(first, second)` would, and an associated constant declared in `extend Buffer<T>` is `Self::Capacity` there and `Buffer::Capacity` or `Buffer<int>::Capacity` outside. A member may be named through an instantiation anywhere: `Result<int, E>::Failure(error)` is `Result::Failure<int, E>(error)`, the type's arguments before any the member adds, and `Maybe<int>::Nothing` is the unit case of `Maybe<int>`. Through an alias of an instantiation, `type Kept = Result<int, Error>`, a case takes the alias's arguments: `Kept::Success(3)` is a `Result<int, Error>`. `Self` is a plain alias: it does not make a method generic over the types that implement something, so it changes neither dispatch nor layout, and in an `extend Rod : Measured` block it is `Rod`. Inside an interface `Self` is the implementing type, which a bound supplies. Anywhere else, including a declaration nested inside a method body, it is `'Self' is only available inside an 'extend' block or an interface`.

A method may be called as `Type::Method(receiver, args...)`, with an explicit first receiver argument and the same borrowing, mutability and ownership rules as a call through a value. Methods follow the same variadic and default-argument rules as free functions. A typed variadic parameter receives a slice of its elements, accepts compatible explicit elements or a spread, and may be empty. Generic functions can infer their type arguments from variadic elements. Overload selection, for functions and methods alike, checks every variadic element, prefers a fixed overload filled exactly over one needing defaults or a variadic tail only when argument conversions tie, and diagnoses remaining ambiguity. A `#Format` parameter on a method receives the same literal-placeholder check as a free function.

### Extern variables

`extern Name: Type;` declares data a library defines: storage this program reads and writes but does not own. It names the library it is imported from with `#Link("library")` before it or on the `extern { ... }` block holding it, and `#Link("library", "symbol")` gives it a Rux name that differs from the exported symbol. A declaration with no library is an error: `extern variable 'Name' must name the library it is imported from`. Its type must have a C ABI mapping, as an extern parameter's must.

```rux
#Link("libc.so.6")
extern opterr: int32;

func Quiet() {
    opterr = 0;
}
```

A read, an assignment and `@Name` all reach the library's own storage. The loader writes the variable's address into a slot of the program, which the code reads it from: an import address table entry on Windows, a GOT entry on Linux and FreeBSD, and a non-lazy pointer on macOS. A FreeBSD executable defines `environ` and `__progname` itself, so an extern variable naming one of them reaches the executable's own word. As with a module variable, nothing moves out of an extern variable: `move Name` is rejected with `cannot move out of extern variable 'Name'`.

### Attributes

An attribute is a `#Name` written before what it describes, with its arguments in parentheses when it takes any. Before a declaration, `#Abi(.Win64)` selects a calling convention, `#Link("library")` describes how an extern declaration is imported, `#NoReturn` marks a function that never returns to its caller, `#Test` marks a function that a test build of its package runs, `#Unsendable` marks a struct, variant or union whose values stay on the thread that made them, `#NoCopy` and `#NoMove` mark one whose values are never copied or never moved (see [Copy and Move](#copy-and-move)), `#Allow("naming.type")` suppresses one lint rule for that declaration, and `#Error("message")` and `#Warn("message")` report at each use. An attribute that takes no arguments is written without parentheses: `#Test()` is `'#Test' is written without parentheses`, and an argument inside them is an error such as `'#Test' does not accept arguments`. `#Format` is the one attribute written before a parameter:

```rux
pub func PrintLine(#Format format: char8[..], args: &Display...) -> IoError?
```

It marks a format string: a `char8[..]` parameter followed by a variadic parameter whose arguments fill its placeholders. When a call passes a string literal for it, the compiler counts the literal's placeholders — `{}` and `{:spec}`, with `{{` and `}}` as literal braces — against the variadic arguments, and a mismatch is an error such as `format string has 2 placeholders, but 1 argument was provided`. A format that is not a literal, a spread argument, and a pattern that is itself malformed are left to the formatter's run-time check. The attribute is all the compiler knows: it recognizes no package or function by name, so any function taking a format string can declare one. `#Format` takes no arguments, appears at most once per parameter list, and cannot be applied to the receiver or to the variadic parameter itself.

A placeholder may instead name the value it writes: `PrintLine("{key} = {value}")` and `PrintLine("{point.x:>8} at {index}")`. `{name}` and `{name:spec}` name a binding in scope — a local, a parameter, a module constant or variable, or `self` — or a field path through one, and write it as `{}` and `{:spec}` would write the same value passed as an argument. Only names are allowed: a call or an operator inside the braces is an error (`format placeholder '{Twice(count)}' must name a binding or a field path`), and the expression is passed as an argument to a `{}` placeholder instead. One literal names every placeholder or none (`format string mixes named and positional placeholders`, with `help: name every placeholder or none`), and a literal with named placeholders takes no arguments after it. A name nothing declares is reported where the placeholder writes it. The compiler rewrites such a literal into the positional literal and the values its names stand for, so the run-time formatter is unchanged, and only a literal passed for a `#Format` parameter is read this way: elsewhere `"{key}"` is plain text.

`#Warn("message")` and `#Error("message")` report at every use of what they mark, once per place it is written: a call, a function used as a value, a constructor call, a struct literal, and a type, constant, alias, enum or variant named anywhere, a signature or another type's field included. Uses inside the marked declaration itself, its fields, and the extensions of the type it declares are not reported.

A `#NoReturn` function cannot `return`, and its body must not reach its end: every path ends in `Panic`, a call to another `#NoReturn` function or extern, qualified or not, or an endless `loop`, and a body that can finish is an error (`'#NoReturn' function 'Stop' can reach the end of its body`).

`#Abi` names one of `.C`, `.SysV` and `.Win64`. AArch64 has one calling convention, which `.C` names, so `.SysV` or `.Win64` in a build for AArch64 is an error (`'#Abi(.Win64)' names an x86-64 calling convention, which a build for AArch64 cannot use`); a declaration written for both machines keeps its x86-64 convention in a `when #target.arch` arm.

## Values, References, and Raw Pointers

The five storage-facing type forms are:

```text
T        owned value
&T       immutable reference
&var T   mutable reference
*T       raw read-only pointer
*var T   raw writable pointer
```

References are non-owning, non-null, automatically dereferenced for member and index access, and cannot perform pointer arithmetic. They may be parameters, receivers, local aliases, or the result of a function whose signature says what it borrows from, as [Returning references](#returning-references) describes. They cannot be fields or otherwise stored beyond the current call, and a reference, or a type holding one, is no type argument: a generic body is checked once for whatever its parameter stands for, so nothing proves it does not store the value, as `Thread::Spawn` stores its context. Any number of immutable borrows may coexist, or one mutable borrow may exist exclusively; a borrow ends after its last use. A reference cannot destroy its referent or move ownership out of it. A concrete value may be borrowed directly as an immutable or mutable interface view without copying or consuming the value, as [Interface views and handles](#interface-views-and-handles) describes.

A reference to a Copy primitive scalar can supply its value in a typed assignment, return, call, cast, arithmetic expression, comparison, or boolean condition. The compiler loads the scalar without moving ownership or changing the reference's type. Passing the same expression to a reference parameter preserves the borrow, and mutable borrows keep their usual exclusivity rules. Arrays and tuples can hold scalar copies read from references, but aggregate referents are never implicitly copied. Generic scalar reads are checked for every instantiated type.

A name bound to a reference to a Copy primitive scalar — a parameter, a receiver, or a `let` alias — is also a place to write: through `count: &var int`, `count = value`, `count += 1`, `count++`, and `--count` store into the caller's storage, so `func Bump(count: &var int) { count += 1; }` increments the argument. A reference is never reseated: `=` through it always writes through, so a `var` binding of reference type, written or inferred, is an error (`'var' binding 'alias' cannot hold reference '&var int'`) whose help reads `references cannot be reseated; use a pointer or a new binding`. A write is an exclusive access under the usual borrow rules. Through `&T` the same write is an error naming the reference. A reference is never written with `*`, which stays a raw-pointer operator. For the same reason `@` of a reference is the address of its referent, not of the reference: with `p: &Point`, `@p` is a `*Point` equal to the caller's `@point`, and through `&var Point` it is a `*var Point`.

A referent of any other type — a struct, tuple, array, or a type parameter — is replaced whole the same way, with `=` through a parameter, a receiver, or a `let` alias of type `&var T`: `func Reset(pair: &var Pair) { pair = Pair { a: 0, b: 0 }; }` replaces the caller's value, and so does `self = Self { a: 0, b: 0 };` in a method taking `&var self`. The write follows the assignment rules for `T` exactly as an assignment to a `var` local of type `T` does: the new value is produced first, the old value is destroyed once, and the result is installed; a move-only value needs `move` or an explicit `Clone`; and the write is an exclusive access. Compound assignment and `++`/`--` reach a scalar referent only, and an interface view cannot be replaced, because its referent's type is not known. Parts of an aggregate are written through their fields and elements as before.

Writing a parameter taken by value is the error `cannot modify parameter 'x'`, and writing a receiver taken by value is `cannot modify immutable receiver 'self'`. Neither has a `var` form, so the help offers taking it as `&var T` to change the caller's value, or, for a parameter, moving it into a `var` local.

Interface method arguments follow the same conversions as ordinary function arguments. A concrete local or concrete reference can supply a borrowed interface argument directly, and a temporary can supply a read-only one; an existing interface view retains its data and vtable. Mutable interface arguments require writable storage. Interface requirements may provide typed default arguments and variadic interface arguments.

An interface requirement states through its receiver what it does with the implementing value. A requirement with no receiver, or with `&self`, only reads it; a requirement that writes it declares `&var self`. An implementation may read where its requirement permits a write, but a method declaring `&var self` cannot satisfy a read-only requirement, because a read-only view would then modify storage its caller only lent for reading. A call of a writing requirement needs a writable receiver exactly as a concrete method taking `&var self` does through a view: it is accepted through `&var Gauge` or a `&var T` bounded by the interface, and rejected through `&Gauge` or a `&T`. An interface value is a handle, so a writing requirement is called through any one, `let` or `var`: the call writes the implementation the handle addresses, never the handle. The receiver is the data half of the view, so naming `Self` there does not restrict where the requirement can be called.

A requirement ends with `;`: an interface supplies no default, so a body is an error (`requirement 'Describe' of interface 'Shape' cannot have a body`). An implementation's method takes and returns exactly what its requirement says, with `Self` standing for the implementing type, because every call through the interface is typed by the requirement: `extend Square : Shape` with an `Area` that returns `int32` where `Shape` requires `float64` is `method 'Area' does not match the requirement of interface 'Shape'`, with notes spelling both signatures, and a type whose methods have the right names but the wrong signatures does not satisfy a `T: Shape` bound.

Inside `func Render<T: Display>(value: T)`, the declared bound allows `value` to be passed to a `&Display` parameter, including a variadic one. Bounds also permit borrowed interface arguments with the usual mutability restrictions. A primitive satisfies a nonempty interface bound only when it has a matching implementation; built-in comparison operators do not implicitly implement `Core::Comparable`. `Core::Sendable` and `Core::Copyable` are the interfaces the compiler decides for itself: no extension implements either. Every type satisfies `Sendable` except a reference and a type that is or holds one declared `#Unsendable`, such as `Rc`. A raw pointer satisfies it, since what it shares is the program's to vouch for, and a type parameter satisfies it only when bound by it. `Thread::Spawn` and `SpawnWith` bound their context and result by it. A type satisfies `Copyable` when it is plain data, as [Copy and Move](#copy-and-move) defines it, and a type parameter only when bound by it: `Twice<Owner>(...)` for `func Twice<T: Copyable>(value: T) -> (T, T)` is `type argument 'Owner' does not satisfy interface bound 'Copyable' on type parameter 'T'`, with the note `'Owner' owns resources, so it moves instead of copying`. `Core::Cloneable` is an ordinary interface, `interface Cloneable { func Clone(&self) -> Self; }`, which an owning type implements to be duplicated by generic code under `T: Cloneable`.

An interface may take type parameters, as in `interface Source<T> { func Next(&var self) -> T?; }`, to name the types its operations work with. A generic interface is only a bound: `func Map<I: Source<T>, T, U>(source: I, transform: func(T) -> U)` may call `source.Next()` and gets a `T?`, while using `Source<int32>` as the type of a value, a field or a parameter is an error. A bound names exactly as many type arguments as the interface declares, which may be the bounding declaration's own parameters, and an `extend Counter : Source<int32>` names them too; an interface's own parameters take no bounds. A type satisfies such a bound when its methods have the names the interface requires and return the types the bound's arguments fix, so a `Counter` whose `Next` returns `int32?` satisfies `Source<int32>` and not `Source<int64>`, whether or not it says so in an `extend`. A type parameter that appears only in a bound's type arguments, like `T` above, is deduced from what the bounded argument's method returns or takes, so `Map(Counter { ... }, Double)` needs no type arguments written and generic types chain: `Map(Map(counter, Square), Double)`.

### Interface views and handles

An interface is used in three ways, and each says where the implementing value lives:

```rux
func Measure<S: Shape>(shape: &S) -> int32 { return shape.Area(); }   // a bound: resolved for each concrete type
func Describe(shape: &Shape) -> int32 { return shape.Area(); }        // a view: borrowed for the call
struct Scene { shape: Shape; }                                        // a value: a handle kept in storage
```

- **A bound** is static dispatch. Each instantiation calls the concrete type's methods directly, and the value stays where the caller has it, borrowed through `&S` or `&var S` or moved in as `S`.
- **A view**, `&I` or `&var I`, is a borrow like any other reference: a concrete place converts to one directly, without copying or consuming the value, and the borrow ends with its last use. A read-only view may also borrow a temporary, which lives until the end of the block it is written in, so `Describe(Square { side: 2 })` and `PrintLine("{}", total + 1)` lend their arguments for the call; a `&var I` needs a place, since writes to a temporary would be lost. The format functions take their arguments as `args: &Display...` for this reason.
- **A value** of type `I` is a handle: the address of an implementation and its method table, copied by copying both and destroying nothing. It owns nothing and copies nothing, so it never keeps storage alive and a function may return one or store it anywhere. An `I?` and an array of `I` hold handles the same way.

No implicit conversion makes a handle from a value with state, because the handle would have to point at a copy that something must own: `let shape: Shape = square;`, an argument, a field, a return, an assignment, an optional or an array element made from a `Square` is `cannot make interface value 'Shape' from 'Square'`, with the note `an interface value is a handle to an implementation stored elsewhere and copies nothing, so only a stateless value converts to one` and `help: borrow it as '&Shape', bound a generic parameter by 'Shape', or make a handle from a pointer to it inside 'unsafe'`. A view does not become a handle either, `let kept: Shape = view;`, since the handle would outlive the borrow, and neither does a value of a type parameter bounded by the interface. A handle comes from one of three places:

- **A stateless value.** A structure with no fields and no destructor has nothing to address, so it converts to a handle anywhere, and the handle stays valid for the whole program: its data half points at the method table rather than at any storage. `SystemAllocator` is one, which is why `Vector<int>(SystemAllocator())` needs nothing more.
- **A pointer, inside `unsafe`.** `p as Shape` with `p: *var Square` makes a handle to what `p` addresses. The handle is used later without `unsafe`, so making it is the unsafe operation (`making interface value 'Shape' from pointer '*var Square' requires 'unsafe'`), and its `// Safety:` comment states the promise: the implementation outlives every copy of the handle and is not moved while one exists. A read-only `*Square` makes a handle only to an interface none of whose requirements writes its receiver (`cannot make interface value 'Counter' from read-only pointer '*Cell'`, noting the requirement). An `unsafe func` that returns such a handle, such as `Arena::Handle`, carries the same promise to its caller.
- **Another handle**, copied or returned as it is.

The rationale is that a conversion is where ownership is decided, and an interface value made from a concrete value used to decide it silently: it held a copy of the value in the frame that made it, so it could not be returned, stored past the call, or kept in an array element made in a loop, and an owning value was moved into it and destroyed with that frame. Every one of those cases needed its own rule. A handle needs one: it owns nothing, so it is copied freely and ends without a destructor, and the only question — whether what it addresses is still there — is answered either by there being nothing to address or by an `unsafe` block that says why. Code that needs the value's own storage borrows it as a view, and code that wants the concrete type keeps it through a bound.

Static and dynamic dispatch trade code size for call cost. A bound produces one copy of the generic function per concrete type, and a release build inlines the methods into it; a view or handle produces one copy of the function, which calls each method through the table with two dependent loads before an indirect call. Measured on `windows-x86_64` in a release build, a function making three calls of a one-line method, used with one, two and four implementing types, came to 592, 1,241 and 2,531 bytes of code through a bound, and to 488, 756 and 1,301 bytes through `&var Counter`. Containers keep their allocator as a handle for this reason: the allocator is called rarely next to the work around it, and a container's type should not depend on which allocator made it.

### Returning references

A function may return `&T`, `&var T`, or the optional reference `&T?`, with no lifetime written anywhere: the signature alone says what the result borrows from. It is the receiver when the function takes one by reference, `&self` or `&var self`; otherwise it is the one parameter that is a reference or a slice. A receiver taken by value lends nothing, and neither does a variadic parameter, whose slice the call builds. Any other signature is the error `cannot tell what the returned reference borrows from`, with `help: return an owned value`, and there is no annotation that names a source instead. An operator, `[]` included, cannot return a reference, because it is called by its spelling rather than by a name a borrow can be followed through, so an accessor that lends an element is a method such as `At`.

```rux
extend Slots<T> {
    func At(&self, index: uint) -> &T { return self.items[index]; }            // borrows self
    func AtMutable(&var self, index: uint) -> &var T { return self.items[index]; }
    func Find(&self, index: uint) -> &T? {                                     // none when missing
        if index >= self.count { return none; }
        return self.items[index];
    }
}
func First(items: int32[..]) -> &int32 { return items[0]; }                  // the only slice parameter
func Longer(a: &String, b: &String) -> &String { ... }                         // error: two possible sources
```

Inside the function every `return` yields a reference derived from the source: the source itself, a field or element of it, a reference that a call returned from it, or a local reference holding one of those. A reference to a local, to a parameter taken by value, or to a temporary is `cannot return a reference to local 'copy'` or `cannot return a reference to a temporary value`, and one borrowed from another reference parameter is `cannot return a reference borrowed from 'other'`. `&var T` needs storage the function may write, so `return self.second;` from a method taking `&self` is `cannot return a part of immutable 'self' as '&var int32'`. Storage reached through a raw pointer, inside `unsafe`, is the user's to vouch for and may be returned as a reference to the receiver's elements, which is how a container lends what its allocation holds. A function returning `&T?` returns `none`, or a reference, which is made present.

At the call site the result is second-class, as every reference is: it lives in a `let` local, is passed on to a reference parameter, or is returned again under the same rule, and it is never stored in a field, an array, or a tuple. A Copy primitive scalar is read from it as from any reference, so `let first: int32 = pair.First();` copies the value. While the result is in use, the argument passed for the source is borrowed: a `&T` result keeps it from being written or moved, and a `&var T` result from being used any other way, until the result's last use. The borrow is the one a local reference holds, so `let first = pair.First(); pair.first = 3; Read(first);` is `cannot modify 'pair.first' while it is immutably borrowed`, while the same write after the last use of `first` is accepted. The source argument must be storage that outlives the statement, a place or a reference, so `Make().First()` is `the returned reference would borrow from a temporary value`. Only a function called by name returns a reference: a function value that would return one is `cannot call a function value that returns reference '&int32'`, since nothing about the value says what its result borrows.

`&T?` is spelled with the suffix after the referent, and a reference to an optional is `&(T?)`. An optional reference may be a return type and the type of a `let` local, and nothing else: as a parameter or field it is `function parameter cannot be optional reference '&int32?'`. Its representation is an ordinary optional whose payload is the address. `found ?? fallback` yields a reference, the payload's or the fallback's, so the fallback must be a place or a reference too; `if found is .Some(value)` and a `match` arm `.Some(value)` bind a reference. Each of them borrows what the optional borrowed: when the fallback lies elsewhere, the result of `??` is taken to borrow every storage in use, while a fallback that leaves, such as `?? return` or `?? Panic("missing")`, adds nothing. `?` does not propagate the absence of an optional reference, because the payload it would leave in the enclosing expression could not keep its source borrowed; match it, or use `??`.

Raw pointers remain the deliberate mechanism for FFI, nullable or sentinel values, stored addresses, pointer arithmetic, and unsafe memory APIs. Address-taking is explicit with `@value`. Moving a value out through a raw pointer is rejected because the pointer does not own the storage it addresses. A cast retypes an address but never adds write access to what a read-only pointer reaches: `@x as *var int` and `@x as *var uint64` on a `let x` are both `cannot cast '*int' to '*var uint64', which would give a read-only pointer write access`. Untyped memory is the explicit escape, since `*opaque` to `*var T` retypes an address nothing typed and `*T` to `*var opaque` writes nothing by itself, as the C functions taking a `void *` they only read declare it.

### First-party raw-pointer boundaries

The package surface keeps raw pointers only where the address itself is part of the contract:

- `C` and the platform binding packages mirror foreign ABIs, including nullable arguments, buffers, handles, and operating-system-owned storage.
- `Memory` and `Allocator` traffic in untyped blocks, alignment, address arithmetic, and ownership transfers that begin or end at an allocation boundary.
- `Core` slices store an address and a length, and nullable `Try*` output slots use null to mean that no destination was supplied.
- `Collections` stores backing blocks, node links, and iterator positions. It uses references for safe call-time access and raw addresses only for stored or ownership-aware internal places.
- `Io`, `Compress`, `Encoding`, `Json`, and `Log` keep a `Reader` or `Writer` handle when a stream must be stored in a field, made from the stream's address inside `unsafe`; a non-escaping interface reference cannot represent that lifetime. Ordinary calls borrow streams and values through references.
- `Allocator` gives the handles of its stateful allocators, `Arena::Handle`, `Pool::Handle` and `FixedBuffer::Handle`, from an `unsafe func`, whose caller promises the allocator outlives every container that keeps one.

Package READMEs identify these retained boundaries beside the APIs that expose them. A new safe parameter or receiver uses a reference unless it has one of the address-level contracts above.

## Unsafe Code

The compiler checks references, bounds, ownership and borrows, but it cannot check what a raw pointer addresses, what a foreign function does, or what an `unsafe func` expects of its caller. The operations that depend on such facts are allowed only in an unsafe context — an `unsafe { ... }` block or the body of an `unsafe func` — so a reader finds every place the program's soundness rests on a promise by searching for `unsafe`.

These operations need an unsafe context:

- reading or writing through a raw pointer: `*p`, `*p = v`, `p[i]`, a range `p[a..b]`, and a field `p.x` reached through a `*T` or `*var T`, including `self.x` where the receiver is `*self` or `*var self`, and `@p.x` or `@p[i]`, which compute an address from one;
- pointer arithmetic that produces a pointer: `p + n`, `n + p`, `p - n`, `p += n` and `p -= n`;
- a cast that gives an address a type it did not have: an integer to a typed pointer, `*opaque` to `*T`, `*A` to `*B`, and an address to a function type;
- calling an `extern` function, an `asm` function or an `unsafe func`, and naming one as a function value, since a call through the value is not marked;
- reading or writing an `extern` variable.

Everything else about raw pointers is safe: taking an address with `@`, `@` of an extern variable, copying, comparing and null-testing pointers, converting a pointer to an integer, and a cast that only weakens what a pointer says — `*var T` to `*T`, the same pointee, or anything to `*opaque`. Each operation outside an unsafe context is an error that names it, with a help pointing at the block:

```text
Src/Main.rux:5:12: error: dereferencing a raw pointer requires 'unsafe'
  note: the compiler cannot prove that a raw pointer addresses a live value of its type
  help: wrap it in 'unsafe { ... }' and say why it holds in a '// Safety:' comment above it
```

The others read `call to extern function 'write' requires 'unsafe'`, `call to unsafe function 'Copy' requires 'unsafe'`, `call to 'asm' function 'Syscall3' requires 'unsafe'`, `accessing field 'next' through a raw pointer requires 'unsafe'`, `indexing a raw pointer requires 'unsafe'`, `pointer arithmetic requires 'unsafe'`, `casting '*opaque' to '*var Node' requires 'unsafe'` and `access to extern variable 'opterr' requires 'unsafe'`.

`unsafe { ... }` is a value block, by the rules of [`if` Values and Value Blocks](#if-values-and-value-blocks): in value position it yields its last expression written without a `;`, so a single read is `let first = unsafe { *p };` and a block can end a value block as its value. At the start of a statement it is the statement and ends at its `}`, as an `if` statement does, so it yields nothing and a value it ends with is an error (`this 'unsafe' block yields a value of type 'int' that is not used`). Followed by a tight `?`, `.`, `??`, `as`, `is` or a binary or assignment operator, it is instead the first operand of an expression statement, in value position: `unsafe { Find(p) }?;` propagates, and `unsafe { *p } + 1 == total;` compares. A token that could also start the next statement, such as `*`, `-`, `(` or `[`, ends the block as before. A block is otherwise exactly the statements it holds: its locals are destroyed at its end, `return`, `break` and `continue` inside it leave as they would anywhere, and `defer unsafe { ... }` defers it. Nothing about a program's behavior changes; the block only grants permission. An `unsafe` block that holds no operation needing it, or one written inside another unsafe context, is a warning, `unnecessary 'unsafe' block`.

`unsafe func` declares a function whose caller carries an obligation the compiler cannot check, such as `Memory::Copy`, whose ranges must be readable and writable for `length` bytes. A call needs an unsafe context, and the body is one. The keyword stands directly before `func`: `pub unsafe func`, a method in an `extend` block, and `pub intrinsic unsafe func`, as the `Core` atomic intrinsics and `Zeroize` are declared. An `extern` declaration and an `asm` function are always unsafe to call, so `unsafe` on one is an error, as it is on anything but a function or a block. A destructor, an operator and an interface requirement or its implementation cannot be `unsafe`, because each is called where no call is written or through an interface that does not show which implementation it reaches: `destructor '~Holder' cannot be 'unsafe', because nothing marks where it is called`, `interface requirement 'Area' cannot be 'unsafe'`. A function whose safety does not depend on its caller stays safe and keeps its raw operations in `unsafe` blocks inside its body.

`rux lint` asks for the reason. Rule `unsafe.comment` warns about an `unsafe` block unless a comment run ending on the line directly above the line holding `unsafe` has a line starting `Safety:`, and about an `unsafe func` unless one does above its declaration, attributes in between allowed — usually a `/// Safety:` line of its documentation, which states what a caller must uphold. A comment at the end of the previous line, or one separated by a blank line, does not count. The comment says why the operation is sound at that place, such as `// Safety: index is below self.length, checked above`, and `#Allow("unsafe.comment")` on a function silences the rule for its body.

## Strings and Text

### Primitive types and associated constants

Every primitive type is built into the compiler: `int8` through `int512` and `uint8` through `uint512`, the eight floating-point widths `float8`, `float16`, `float32`, `float64`, `float80`, `float128`, `float256` and `float512`, `bool8` through `bool512`, `char8` through `char512`, the pointer-sized `int` and `uint`, `opaque`, and the four built-in aliases `bool` (`bool8`), `byte` (`uint8`), `char` (`char32`) and `float` (`float64`). They are usable in every package without an import, and no package declares them. A built-in name is reserved in every package and module, so `type bool = int32;` or `struct int8 {}` is rejected with "type 'bool' is already declared in this scope". Locals, parameters and fields may still use the spelling.

A package adds associated constants to a primitive with an ordinary extension. Core supplies the standard ones; another package can supply its own, and the compiler gives Core's package name no privilege.

```rux
/// A signed integer in 8 bits, two's complement.
/// @see https://rux-lang.dev/docs/api/core/integers#int8
extend int8 {
    pub const Min: int8 = -128i8;
    pub const Max: int8 = 127i8;
}
```

An extension names a primitive by any spelling, and an alias extends its canonical width: `extend byte { ... }` is `extend uint8 { ... }`.

Associated constants are source members, visible where they are imported:

- A package sees its own primitive extensions in every file, with no import.
- `import Core::int8;` imports Core's extensions of `int8` into that file. The module the path names must extend `int8`, or the import is rejected with "package 'Core' declares no extension of primitive type 'int8'". A primitive needs no import as a type, only for its constants.
- An alias spelling imports its canonical width: `import Core::byte;` makes both `byte::Max` and `uint8::Max` available, and they are the same constant.
- A glob import, `import Pkg::Module::*;`, imports the extensions of every primitive that module extends.
- A user alias such as `type Tiny = int8;` reaches the extensions in force where it is declared and those imported where it is used, so `Tiny::Max` works wherever `Tiny` is visible.
- An alias of a generic type's instantiation names the constants an extension of that instantiation declares: with `type Hits = Atomic<uint64>;`, `Hits::Zero` is the `Zero` of `extend Atomic<uint64>`, which is how a module variable of a generic type starts as a constant.
- Depending on Core without importing exposes nothing, and a dependency's imports never reach its consumers.
- When two imported extensions declare the same constant, a use of it is ambiguous and is rejected; importing both is not.

Constants retain normal package visibility. Their initializers are checked in the declaring package, not the caller's scope. Expressions, constant initializers and `when` conditions resolve them identically.

The `intrinsic type` declaration is removed: `intrinsic type int8;` is rejected with "intrinsic type declarations have been removed; primitive types are built in" and help naming the extension that replaces it. Slices and ranges are native types, with no source declaration controlling their representation. An ordinary struct never acquires compiler-owned behavior from its name.

### Intrinsic declarations

An intrinsic declaration names a function, constant or value the compiler supplies. It does not load a package or give its declaring package special privileges. Core supplies the standard declarations; another package can supply its own.

Extension blocks may contain ordinary typed constants as well as methods. The only compiler-supplied associated constant values are the floating-point limits `Max`, `Lowest`, `MinPositive`, `Epsilon`, `Infinity` and `NaN`, at every float width: `pub intrinsic const Max: float128;` declared without an initializer inside an extension of `float128` takes the largest finite `float128` from the compiler, and the declared type must be the extended type. `MinPositive` is the smallest positive normal value, `Epsilon` the distance from one to the next value above it, and `NaN` a quiet NaN. Any other name is rejected with "intrinsic associated constants support only floating-point Infinity, NaN, Max, Lowest, MinPositive and Epsilon". Core declares `Infinity` and `NaN` this way at every width and all six for `float80` and wider; other limits and metadata belong in ordinary source initializers. A `when` condition retains each floating-point width and uses exact compiler arithmetic, so finite limits, infinities, NaNs and values beyond the `float64` range work at every width. An unsuffixed float literal in a typed float constant's initializer is read at the constant's own width, so `const Single: float32 = 1.5;` is the `float32` nearest 1.5 in a condition as it is at run time. Integer casts in conditions retain or truncate all bits at the requested width. A cast to a character width never truncates: it is checked by the width's own rule, and a value the width cannot hold makes the condition invalid.

`intrinsic func` names a function the compiler implements itself, so only the ones it knows are accepted: `ConvertBits`, `Zeroize`, `Assert`, `DebugAssert`, `Panic`, `#Error`, and `#Warn`, the methods `Target.HasFeature`, `Compiler.HasFeature`, `Config.Get`, and `Config.Has`, and a `Truncate<T>` method in an extension of an integer type, which converts to the integer type `T` without the range check `as` adds. Any other name is rejected with `'Frobnicate' is not a supported intrinsic function`. The compiler emits each call for one signature, so a declaration must match it exactly: `Assert` and `DebugAssert` take `(condition: bool, message: char8[..])`, `Panic`, `#Error`, and `#Warn` take `(message: char8[..])`, and none of them returns a value.

`intrinsic #name: Root;` declares a value the compiler supplies, where `Root` is one of `Target`, `Build`, `Source`, `Compiler`, and `Config`. The struct `Root` names may declare any subset of the fields the compiler fills in for that root, and no others, and every declared field must have the type the compiler supplies. A `struct Target { wordSize: uint; }` behind `intrinsic #target: Target;` is rejected because nothing would fill `wordSize`; declaring `pointerBits` as anything other than `uint` is rejected because lowering supplies that field as a `uint` value.

### Writable sequence views

`T[..]` is a read-only slice and `var T[..]` is a writable slice. Both contain a data pointer and an element count. The storage must outlive the view, and within a function the compiler checks it: a value that views an array the function holds — a local, or a parameter passed by value — cannot be returned or stored through a pointer, a reference or into a module variable, whether it is a slice of the array, the array itself converted to a slice, a local given such a view, or a structure, array or tuple built from one. A call's result that holds views is taken to view whatever its view-holding arguments and its receiver view, since nothing in a signature says which one a returned view borrows, so `spec.WithOptions(local[..])` returned from a function is refused too. Copying a view copies its descriptor, not its elements.

```rux
var values: int[3] = [1, 2, 3];
let writable = values[..];
writable[1] = 4;
let shared: int[..] = writable;
let empty: var int[..] = [];
```

Writability belongs to the view: a `let` binding can write through a writable view, while a `var` binding of a read-only view cannot write its elements. Writable slices implicitly weaken to read-only slices; the reverse is rejected. Viewing or spreading an existing array requires the same element type: an `int32` array cannot become an `int64` slice, even though an individual `int32` can widen to `int64`. Such a view would read the original storage with the wrong stride. Array views inherit the writability of the array place, and sub-slices preserve their parent's writability.

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

Character literals hold one value. An unprefixed `'A'` has type `char32`; `c8'A'`, `c16'A'`, `c32'A'`, `c64'A'`, `c128'A'`, `c256'A'`, and `c512'A'` select their respective character widths. `char64`, `char128`, `char256` and `char512` store the same Unicode scalar values as `char32` in 8, 16, 32 and 64 bytes, so `c64'😀'` and `c64'\u{1F600}'` have the same value, and `c512'😀'` holds that same scalar in a wider type. Prefixes work in constants, generic arguments, and match patterns. Identifiers named `c64`, `c128`, `c256` and `c512` remain valid; none of the four is a string encoding.

A prefixed literal holds exactly one character of its width and is never truncated. `c8` takes a character that is one UTF-8 code unit, U+0000 through U+007F; `c16` takes one UTF-16 code unit that is not a surrogate, up to U+FFFF; `c32` and every wider prefix take any scalar value. `c8'é'` and `c16'😀'` are rejected with `character 'é' (U+00E9) does not fit one 'char8' code unit`: a character that needs several code units is written as a string literal such as `c8"é"`, and a single byte such as `0xE9u8` is written as an integer. An unprefixed `'a'` takes `char8` or `char16` from its destination by the same rule, so `let accent: char8 = 'é';` is rejected with the same message.

Character literals accept the existing simple escapes and `\u{...}` with one to eight hexadecimal digits. Empty literals, multiple characters, invalid escapes, surrogate scalars, and scalars above U+10FFFF are rejected. Character patterns compare decoded values, so a Unicode escape matches its literal spelling and equivalent unguarded arms are duplicate patterns.

A scalar width widens implicitly to any scalar width at least as wide — `char32` to `char64`, `char64` to `char128`, and on up to `char512` — because each carries the same scalar value. The code-unit widths `char8` and `char16` widen to nothing. Every other conversion between character widths, and every conversion from an integer, is written with `as`. A constant cast to a character width is checked against that width's rule, wide integer constants included, so a scalar width rejects a constant above U+10FFFF or in the surrogate range, and at run time the same rule stops the program with `Panic: invalid character cast`, so no cast leaves a `char64` or wider holding a number no character has.

### Range types

Range expressions and annotations use the same punctuation family. Bounds in a two-sided range have one type. Bounds written with different integer types meet in one, as an operator's operands do: an unsuffixed literal or constant takes the other bound's type when it fits, and otherwise the range takes the type of the bound that holds every value of the other, so with `a: int32` and `b: int64`, `a..b` is an `int64..int64`. Bounds of which neither holds the other, such as `int32` and `uint32`, are an error (`range bounds 'int32' and 'uint32' have no common type`), and so is a literal bound that the other bound's type cannot hold, as in `0u8..300`.

| Expression | Type        | Members          |
| ---------- | ----------- | ---------------- |
| `1..4`     | `int..int`  | `.start`, `.end` |
| `1..=4`    | `int..=int` | `.start`, `.end` |
| `1..`      | `int..`     | `.start`         |
| `..4`      | `..int`     | `.end`           |
| `..=4`     | `..=int`    | `.end`           |
| `..`       | `..`        | none             |

Range members are compiler-owned and need no import. Inclusive ranges include their final bound; ordinary two-sided ranges exclude it. `..=` is the only inclusive spelling, in expressions and patterns alike: `1...5` is `'...' is not a range operator` with the help `use '..='`, and `...` keeps only its other roles, the variadic marker `args: &Display...` and the spread `values...`. A `for` over an inclusive range visits its end last and stops there, even when the end is its type's maximum, so `for v in start..=255` over `uint8` never wraps around; a range whose start is past its end visits nothing. The full range selects an entire array or slice. A pointer requires an explicit end.

An extension of one instantiation of a generic type, such as `extend Pair<int32>`, gives methods to that instantiation alone: calling one on a `Pair<int64>` is `type 'Pair<int64>' has no method 'Sum'`, with a note naming the extension it is declared in. Extensions of two instantiations, such as `extend Pair<int32>` and `extend Pair<int64>`, may each declare a method of the same name and parameters, since no receiver can call both; a method of the same signature in `extend Pair<T>` competes with them and is `function 'Sum' has the same parameter signature as an earlier overload`. `extend Pair<T>` gives methods to every instantiation, and `extend Box<T> : Named` makes every instantiation of `Box` a `Named`: a `Box<int32>` converts to a `Named` interface value or a `&Named` borrow, through a table of the `Box<int32>` instance's methods that the module converting it carries.

Concrete extensions such as `extend int[..]` and `extend char8[..]` have separate method sets. A writable view, `var int[..]`, also calls the methods of `extend int[..]`, as it is passed wherever an `int[..]` is taken, while the methods of `extend var int[..]` stay off read-only views. A fixed array has a method set of its own, keyed by its element and its length: `extend int[3]` gives methods to `int[3]` and to no other array. Generic slice extensions such as `extend T[..]` are rejected; generic reusable algorithms remain ordinary functions.

## Copy and Move

Copy and move are different operations:

```text
=          copy, or initialize or replace a destination
move x     move: the source 'x' is consumed
```

`=` always initializes or replaces its destination; `move` always says the source is consumed. A named source is copied unless it is written `move source`, and only plain data copies. A named move-only source in any by-value context — a binding, argument, receiver, return, assignment, aggregate, field shorthand, or conditional arm — is therefore an error without `move`: `cannot copy 'String', which owns resources`, with `help: write 'move name' to transfer it, or 'name.Clone()' to copy it`, or `cannot copy 'Ticket', which prohibits copying` for a `#NoCopy` type, whose help offers the transfer alone. The help offers `Clone()` only for a type that has one; an owning type without it, such as Collections' `BitSet`, is offered the transfer alone. A note names the context, and the help writes the transfer the way the context needs it, such as `match move outcome`, `(move outcome)?`, or `buffer: move buffer`. A fresh temporary transfers directly because no visible source remains afterwards, so it needs no `move`:

```rux
let copy = value;
let moved = move value;
destination = move moved;

Consume(value);          // copy
Consume(move value);     // move
Consume(MakeValue());    // direct temporary transfer
return move buffer;
let (left, right) = move pair;
match move outcome { ... }
```

`move` is a prefix operator with the precedence of every other unary operator, so `move a.b` moves the field `a.b`, `move a + b` is `(move a) + b`, and `(move x)?` moves the optional before propagating it. `<-` is not an operator: `let x <- y;` is `'<-' is not a binding operator` with `help: write 'let name = move source'`, `a <- b;` is `'<-' is not an assignment operator` with `help: write 'target = move source'`, and a prefix `<-v` is `'<-' is not an operator` with `help: write 'move value' to transfer ownership`. `move` is a reserved word, so it cannot name a binding.

Copying never invalidates its source. Moving invalidates the source, even a copyable one, suppresses its later destruction, and makes every subsequent read a compile error. A move is always a byte copy of the value followed by forgetting the source: no user code runs, and the source's destructor does not run, so the value is destroyed exactly once, by its last owner. A live borrow of the source blocks the move (`cannot move 'buffer' while it is immutably borrowed`), a reference cannot be moved from (`cannot move a non-owning reference`), moving a place into itself is an error, and so is moving one field out of a droppable value. Assignment to a live destination releases that destination's old state at the operation's defined replacement point. Initializing previously uninitialized storage performs no prior destruction.

A field, a tuple element, and an element of an array or a slice are destinations like any other: `holder.item = move value`, `pair.0 = other`, and `items[i] = move value` produce the new value, destroy the value the place held, and install the new one, whether the place is part of a local, nested inside further fields and elements, or reached through a `&var` parameter, receiver, or alias, or through a slice. A part of a local holds a value only while the local owns one, so a droppable local declared without a value, or moved from, has to be assigned whole before any of its parts is written — see [Construction and Initialization](#construction-and-initialization). A write through a raw pointer — `*p = move value`, `p[i] = value`, or a field reached through `*var T` — initializes the storage it addresses and destroys nothing, because a pointer cannot tell whether that storage holds a value yet; code that replaces a value through a pointer destroys or moves out the old one first. A `v[i] = x` that calls a declared `[]=` is a method call, and what it replaces is up to that method.

A type is **copyable** when it is plain data: it declares no destructor, every field, element and payload it holds is copyable, and it is not `#NoCopy`. Numbers, `bool`, characters, raw pointers, `T[..]` views, function values, and structures, tuples, variants, unions, optionals and fixed arrays `T[N]` of copyable types are copyable, and `=` from a named one copies its bytes. Every other type is **move-only**: a type with a destructor owns resources, and so does anything holding one, so copying its bytes would leave two owners to release them. Copyability is decided once per type, when it is classified, and recorded in the semantic model. A move-only value is duplicated only by an ordinary method, by convention `Clone(&self) -> Self`, called where the copy is wanted; for `String`, `Clone` allocates, and running out of memory panics as every allocation in a collection or a string does. A type moves when every field can, and the compiler generates the move. Two attributes on a struct, variant or union take a capability away from the type and from everything that holds it:

```rux
#NoCopy
pub struct RawHandle { fd: int; }              // plain data, but never duplicated, so it moves instead

#NoMove
pub struct ListNode { next: *var ListNode; }   // points into itself, so it stays where it was built
```

A `#NoCopy` value is move-only even with no destructor, which suits plain data that must not be duplicated, such as a handle a destructor does not release. A `#NoMove` value cannot be moved either, so `#NoMove` implies `#NoCopy`: moving one is `moving type 'ListNode' is prohibited`. Such a value is still created in place from a temporary — `let node = ListNode { next: null };` or `let node = MakeNode();` — because nothing can have taken a temporary's address, and it is shared through a pointer. Both attributes take no arguments and are written without parentheses, and either may be written once per declaration.

A bodyless canonical operator prohibits nothing: `func =(&var self, other: &T);` inside `extend T` is `a bodyless 'func =' does not prohibit copying`, with `help: mark 'T' with '#NoCopy' instead`, and a bodyless `func <-` is rejected the same way in favour of `#NoMove`. Bodyless ordinary functions in interfaces remain interface requirements. A move operator with a body is rejected with `custom move hooks are not supported` and `help: a type that must stay in place uses '#NoMove'`: a move never runs user code.

`=` never runs user code. A copy operator with a body, `func =(&var self, other: &T) { ... }` or one taking another source type, is rejected with `custom copy hooks are not supported` and `help: implement 'func Clone(&self) -> Self' on 'T' and call it where a copy is needed`. Assignment produces its new value first, then destroys the old destination and installs the result: `value = MakeValue();`, `value = move other;` and `value = other.Clone();` each transfer a value nobody else holds, which is destroyed once, with its new owner.

Generic code may copy a value of a type parameter only under `T: Copyable`; otherwise it moves the value, or calls `Clone` under `T: Cloneable`. A copy of an unbounded `T` is checked for each instantiation: instantiating it with a move-only type reports `cannot copy 'Owner', which owns resources` at the copy in the generic body, with a note naming the instantiation and a help that offers the `Copyable` bound. A `T: Copyable` bound moves the check to the call, which is refused for a move-only type argument without reporting the copies in the body again.

## Construction and Initialization

An associated or instance method may declare its own type parameters, including on a non-generic type. For example, `Layout::ForValue<int32>()` selects a method type argument; a method with a value parameter can infer it from the argument. Instance calls such as `holder.Convert<int64>(value)` take receiver parameters from `holder` independently. For associated calls on a generic type, write the receiver arguments first and then the method arguments, as in `Holder::OtherWidth<int32, int64>()`. Method bounds are checked at the call and concrete instantiations retain their own parameter, result, and symbol identities.

Inside `extend T`, a receiverless function named `T` that returns exactly `T`, or `T` with a `throws` clause, is a constructor candidate:

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

Constructors are called as `String(...)` or `Vector<int32>(...)`, not through an implicit conversion. Inside the type's own `extend` block, `Self(...)` calls one with the block's type arguments. A constructor that can fail declares `-> Self throws E`, and its call is a throw site like any other: `let number = try JsonNumber(allocator, text);`. Descriptive factories such as `FromBytes`, `WithCapacity` or `Connect` name a specific source and keep their names, and a lookup that may find nothing returns `T?`.

A struct literal or a case literal with named fields may write a field as its name alone when a binding of that name holds its value: `Point { x, y }` is `Point { x: x, y: y }`, and `Shape::Rectangle { width, height }` and `Self { data: null, allocator }` read the same way, so construction reads as the matching pattern does. The value is an ordinary use of the binding, so it is copied or moved by the ordinary rules and the shorthand never moves on its own: `Holder { buffer }` with an owning `buffer` is `cannot copy 'Buffer', which owns resources`, with the help `write 'buffer: move buffer' to transfer it`, followed by `, or 'buffer.Clone()' to copy it` when `Buffer` has a `Clone`. A name with no binding in scope is reported as any unknown name is.

`var value: T;` invokes `T()` when an accessible default constructor exists. With no `T()`, the declaration remains legal but denotes compiler-tracked uninitialized storage. Reading or destroying that storage before definite initialization is an error. Taking its writable address with `@value` counts as initializing it, because the address exists to be filled through: `var info: SystemInfo; GetSystemInfo(@info);` leaves `info` usable, and the compiler trusts the callee to have written a whole value. The same holds for a local that was moved from, so `@value` inside a loop gives each pass a value of its own. Construction and assignment are distinct: a constructor creates a value, while `=` replaces or initializes storage according to its current state. Constructor lookup is never a general hidden conversion rule.

A local's drop flag covers the whole value, so writing one of its parts cannot make it live. Writing a part — a field, a tuple element, or an element of a fixed array, at any depth — of a local that is not definitely initialized, because it was declared without a value and not yet assigned whole or because it was moved from on some path, is an error when the local's type needs destruction: `cannot write field 't' of 'empty', which holds no value`. Taking the writable address of such a part with `@` is the same error. A value written there would never be destroyed, so the local is assigned whole instead — `empty = Holder { ... }` or `empty = move other` — and from then on its parts are replaced like any other place. A type with nothing to destroy, such as a plain structure of numbers or an array of them, may still be initialized one part at a time. A part that has been written, or one inside it, may be read at once, while any other part and the whole value may not: after `var q: Point; q.x = 1;`, reading `q.x` is fine, but `q.y`, `let r = q;`, and passing `q` are rejected with `variable 'q' is used before it is initialized`. The local counts as initialized once every field and tuple element, at any depth, has been written on every path that reaches the use, or one member of a union. A fixed array of such elements counts as initialized from its declaration, alone or as a part of such a local, so a loop can fill it. A type that still depends on a type parameter may need destruction once instantiated and follows the stricter rule. Writes through a `&var` reference, a `&var self` receiver, a slice, or a raw pointer do not write into a local's own storage and are unaffected.

### Repeated fixed-array initialization

`[value; count]` constructs a fixed inline array by evaluating `value` exactly once and copying the result into every element. `count` must be a non-negative compile-time integer. The expression has type `T[N]`, so both the element type and extent can be inferred, while an explicit destination type contextually types the repeated value:

```rux
let inferred = [0u8; 16];
let contextual: uint8[16] = [0; 16];
```

The element type must be copyable, so it is plain data with nothing to destroy. Construct move-only elements explicitly or initialize mutable array storage in a loop instead. A zero count still evaluates the value once. Repeated arrays use the same fixed-array-to-`T[..]` coercion as ordinary array literals.

## Errors: `throws`, `throw` and `try`

A function that can fail declares the error after its return type, or after its parameters when it returns nothing, and the error may be a sum:

```rux
variant DigitError { Blank, NotADigit(char) }

func ReadDigit(c: char) -> int throws DigitError {
    if c == ' ' { throw .Blank; }
    if c < '0' || c > '9' { throw .NotADigit(c); }
    return (c as int) - ('0' as int);
}

func Close(&var self) throws IoError { ... }
func Load(path: char8[..]) -> Config throws IoError | ParseError { ... }
```

Errors are values, not unwinding. A function that throws returns its success or its error as one tagged value, so each call of it is the call, a test of the tag and a branch: to the code that continues on success, and on failure to the handler around the call within the same function, or out of the function as an ordinary return. Nothing walks the stack, a throw costs what a return costs, and a function that does not throw returns its plain type and pays nothing. The reasons are recorded in the [errors decision table](NativeOutcomes.md#errors).

No fallible type can be written. `-> T ! E`, `-> ! E` and `T ! E` anywhere a type is expected are `'T ! E' is not a type`, with `help: declare the error after the return type, as in '-> T throws E', or store an outcome in 'Result<T, E>'`. `throws` belongs to signatures: to functions and methods, to function types, `func(int32) -> int32 throws Closed`, and to interface requirements, which an implementation matches exactly, error included. A constructor that can fail declares `-> Self throws E`, as [Construction and Initialization](#construction-and-initialization) describes, and its call is a throw site like any other. `Main` may declare `throws E`, with an `int` result or none; an error that reaches it runs every defer and destructor on the way out and ends the program with status 1, without printing the error.

### Throwing

`throw value;` sends an error to the innermost handler around it, or else out of the function through its `throws` channel, where the value converts to the channel's type as a `return` value converts to the return type, so a member of a sum channel is injected. It is also a whole arm body and the right side of `??`, as in `lookup ?? throw NotFound {}`, where it produces no value. A case written without its type takes the error type of where it goes: the `throws` clause, or the errors the `try` block around it has seen so far; when that is a sum, write the owner, as in `throw ParseError::Invalid(at)`. A unit error is thrown as `throw ();`. `throw` needs no `try`, and outside a handler it needs a function that throws: otherwise it is `'throw' needs a function that throws, but 'Parse' returns 'int'`, with `help: declare 'throws E' on the function, or throw inside 'try { ... } catch { ... }'`. `fail` is not an alternative spelling: `fail value;` is `'fail' is not supported` with `help: write 'throw value;'`, and `fail` stays a reserved word.

### Marking throw sites

Every call that can throw is marked. A call of a `throws` function, of a throwing method, constructor, interface requirement or operator, is a _throw site_, and it must be covered by `try` in one of four forms:

| Form                         | Where the error goes                                                                     |
| ---------------------------- | ---------------------------------------------------------------------------------------- |
| `try expr`                   | To the innermost handler around it, or out of the function through its `throws` channel. |
| `try expr catch { arms }`    | To the arms, which produce a value of the operand's type or leave.                       |
| `try? expr`                  | Dropped: the result is the operand's value made present, `T?`, or `none`.                |
| `try { ... } catch { arms }` | To the arms. Every statement in the block is covered by the block itself.                |

A `try` block covers every statement in it, those in nested blocks included, but not a deferred statement, which runs as the scope is left, nor the arms of a handler inside it. `try` has the lowest precedence of any operator, so it covers the whole expression to its right: `try ReadDigit(t) * 10 + ReadDigit(u)` marks both calls, `let x = try A().B();` marks both `A` and `B`, and `(try? F()) ?? fallback` needs its parentheses. It is written where an expression starts, including the value of an assignment, `total = try Next();`, and never to the right of another operator (`x + try F()` is `'try' cannot appear to the right of an operator`, with `help: write 'try' at the start of the whole expression, as in 'try x + F()'`). A call that throws evaluates to its success, so `try MakeCounter().Doubled()` continues on the counter. Evaluation stops at the first throw site that fails; nothing to its right runs.

An unmarked call is `call to 'ReadDigit' can throw 'DigitError' but is not marked with 'try'` with `help: write 'try' before the expression`, so an error can never be stored, ignored or matched by accident; a `try` that covers no throw site is the warning `'try' has no call that can throw`. A postfix `?` does not pass an error on: `ReadDigit(c)?` is `'?' does not propagate errors`, with `help: write 'try' before the expression to pass the error on`, because `?` only propagates the absence of an optional. A call through a function value whose type declares `throws` is a throw site like any other, so it is marked the same way.

### Passing errors on

An error that a throw site raises and no handler takes leaves through the function's `throws` channel. It must fit the channel by identity, as a member of a sum, or as part of a larger sum; it is never converted. Otherwise it is `call to 'ReadDigit' can throw 'DigitError', but 'Load' throws 'IoError'`, with a note that an error passes on only as itself, as a member of a sum, or as part of a larger sum, and a help suggesting `try ... catch` or adding the error to the `throws` clause; in a function that does not throw it is `call to 'ReadDigit' can throw 'DigitError', but 'Main' does not throw`. A different error type is mapped explicitly, as below.

An error leaving the function follows ordinary return behavior: it is captured before the registered defers run in LIFO order, and then ownership cleanup runs. Live locals, and the completed components of an aggregate whose construction the throw interrupted, are destroyed exactly once. The success moves onward and the error moves to its handler or the caller with their move operations, so a payload whose move is prohibited, or a reference payload, is rejected at the throw site, including when a generic is instantiated.

### Handling errors

A handler takes the sum of the errors thrown to it, and its arms are exactly the arms of `match error { ... }`: patterns, `_`, alternatives, guards, typed patterns over a sum, block arms that yield their last expression, and a trailing comma. They must cover every error (`'catch' arms do not cover every error of type 'ParseError'`, with `help: add a '_' arm, or bind the whole error, as in 'e => ...'`), so passing an error on is an arm of its own, `e => throw e`:

```rux
func LoadWithDefaults() -> Options | Defaults throws IoError {
    return try Read() catch {
        _: ParseError => Defaults {},
        e: IoError => throw e
    };
}
```

Each arm of a `try ... catch` expression either produces a value that converts to the operand's type or leaves through `throw`, `return`, `break`, `continue`, a call to `Panic`, or a no-return function; a leaving arm never decides the result type. The type the context expects of the whole expression is the operand's and every arm's, so `let kept: Result<int, DigitError> = try .Success(ReadDigit(c)) catch { e => .Failure(e) };` builds both cases without naming the variant. A block arm completes with `()`, so `try Close(file) catch { _ => {} };` is valid only for a call that returns nothing, and a non-unit success never acquires an invented fallback value (`a block arm completes with '()', but 'catch' must produce a value of type 'int32'`). The arms of a `try` block statement run for their effect, as the arms of a match statement do.

An arm is outside its handler: an arm's `throw`, or a call it marks with `try`, goes to the next handler out or to the caller. `break`, `continue` and `return` leave a `try` block as they leave any block. An arm that binds the error owns it, so an error whose move is prohibited cannot be bound by value. A `try` block needs `catch` arms (`a 'try' block needs 'catch' arms`, with `help: add 'catch { e => ... }' after the block, or write 'try' before a call to pass its error on`), `catch` follows only `try` (`F() catch { ... }` is `'catch' must follow 'try'` with `help: write 'try value catch { ... }'`), and `try?` takes no arms.

`try?` handles every error of its operand by dropping it: the error is destroyed where it is dropped, and the result is the operand's value made present, or `none`. A value that is already optional gains a level, so `try? Lookup(key)` on a `-> int32? throws E` is an `int32??`, and a call that returns nothing gives `()?`.

Ownership follows every path an error can take: the state of each local at a throw site is the state the arms start from, so a value moved after one call that throws and before another is possibly moved in the arms. An arm reads and moves surrounding locals under the ordinary rules; a local moved only inside an arm that leaves is still owned on the path that continues after the handler.

### Mapping errors

Adding context to an error, or converting it to the channel's type, is an arm that throws the mapped value:

```rux
func LoadConfig(path: char8[..]) -> Options throws ConfigError {
    let value = try ReadAt(path) catch { e => throw ToConfigError(e, path) };
    return value;
}
```

On success the arm never runs and the success continues unchanged. On failure `e` owns the complete error, including every member of an error sum, and the arm runs once; a `match` in it maps different members differently. The arm's own `throw` leaves the function without being mapped again. The removed `value? else (e => mapped)` is `'? else' is not supported`, with `help: write 'try value catch { e => throw mapped }'`.

### Cleanup and `finally`

There is no `finally` (`'finally' is not supported`, with `help: use 'defer' inside the try block`), and `finally` is reserved. Code after a `try { } catch { }` whose arms do not leave always runs; cleanup that must run on every exit is a `defer` or a destructor. When an error leaves a `try` block, the defers registered inside it run and its locals are destroyed in the order a return runs them, and the completed parts of a value built only in part are destroyed, as are the arguments a call already has in hand when a later argument throws (`Take(Tracked { ... }, try Parse(text))` destroys the `Tracked`), before the arm runs. A deferred statement and a destructor run while their scope or value is already being left, so no error may leave either: a call in them that can throw is written `try?` or `try ... catch`, as [Return and Deferred Cleanup](#return-and-deferred-cleanup) describes.

### Keeping an outcome

An error is handled at the call or passed on, so nearly every call leaves nothing to store. An outcome that has to outlive the call — kept in a field or a container, sent elsewhere, or decided later — is Core's `Result<T, E>`, an ordinary `variant Result<T, E> { Success(T), Failure(E) }` with no compiler privileges. It is built with `try .Success(F()) catch { e => .Failure(e) }`, asked about with `is`, as in `if stored is .Success(_)`, which borrows rather than consumes it, and taken apart with `match`. `?`, `??` and `try` do not apply to it, and a variant with `Success` and `Error` cases, whatever it is called, is equally ordinary: `?` rejects it with a note that it is an ordinary variant and `help: declare 'throws E' and pass errors on with 'try', or match the variant`.

## Optional Propagation and Coalescing

### Optional propagation

On a native optional `T?`, `value?` removes exactly one presence level: a present value continues as its payload, which may itself be an optional, and absence returns from the enclosing function. The enclosing function returns an optional `U?`, whether or not it also throws, and the absence returns `none`; any other function is `'?' propagates the absence of 'int32?', but the enclosing function returns 'int32'`, with the help to declare an optional result or supply a fallback with `??`. Absence never becomes an invented error, and no deeper level is a target. The operand is consumed: a named copyable optional is copied, a move-only operand is transferred as `(move value)?`, and a borrowed optional is never an operand. The complete active payload continues, including nested and generic payloads and zero-sized values. Reference payloads, such as that of an optional reference `&T?`, cannot preserve their borrow through `?`, and payloads that prohibit moving are rejected, including when instantiated in a generic; match the optional when the payload needs to be borrowed or copied instead.

A propagated absence follows ordinary return behavior: it runs registered defers in LIFO order and then performs ownership cleanup, and live locals and the completed components of interrupted aggregate construction retain their normal cleanup. Absence propagates only from a native optional: a variant with `Some` and `None` cases is an ordinary variant whatever its declaration is called, and `?` rejects it with a note naming the native form.

### Coalescing

`option ?? fallback` takes the payload of one native optional level, or evaluates `fallback` when it is absent. The fallback is evaluated lazily and must convert to the payload type, or leave: `candidate ?? throw NotFound {}` turns absence into an error, and `?? return`, `?? break`, `?? continue`, and a call to `Panic` are accepted the same way. A present absence is a value: with `nested: int32??`, `nested ?? fallback` yields the inner `int32?`. `??` operates only on an optional, and a borrowed optional is never an operand. An error becomes a fallback through `try?` first, written in parentheses because `try` covers everything to its right: `(try? F()) ?? fallback`.

### Evaluation and ownership

The left operand is evaluated exactly once. The fallback is checked at compile time but evaluated only for absence, so it may perform expensive work or visible side effects without paying that cost when a value is present. A variant with `Some` and `None` cases is not an optional and is rejected as a left operand, as is a raw pointer; a raw pointer may still be the payload of an optional. On an optional reference `&T?`, which a function returning a borrow hands back, `??` yields the reference and keeps what the optional borrowed borrowed, and its fallback is a place or a reference that it borrows rather than consumes, as [Returning references](#returning-references) describes.

Coalescing consumes its evaluated optional. A named copyable optional is copied and remains usable, while a named move-only optional must make the transfer visible:

```rux
let value = (move ownedOption) ?? move ownedFallback;
```

The fallback transfer is conditional, so `ownedFallback` is possibly moved after this expression. `??` associates to the right, and logical `||` binds more tightly, so `first ?? second ?? fallback` means `first ?? (second ?? fallback)`. The operator is compiler-owned control flow: it cannot be declared in an `extend` block or overloaded, and there is no `??=` form. Because `??` is a single maximal-munch token, two adjacent postfix propagations must be written `(nested?)?` instead of `nested??`.

## Iteration

`for item in subject` iterates an array, a slice, or a range directly. Any other subject is driven by the iterator convention: an iterator declares `func Next(&var self) -> Item?` in its `extend` block, and a container declares a parameterless `Iterate` returning such an iterator, so two loops over one container advance independent iterators. Each iteration calls `Next` once; a present result continues the loop with its payload, and only the outer absence ends it. An iterator the loop names as a place — a variable, a field, an element — is advanced where it is, so a loop that stops early leaves it where the walk stopped and a later loop resumes there; the place must therefore be writable, and `for x in counter` over a `let counter` is `cannot advance immutable iterator 'counter'`. Any other iterator, such as a call's result or a struct literal, is a temporary the loop advances. An item may itself be an optional, a fallible, a sum, or the unit: an absent or failed item is an ordinary value of the loop, and no error channel is added to iteration. A `Next` returning a variant with `Some` and `None` cases is rejected where it is declared, because no loop reads such a variant.

A loop body runs again from where its last pass ended, so a value one pass moves out is not there for the next. Moving an outer local inside a loop body is an error unless the same pass gives it a new value before the loop repeats, or leaves the loop with `break`, `return`, or `throw` after the move; the loop's own `for` variable and locals declared inside the body are fresh on every pass.

The loop variable may be a pattern that matches every element — names, `_`, and tuples and structures made of them — which takes each element apart: `for (key, value) in pairs`, `for Point { x, y } in points`, `for (_, value) in entries`. The element is moved into the pattern, so an owning part is the body's to keep or let go of. A pattern that could fail to match, such as `.Some(value)` or `(0, value)`, is `a 'for' pattern must match every element`; such a loop binds the element and tests it in its body with `if item is ...`.

A loop may be labeled, as in `outer: for i in 0..3 { ... }`, so that `break outer` and `continue outer` name it from inside a nested loop. A label names exactly one loop at a time: an inner loop may not reuse the label of a loop around it, because `break outer` inside it would then read as either loop.

## `if` Values and Value Blocks

`if` is a value wherever an expression is expected, and a statement where a statement starts. It replaces the conditional operator: `ready ? a : b` is `the conditional operator 'c ? a : b' is not supported`, with `help: write 'if c { a } else { b }'`:

```rux
let largest = if a > b { a } else { b };

let label = if ready {
    RecordReady();
    "ready"
} else {
    "waiting"
};

let size = match kind {
    .Small => 1,
    .Large => {
        let base = Measure();
        base * 2
    },
};
```

A block in value position — an `if` branch used as a value, or the block of a `match` or `catch` arm — yields the expression it ends with when that expression is written without a ';'. A block that ends otherwise yields `()`, and a block that cannot complete, because it ends by returning, throwing, calling a function that never returns, or leaving a loop, yields nothing and does not decide the type. The yielded value leaves the block before the block's own locals are destroyed, so a local can be moved out as its last expression (`{ let made = Build(); move made }`). Only these blocks yield values: a function body still needs `return`, and a loop body or a nested block keeps every ';'.

An `if` used as a value needs an `else` (`an 'if' used as a value needs an 'else' branch`), and `else if` chains are allowed. The branches unify by the rules for the arms of a `match`, whichever comes first: a block that yields `()` decides nothing while another branch yields a value, an unsuffixed literal takes the type its context gives the whole `if`, as `let r: uint128 = if flag { 0 } else { 1 };` does, and an annotation of an optional or a sum is the type of every branch. A branch that leaves — `return`, `throw`, or a call to `Panic` — does not take part, and two values of unrelated types are an error rather than a value of either. A ';' after the last expression of a block that must yield a value is reported at the ';' (`this block yields '()' because of the ';'`, with the help to remove it) rather than as a distant mismatch. An `if` that ends a value block and yields a value is that block's value. So is a `match` that ends a block in value position, an `if` branch used as a value, the block of an arm of a `match` or `catch` used as a value, or an `unsafe` block used as a value: `let size = if known { let base = Measure(); match base { 0 => 1, _ => base } } else { 0 };`. Ending a statement's block, as the block of an arm of a `match` statement does, a `match` stays a statement.

An `if` at the start of a statement is the statement: its value is not used, so a branch may end with an expression that yields `()`, such as a call that returns nothing, without its ';', and a branch that ends with any other value is an error whose help reads `add ';' or use the value`. The same holds for the block of an arm of a `match` statement. An `if` value is lowered as a two-arm `match` on its condition.

## Indexing

Built-in indexing of an array or a slice is bounds-checked on every target and in every build profile. `a[i]`, as a read, a write, or a compound assignment, stops the program with `Panic: index out of range`, followed by the function, file, line, and column of the subscript, unless `i` names an element; a negative index is never in range. A range subscript is checked the same way: `a[start..end]` requires `start <= end <= a.length`, and an inclusive end must be below the length. A constant index into a fixed array is checked when the program is compiled instead, so `primes[7]` on an `int32[4]` is an error, and so are constant slice bounds past its end, as in `primes[1..9]` (`slice end 9 is out of range for an array of 4 elements`). Indexing or slicing a raw pointer is not checked, because a pointer carries no length; the caller vouches for what it addresses, which is why it needs an [unsafe context](#unsafe-code). An array type always has a length: `T[]` is rejected wherever it is written (`an array type needs a length between '[' and ']'`) with the help `use 'Vector<T>' for a growable array or 'T[..]' for a view`. A release build removes a check it proves always passes.

A type indexes itself by declaring the indexing operators in an `extend` block, the same way it declares `==` or `<`. `[]` reads one element and `[]=` writes one:

```rux
struct Vect {
    data: int[10];
}

extend Vect {
    func [](&self, index: uint) -> int {
        return self.data[index];
    }

    func []=(&var self, index: uint, value: int) {
        self.data[index] = value;
    }
}
```

`v[i]` on such a type is a call to `[]`, and `v[i] = x` is a call to `[]=`. Each has exactly one shape, checked where it is declared: a read takes `&self` and one index and returns the element; a write takes `&var self`, the index, and the new value, and returns nothing. Neither takes type parameters, variadics, or default arguments. Arrays, slices, and pointers keep their built-in indexing, which no `extend` block can displace or redefine; the operators are reached only for a type the language does not index on its own.

Nothing fixes the index type. Overloads of either operator are separated by it, so one type may answer several index forms, including a range:

```rux
extend Vect {
    func [](&self, span: int..int) -> int[..] {
        return self.data[span];
    }
}
```

The two are independent. A type may declare only `[]` and be read but not written, only `[]=` and be written but not read, or both. Declaring both does not make the pair act as one: neither operator reads and writes on its own, and combining them would evaluate the receiver and the index twice, so `v[i] += x`, `v[i]++`, and `v[i]--` are rejected in favour of writing the read and the write out. A field of a read result is not a place either — `[]` returns a value, and an operator cannot return a reference, so there is no place-returning indexer — which leaves `v[i].field = x` and borrowing `v[i]` as a `&T` rejected as well. A container lends an element through a method that returns a reference instead, so `v.AtMutable(i).field = x` writes the field in place.

Either call borrows its receiver whole for the duration of the call, exactly as a method call does: `[]` reads it, and `[]=` needs a receiver it may write, so a `let` binding or an access through `&T` is rejected. Both therefore conflict with an exclusive borrow of any part of the receiver, and a read never counts as a partial move out of it — the result is a fresh value that transfers on its own. The value written by `v[i] = x` reaches `[]=` as an ordinary by-value argument, so it copies or, with `v[i] = move x`, moves.

Only the assignment's own target is written. In `v[keys[j]] = x`, the subscript on `keys` is a read through `[]` and only the outer index writes.

## Destruction

A destructor is a body-bearing special function named after its type with a leading `~`:

```rux
extend String {
    func ~String(&var self) {
        Free(self.data);
    }
}
```

The compiler invokes it exactly once for each initialized value that still owns its state, then destroys contained fields, elements, and payloads in reverse construction order. Destruction runs at ordinary scope exit, replacement, `return`, `break`, `continue`, the propagation of an absence through `?`, and an error leaving a scope for its handler or the caller. A moved-from or never-initialized value is not destroyed. Panic and process termination do not unwind.

A destructor makes its type move-only, as [Copy and Move](#copy-and-move) describes, so a value with one is never copied implicitly and each one is destroyed by its last owner. A destructure that takes its value over owns every part of it: a destructuring `let`, which owns its whole initializer, and a match arm whose subject was handed over with `move`, is a temporary such as a `Clone` result, or is the copy a by-value match makes of plain data, as well as a `catch` arm, which owns the error. A part the pattern binds to a name belongs to that binding and is destroyed with it. A part bound to `_`, and a structure field the pattern leaves out, has no owner left, so it is destroyed on the spot — at the `let`, or at the top of the arm before its body runs — like a temporary nobody kept, in the order the parts appear in the pattern. `let (_, second) = move pair;` therefore destroys the first element immediately and the second when `second` goes out of scope, and `let Parcel { contents: kept } = move parcel;` destroys every other field of `parcel` at the `let`, in declaration order. A structure pattern in `let` that binds nothing destroys the value whole, as an arm does. `_` on its own is the same wildcard: `let _ = move value;` and `let _ = value.Clone();` bind no name, so `_` cannot be read afterwards and the value is destroyed at the statement, before the next one runs. An arm that binds nothing from a consumed subject destroys the subject whole, a consumed subject that no arm takes — every arm refuted it, by a guard or a test — is destroyed as the match ends, and a match that takes nothing from a named subject leaves it with its owner. A match that binds from a named plain-data value without `move` matches a copy of it: the original stays with its owner and the arms own the copy exactly as they would own a subject handed over. A named owning subject is matched with `match move value`, or as a clone with `match value.Clone()`, whose arms own the clone and destroy it once.

A moving pattern may split a tuple, or a structure that declares no destructor of its own, even when the parts are droppable. A structure whose type declares `~T` cannot be split by a moving pattern, whether it is the subject, a member of a tuple or payload, or part of a clone a match takes apart: its destructor runs on the whole value, so a pattern that binds any of its fields in an arm that owns them is an error, `cannot split 'T' with a moving pattern, because it declares destructor '~T'`. A destructuring `let` always owns what it takes apart, a clone included, so `let T { field: name } = value.Clone();` is refused for such a type as well. Such a value can still be tested with a pattern that binds none of its fields, bound whole, or matched through a borrow, which takes nothing from it. The rule matches the one that rejects moving a single field out of a droppable value with `move`.

`Core::Drop` is not part of the language or Core package. Lifecycle cleanup is expressed only by `~Type`, and an ordinary interface or method named `Drop` has no compiler-defined ownership meaning.

## Final Ownership Boundary

Implicit consumption of named move-only values, the `<-` transfer, bodyless copy and move operators, mutable-parameter prefixes, exact-type forwarding `New` wrappers, and `Core::Drop` are removed. Parser, semantic, golden and integration tests preserve rejection of these removed forms; source conventions are documented rather than scanned. Throwing constructors, descriptive factories such as `WithCapacity` or `FromParts`, interface `Self`, and deliberately raw pointer APIs are permanent parts of the language and packages rather than compatibility syntax.
