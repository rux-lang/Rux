# Language Ownership and Lifecycle Contract

This page is the settled contract for Rux values, borrowing, copying, moving, construction, and destruction. The
compiler and first-party packages migrate to it in dependency order. Until the final compatibility work is complete,
the repository may contain both the old and new spellings; compatibility exists only to keep each intermediate commit
buildable and is not part of the finished language.

The design favors locally visible ownership effects, explicit signatures, and separate syntax for safe borrowing and
raw addresses. Declarations and members remain private by default and use `pub` for public API. A `struct` declares
layout, while functions, operators, constructors, destructors, and interface implementations live in `extend` blocks.

## Bindings and Parameters

Bindings have three forms:

```rux
const MaxSize = 1024;
let size = 10;
var index = 0;
```

`const` is a compile-time value, `let` is an immutable runtime binding, and `var` is a mutable runtime binding.
Parameters always use `name: Type`; binding mutability is not written before a parameter name. A method is a function
whose first parameter is named `self`, and that receiver has the same explicit type syntax as every other parameter:

```rux
func Read(self: &Buffer)
func Clear(self: &var Buffer)
func Consume(self: Buffer)
```

Members are always named through their value, such as `self.length`; a receiver never enables implicit field lookup.
`Self` remains available only where an interface must name the unknown concrete implementing type.

## Values, References, and Raw Pointers

The five storage-facing type forms are:

```text
T        owned value
&T       immutable reference
&var T   mutable reference
*T       raw read-only pointer
*var T   raw writable pointer
```

References are non-owning, non-null, automatically dereferenced for member and index access, and cannot perform
pointer arithmetic. They may be parameters, receivers, or local aliases. They cannot be fields, returned values, or
otherwise stored beyond the current call. Any number of immutable borrows may coexist, or one mutable borrow may
exist exclusively; a borrow ends after its last use. A reference cannot destroy its referent or move ownership out of
it. A concrete value may be borrowed directly as an immutable or mutable interface view without copying or consuming
the value.

Raw pointers remain the deliberate mechanism for FFI, nullable or sentinel values, stored addresses, pointer
arithmetic, and unsafe memory APIs. Address-taking is explicit with `@value`. Moving a value out through a raw pointer
is rejected because the pointer does not own the storage it addresses.

## Copy and Move

Copy and move are different operations:

```text
=     copy or copy assignment
<-    move or move assignment
```

A named source is copied unless the source expression is prefixed by `<-`. A fresh temporary transfers directly
because no visible source remains afterwards:

```rux
let copy = value;
let moved <- value;

Consume(value);          // copy
Consume(<- value);       // move
Consume(MakeValue());    // direct temporary transfer
```

Copying never invalidates its source. Moving invalidates the source, suppresses its later destruction, and makes every
subsequent read a compile error. Assignment to a live destination releases that destination's old state at the
operation's defined replacement point. Initializing previously uninitialized storage performs no prior destruction.

Copy and move capabilities are structural. An absent special operation asks the compiler to generate the operation
when every field supports it. A declaration with a body supplies a custom implementation. A canonical declaration
without a body prohibits that compiler-generated operation:

```rux
extend File {
    func =(self: &var File, other: &File);
}

extend Pinned {
    func =(self: &var Pinned, other: &Pinned);
    func <-(self: &var Pinned, other: Pinned);
}
```

The canonical copy prohibition is `func =(self: &var T, other: &T);`. The canonical move prohibition is
`func <-(self: &var T, other: T);`. Bodyless ordinary functions in interfaces remain interface requirements rather
than prohibitions.

A custom `=` writes a new state into compiler-provided scratch storage and cannot consume its source. Copy assignment
first produces that new state, then destroys the old destination and installs the result. A custom `<-` consumes its
source under the same source-invalidating rule as a generated move. Resource-owning types must explicitly prohibit
copying unless they implement a real independent copy.

## Construction and Initialization

Inside `extend T`, a receiverless function named `T` that returns exactly `T` is a constructor candidate:

```rux
extend String {
    func String() -> String {
        return String { data: null, length: 0 };
    }

    func String(value: Slice<char8>) -> String {
        // allocate and copy
    }
}
```

Constructors are called as `String(...)` or `Vector<int32>(...)`, not through an implicit conversion. Infallible
same-type `New` factories migrate to this form. Fallible factories returning `Option<T>` and descriptive factories
such as `NewKeyed` or `New128` are not constructors and may keep their names.

`var value: T;` invokes `T()` when an accessible default constructor exists. With no `T()`, the declaration remains
legal but denotes compiler-tracked uninitialized storage. Reading or destroying that storage before definite
initialization is an error. Construction and assignment are distinct: a constructor creates a value, while `=`
replaces or initializes storage according to its current state. Constructor lookup is never a general hidden
conversion rule.

## Destruction

A destructor is a body-bearing special function named after its type with a leading `~`:

```rux
extend String {
    func ~String(self: &var String) {
        Free(self.data);
    }
}
```

The compiler invokes it exactly once for each initialized value that still owns its state, then destroys contained
fields, elements, and payloads in reverse construction order. Destruction runs at ordinary scope exit, replacement,
`return`, `break`, `continue`, and failure propagation through `?`. A moved-from or never-initialized value is not
destroyed. Panic and process termination do not unwind.

The existing `Core::Drop` interface remains accepted only during the staged repository migration. Once every package,
test, and example uses `~Type`, the compatibility interface and its semantic recognition are removed.

## Migration Boundary

Compiler syntax and semantics land before package use, with each migration change leaving the repository green. Old
spellings remain accepted only while downstream sources still require them. The cutover ends by removing temporary
constructor wrappers, `Core::Drop`, implicit named moves, and mutable-parameter prefixes, then adding repository policy
guards against their return. No package or manifest version changes merely because an intermediate migration commit
lands.
