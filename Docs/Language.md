# Language Ownership and Lifecycle Contract

This page is the settled contract for Rux values, borrowing, copying, moving, construction, and destruction. The compiler, first-party packages, and positive language examples use this model. Removed ownership spellings are accepted only in negative diagnostic fixtures.

The design favors locally visible ownership effects, explicit signatures, and separate syntax for safe borrowing and raw addresses. Declarations and members remain private by default and use `pub` for public API. A `struct` declares layout, while functions, operators, constructors, destructors, and interface implementations live in `extend` blocks.

## Contextual Expressions

A typed integer initializer, assignment, return, call argument, or aggregate element supplies the required type to both arms of `condition ? first : second`. Unsuffixed integer literals must fit that type; nested conditional arms receive the same context. Each selected arm is materialized at the required width before the values merge, including integers wider than one machine word. The condition runs once and only the selected arm is evaluated.

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
variant Result<T, E> {
    Success(T),
    Error(E)
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

Copy, move, and destruction are also case-aware. A variant is copyable or movable only when every reachable payload supports the operation. Moving transfers the active payload and invalidates the source. Destruction reads the tag once and destroys only the active payload, in reverse field order, exactly once; partially constructed cases roll back only the payloads already initialized.

The runtime representation is a private tag followed by storage aligned for the widest case. Calls and returns use the aggregate ABI selected for that complete layout; a variant is never passed as only its tag. The compiler keeps the tag width, payload offsets, construction, matching, equality, moves, and drop glue on one layout contract for every target. This representation is compiler-managed, not a substitute for a C ABI declaration.

Both forms are closed, so adding a member or case makes exhaustive matches fail until callers handle it. For an enum, an explicitly assigned discriminant is part of the source-visible numeric contract; append or assign members deliberately when those values are serialized or cross an ABI. A variant's tag numbers are not a source contract and must never be serialized directly. Stable files, protocols, and foreign interfaces should encode an explicit scalar enum and then translate to or from the in-memory variant.

Removing or renaming an enum member or variant case is likewise a source-breaking change. Changing a variant payload changes its value layout and the operations required from that payload, including equality, copying, moving, and destruction. Public library evolution should therefore treat case lists and payload types as part of the API even though the private tag representation is not.

`union` remains the explicit overlapping-storage type. It has fields rather than cases, stores no tag, performs no active-case check, and is appropriate for foreign or manually tracked layouts. Use `variant` when the compiler must know which alternative is active; use `union` only when the program or an external ABI owns that invariant.

The source cutover is intentionally narrow. Scalar enums retain their syntax, discriminants, conversions, and representation. First-party payload types that used the compiler's default enum layout migrate directly to `variant`. External payload enums that exposed an integer base or assigned discriminants are not source-compatible: because a variant's tag is private, those declarations require an explicit redesign, usually a scalar wire enum plus a separate payload type or a manually controlled union.

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

Members are always named through their value, such as `self.length`; a receiver never enables implicit field lookup. `Self` remains available only where an interface must name the unknown concrete implementing type.

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

Interface method arguments follow the same conversions as ordinary function arguments. A concrete local or concrete reference can supply a borrowed interface argument directly; an existing interface view retains its data and vtable. Mutable interface arguments require writable storage. Interface requirements may provide typed default arguments and variadic interface arguments.

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

Literals are read-only character slices. They need no package import, and `.data` and `.length` are compiler-owned.

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

### Range types

Range expressions and annotations use the same punctuation family. Bounds in a two-sided range have one type.

| Expression | Type | Members |
| --- | --- | --- |
| `1..4` | `int..int` | `.start`, `.end` |
| `1..=4` | `int..=int` | `.start`, `.end` |
| `1..` | `int..` | `.start` |
| `..4` | `..int` | `.end` |
| `..=4` | `..=int` | `.end` |
| `..` | `..` | none |

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

Constructors are called as `String(...)` or `Vector<int32>(...)`, not through an implicit conversion. Infallible same-type `New` factories migrate to this form. Fallible factories returning `Option<T>` and descriptive factories such as `NewKeyed` or `New128` are not constructors and may keep their names.

`var value: T;` invokes `T()` when an accessible default constructor exists. With no `T()`, the declaration remains legal but denotes compiler-tracked uninitialized storage. Reading or destroying that storage before definite initialization is an error. Construction and assignment are distinct: a constructor creates a value, while `=` replaces or initializes storage according to its current state. Constructor lookup is never a general hidden conversion rule.

### Repeated fixed-array initialization

`[value; count]` constructs a fixed inline array by evaluating `value` exactly once and copying the result into every element. `count` must be a non-negative compile-time integer. The expression has type `T[N]`, so both the element type and extent can be inferred, while an explicit destination type contextually types the repeated value:

```rux
let inferred = [0u8; 16];
let contextual: uint8[16] = [0; 16];
```

The element type must be copyable. Construct move-only elements explicitly or initialize mutable array storage in a loop instead. A zero count still evaluates the value once and destroys the temporary when its type requires cleanup. Repeated arrays use the same fixed-array-to-`T[..]` coercion as ordinary array literals.

## Failure Propagation

`outcome?` evaluates its operand once, extracts its success payload, or returns its failure from the enclosing function. It recognizes ordinary variants with exactly `Success(T)` and `Error(E)` cases, or `Some(T)` and payload-less `None` cases. The enclosing return type must use the same protocol and error type; its success type and case order may differ. Rewrapping preserves the complete active payload, including nested and generic variants and zero-sized values.

Propagation consumes its evaluated outcome. A named copyable outcome is copied and remains usable; a named move-only outcome requires `(<-outcome)?`. The hidden payload is transferred using its move operation. Reference payloads cannot preserve hidden borrow provenance, and payloads that prohibit moving are rejected, including when instantiated in a generic. Use an explicit match when the payload needs to be borrowed or copied instead.

A propagated failure follows ordinary return behavior: it captures the failure before running registered defers in LIFO order and then performs ownership cleanup. The returned payload belongs to the caller; live locals and completed components of interrupted aggregate construction retain their normal cleanup.

## Option Coalescing

`option ?? fallback` extracts the value carried by an Option or evaluates `fallback` when the Option is `None`:

```rux
let port = configuredPort ?? ReadDefaultPort();
```

The left operand is evaluated exactly once. The fallback is checked at compile time but evaluated only for `None`, so it may perform expensive work or visible side effects without paying that cost on the `Some` path. The result type is the payload type, and the fallback must be assignable to it.

Option is a structural protocol rather than a built-in type name. The left type must be a `variant` with exactly `Some(T)` and payload-less `None` cases; a user-declared `Maybe<T>` with that shape works identically. `Result`, scalar enums, malformed lookalikes, references to an Option, and raw pointers used directly as the left operand are rejected. A raw pointer may still be the `T` inside an Option. Reference payloads are not supported because extracting one would lose the borrow provenance hidden inside `Some`.

Coalescing consumes its evaluated Option. A named copyable Option is copied and remains usable, while a named move-only Option must make the transfer visible:

```rux
let value = (<-ownedOption) ?? <-ownedFallback;
```

The fallback transfer is conditional, so `ownedFallback` is possibly moved after this expression. `??` associates to the right. Logical `||` binds more tightly and `?:` binds less tightly, so `first ?? second ?? fallback` means `first ?? (second ?? fallback)`. The operator is compiler-owned control flow: it cannot be declared in an `extend` block or overloaded, and there is no `??=` form. Because `??` is now a single maximal-munch token, two adjacent postfix propagations must be written `(nested?)?` instead of `nested??`.

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
