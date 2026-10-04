# Compiler Defects

Open defects found while writing the example course in `D:\Work\Examples`. Each entry has a
minimal reproducer, the expected and actual behaviour, and the course lesson that works around it.
Compiler identity: `Rux 0.4.0 (2026-10-04 14:40:27 UTC)`, source `680ef68c`, Windows x86-64.
An entry is marked **Fixed**, with its regression coverage, once a test covers its fix.

## D1. Literal `match` arms ignore the expected type (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.

```rux
func F(b: bool) -> int32 {
    return match b { true => 0, false => 4 };
}

func Main() -> int {
    let x: int32 = match true { true => 1, false => 2 };
    return 0;
}
```

- **Expected:** both accepted. The unsuffixed literals take the expected type `int32`, as they
  do in `return b ? 0 : 4;`, which is accepted.
- **Actual:** `rux check` exits 1 with two errors:
  - `'return' value must have type 'int32', but found 'int'`
  - `cannot assign 'int' to 'int32'`
- **Notes:** 66c079f7 ("Type match arms against the expected type") fixed `none` arms. Integer
  literal arms are still typed as `int` before the expected type is consulted.
- **Course workaround:** `SumTypes/SubsetPattern` returns `int` instead of `int32`.

## D2. An array literal does not convert its elements to a declared sum element type (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals` and the `ArrayElementTypes` golden.

```rux
func Main() -> int {
    let values: (int32 | bool)[2] = [1i32, true];
    return 0;
}
```

- **Expected:** accepted. Each element is injected into the declared element type
  `int32 | bool`, as a single `let v: int32 | bool = true;` is.
- **Actual:** `array element 2 has type 'bool8', but element 1 established element type
  'int32'`. Struct members of a sum behave the same way:
  `let tokens: Token[3] = [Number { ... }, Plus {}, Minus {}];` is rejected too.
- **Course workaround:** `SumTypes/Is` binds each element first (`let a: Token = ...;`) and builds
  the array from those names.

## D3. The help for an ungrouped sum after `is` names a placeholder instead of the member (fixed)

**Fixed**; covered by `NativePatternSemanticsTests.cpp`.

```rux
func Main() -> int {
    let x: int32 | bool = 1i32;
    let ok = x is int32 | bool;
    return 0;
}
```

- **Expected:** `error: a sum type after 'is' must be grouped` with the help line
  `write 'value is (int32 | bool)'`.
- **Actual:** the help line reads `write 'value is (A | bool)'`. The left member is printed as
  the placeholder `A`.
- **Course workaround:** `SumTypes/Is` quotes only the error line.

## D4. A slice-typed `match` expression with a diverging arm produces invalid LIR (fixed)

**Fixed**; covered by `Tests/Language/Match`.

```rux
import Core::Panic;

func Pick(day: uint) -> char8[..] {
    return match day {
        0 => "Monday",
        else => Panic("out of range")
    };
}

func Main() -> int {
    return Pick(0).length as int;
}
```

- **Expected:** `rux run` builds the program, which exits with status 6.
- **Actual:** the build fails. It reports `invalid internal LIR while lowering function 'Pick':
  cannot insert instruction into the current block: the current block already has a terminator`,
  repeated about ten times.
- **Notes:**
  - The same match works when it produces `int` or a struct, or when the arm is
    `else => return "other"`.
  - A user `#NoReturn` function in the arm fails the same way as `Panic`.
- **Course workaround:** `Errors/Panic` returns `int` (the days in a month) rather than a day
  name.

## D5. A `match` statement whose every arm returns is not treated as returning (fixed)

**Fixed**; covered by `Tests/Language/Match`.

```rux
func Pick(day: uint) -> int {
    match day {
        0 => return 10,
        else => return 11
    }
}

func Main() -> int {
    return Pick(0);
}
```

- **Expected:** accepted. Every arm leaves the function and `else` covers every other value.
- **Actual:** `function 'Pick' must return a value of type 'int' on every control-flow path`.
  Arms that call `Panic` or a `#NoReturn` function are rejected the same way.
- **Course workaround:** none needed, because no lesson relies on it.

## D6. Diagnostic wording for nested outcome types (fixed)

**Fixed**; covered by the `MatchArmTypes` and `NativeOutcomeMisuse` goldens and `NativeTypeIdentityTests.cpp`.

- A non-exhaustive match on `int ! (DigitError | RangeError)` prints the type as
  `'int ! DigitError | RangeError'`. Without the parentheses, it reads as a different grouping.
- Using a unit function without `#NoReturn` as a match arm reports `expected 'int', found
  'opaque'`. The found type should read `()`.
## D7. Chained assignment reports an internal "opaque" type (fixed)

**Fixed**; covered by the `SemanticOperatorsAndAssignments` golden.

```rux
func Main() -> int {
    var a: int32 = 1;
    var b: int32 = 2;
    a = b = 7;
    return 0;
}
```

- **Expected:** an error saying that an assignment produces no value and cannot be chained.
- **Actual:** `cannot assign 'opaque' to 'int32'`. The diagnostic names an internal type, as in
  D6.

## D8. Integer division by zero ends the program silently with exit code 127 (fixed)

**Fixed**; covered by `RuntimeCheckLoweringTests.cpp`.

- **Reproducer:** `var d = 0; let q = 10 / d;`, executed at run time.
- **Expected:** a panic-style message naming the division by zero, as a failed `Assert` gives.
- **Actual:** the process exits with code 127 and prints nothing.
- **Course workaround:** `Operators/Logical` guards the division with `items != 0 && ...`.
## D9. A tuple literal with unsuffixed integers compares unequal to an equal typed tuple (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.

```rux
import Io::PrintLine;

func Main() -> int {
    let a: (int32, bool) = (3, true);
    PrintLine("{}", a == (3, true));      // prints false
    let e: (int32, int32) = (3, 4);
    PrintLine("{}", e == (3, 4));         // prints false
    PrintLine("{}", a == (3i32, true));   // prints true
    return 0;
}
```

- **Expected:** `true` for both comparisons. The unsuffixed literals should take the other
  operand's member types, as a scalar comparison does, or the comparison should be rejected.
- **Actual:** `false`, with exit code 0. The literal tuple seems to stay `(int, …)` and is
  compared at the wrong width. It is wrong only when a typed member is not `int`.
- **Course workaround:** `Sequences/Tuple` compares against a typed binding.

## D10. Chained tuple indexes do not parse (fixed)

**Fixed**; covered by `LexerTests.cpp` and `Tests/Language/Tuple`.

```rux
let nested = ((1, 2), 3);
PrintLine("{}", nested.0.1);
```

- **Expected:** prints `2`.
- **Actual:** a parse error, `expected a field name or tuple index after '.' before '0.1'`. The
  lexer reads `0.1` as a float literal. `(nested.0).1` works.
- **Course workaround:** `Sequences/Tuple` binds the inner tuple first.

## D11. A default argument that names an earlier parameter reads uninitialised memory (fixed)

**Fixed**; covered by the `DefaultValueScope` golden.

```rux
import Io::PrintLine;

func C(x: int, y: int = x) -> int {
    return x + y;
}

func Main() -> int {
    PrintLine("{}", C(2));
    return 0;
}
```

- **Expected:** a compile error, because a default cannot refer to another parameter, or `4`.
- **Actual:** prints a garbage value such as `5260204362093136202` and exits 0.

## D12. An overload with a default argument makes a call ambiguous, and declaration order decides (fixed)

**Fixed**; covered by `Tests/Language/Functions` and the `OverloadDefaults` golden.

```rux
import Io::PrintLine;

func D(x: int, y: int = 1) -> int { return x + y; }
func D(x: int) -> int { return x; }

func Main() -> int {
    PrintLine("{}", D(2));
    return 0;
}
```

- **Expected:** an ambiguous-call error, or a documented preference for the exact-arity overload.
- **Actual:** prints `3`. Swapping the two declarations prints `2`.

## D13. An error inside a generic body does not name the instantiating call (fixed)

**Fixed**; covered by `SemanticGenericInstantiationTests.cpp` and `NativePatternSemanticsTests.cpp`.

- **Reproducer:** a `func Larger<T>(first: T, second: T) -> T` whose body uses `>`, called with a
  struct that defines no `>`.
- **Expected:** besides the error in the body, a note naming the call that set `T = P`.
- **Actual:** only `operator '>' is not defined for 'P'`, pointing inside `Larger`.

## D14. Spreading an array into a variadic parameter needs an explicit slice (fixed)

**Fixed**; covered by `Tests/Language/Variadic`.

- **Reproducer:** `Sum(scores...)` where `scores: int32[3]` and `func Sum(args: int32...)`.
- **Expected:** accepted, because an array converts to `int32[..]` everywhere else.
- **Actual:** `argument 1 to 'Sum' has type 'int32[3]', but variadic parameter 'args' requires
  'int32[..]'`.
- **Notes:** this may be by design; it is recorded so that it can be decided.
- **Course workaround:** `Sequences/Variadic` writes `scores[..]...`.
## D15. A scalar cannot be written through `&var T` (fixed)

**Fixed**; covered by `Tests/Language/BorrowedScalarWrites`, the `BorrowedScalarWrite` golden, and `ReferenceTypeTests.cpp`.

```rux
func Bump(count: &var int) { count += 1; }

func Main() -> int { var n = 4; Bump(n); return n; }
```

- **Expected:** compiles, and `n` becomes 5. `Docs/Language.md` says a function that changes
  caller-owned storage takes `value: &var T`.
- **Actual:** two errors, `cannot modify immutable variable 'count'` and `operator '+=' produces
  'int', which cannot be stored in target type '&var int'`. Other spellings fail too:
  - `count = count + 1;` gives `cannot assign 'int' to '&var int'`.
  - `*count += 1;` gives `operator '*' requires a pointer operand`.
- **Notes:**
  - Writing through `&var` works for struct fields and array elements.
  - `Tests/Language/BorrowedScalars` covers reading a borrowed scalar only.
- **Course workaround:** `Types/MutableReference` demonstrates `&var` with structs and arrays only.

## D16. A `match` on an enum value outside its cases silently yields a zero value (fixed)

**Fixed**; covered by `RuntimeCheckLoweringTests.cpp`.

```rux
import Io::PrintLine;

enum Status: uint16 { Ok = 200, NotFound = 404 }

func Main() -> int {
    let code: uint16 = 418;
    let status = code as Status;
    let name = match status { .Ok => "ok", .NotFound => "not found" };
    PrintLine("[{}] {}", name, name.length);
    return 0;
}
```

- **Expected:** a run-time failure. The match is accepted as exhaustive, so no value should fall
  through it. Alternatively, an unchecked `as` into an enum could be rejected.
- **Actual:** prints `[] 0` and exits 0.
- **Course workaround:** `Types/EnumValue` checks the code before converting with `as`, and warns
  about the unchecked conversion.

## D17. The help text for a shorthand enum case names a placeholder type (fixed)

**Fixed**; covered by the `EnumShorthand` and `When` goldens and `ConditionalEvaluationTests.cpp`.

- **Reproducer:** `enum Direction { North, South }`, then
  `func Flip(d: Direction) -> Direction { return match d { .North => .South, .South => .North }; }`.
- **Actual:** `'.South' must be written in full, as in 'Enum::South'`.
- **Expected:** the help names the real type, `Direction::South`. Ideally the expected type
  would let `.South` resolve on its own, as `.Some` and `.Success` do.
## D18. Tuple and struct patterns in `match` always match and never set their bindings (fixed)

**Fixed**; covered by `Tests/Language/TupleStructPatterns` and `DestructuringLoweringTests.cpp`.

```rux
import Io::PrintLine;

func Main() -> int {
    let pair: (int32, int32) = (1, 2);
    let label = match pair { (0, 0) => "origin", (x, 0) => "x axis", else => "elsewhere" };
    PrintLine("{}", label);
    let sum = match pair { (a, b) => a + b };
    PrintLine("{}", sum);
    return 0;
}
```

- **Expected:** `elsewhere`, then `3`.
- **Actual:** `origin`, then `1`. For a struct pattern such as `Point { x: a, y: b } => a + b`,
  the result is garbage.
- **Notes:** `LowerPattern` in `Compiler/Lowering/HirToLir/HirToLirAggregates.cpp` (around lines
  426–444) handles `HirTuplePattern` and `HirStructPattern` by allocating binding slots and
  returning the constant `1`. It never compares the literal elements and never writes the slots.
  Destructuring with `let (x, y) = pair;` is correct, because it goes through `BindLetPattern`.
- **Course impact:** lesson 7.3 `TuplePattern` is blocked until this is fixed.

## D19. A non-exhaustive `match` expression on `bool` or an integer compiles (fixed)

**Fixed**; covered by the `MatchExpressionCoverage` golden.

```rux
let b = false;
let r = match b { true => 10i32 };
```

- **Expected:** `match on 'bool' is not exhaustive; missing false`. A match expression on an
  integer should likewise require `else`.
- **Actual:** compiles. The unmatched value produces an arbitrary result: `0` in one run and `1`
  in another.
- **Course workaround:** `Patterns/Exhaustive` always writes `else` and does not claim that the
  compiler checks integer matches.

## D20. A guard after a range pattern does not parse (fixed)

**Fixed**; covered by `Tests/Language/Match`.

- **Reproducer:** `match n { 1..=9 if flag => "a", else => "b" }`.
- **Actual:** `expected '=>' after the match arm pattern before 'if'`, followed by cascading errors.
- **Notes:** `ParsePatternImpl` in `Compiler/Syntax/Parser/ParserExpr.cpp` checks for a guard
  before it parses the range.

## D21. A literal inside a variant field pattern reports the wrong construct (fixed)

**Fixed**; covered by `Tests/Language/Match`.

- **Reproducer:** `.Circle { radius: 0 } => ...` as a match arm.
- **Actual:** `unsupported pattern in let binding`. The construct is a match arm, so the message
  should say so.

Related to D1: an unsuffixed tuple literal passed as an argument is not adapted either.
`Where((3, 0))` with `Where(p: (int32, int32))` gives `no matching overload for 'Where' with
argument types ((int, int))`. **Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.
## D22. A `const` initializer that is not a compile-time value is accepted and re-evaluated at run time (fixed)

**Fixed**; covered by `SemanticTests.cpp` and the `ConstantInitializers` golden.

```rux
import Io::PrintLine;

func Seven() -> int { PrintLine("Seven ran"); return 7; }

const FromCall = Seven();

func Main() -> int {
    PrintLine("{}", FromCall);
    PrintLine("{}", FromCall);
    var v = 1;
    v = 5;
    const C = v;
    PrintLine("{}", C);
    return 0;
}
```

- **Expected:** both `const` declarations are rejected, for example with `'v' is not a
  compile-time constant`. `Docs/Language.md` says `const` is a compile-time value.
- **Actual:** exits 0 and prints `Seven ran`, `7`, `Seven ran`, `7`, `5`. The call runs on every
  use. The error appears only when the constant is used in a compile-time position, such as
  `when C == 5`.
- **Course workaround:** `Basics/Const` states the rule in a comment, without a diagnostic to quote.

## D23. A `c8` or `c16` character literal that does not fit is silently truncated (fixed)

**Fixed**; covered by the `CharacterWidths` golden.

- **Reproducer:** `let e = c8'😀'; let f = c16'😀';`, then print each one `as uint32`.
- **Expected:** a compile error saying that U+1F600 does not fit `char8` or `char16`.
- **Actual:** prints `0 62976`. `c8'é'` is also accepted, as the single byte 0xE9, which is not a
  valid UTF-8 code unit.
- **Course workaround:** `Basics/Character` keeps `c8` literals to ASCII.

## Notes outside the compiler

- **`Io` placeholders.** `PrintLine("{{}}")` with no arguments prints `{{}}` literally, but with
  arguments `{{` collapses to `{`. A placeholder count mismatch, such as `PrintLine("{} {}", 1)`,
  prints nothing; the failure is reported only through the returned `IoError?`.
  **Fixed**; covered by the `FormatPlaceholders` and `FormatAttributeDeclarations` goldens,
  `ParserDiagnosticsTests.cpp`, `ParserDumpTests.cpp` and `SemanticTypeCallDiagnosticsTests.cpp`.
- **`///` inside a function body** is refused with `expected an expression before '/// …'`. A
  diagnostic saying documentation comments attach only to declarations would be clearer.
  **Fixed**; covered by the `DocumentationInBlock` golden.
- **Out-of-range float-to-integer conversion is unspecified in `Docs/Language.md`.**
  `1e20 as int32` gives `0`, while `1e20 as int64` gives `int64::Min`. **Fixed**; covered by `Tests/Language/FloatToInteger` and `RuntimeCheckLoweringTests.cpp`.
## D24. A tuple literal converted into an optional, fallible or sum holds garbage (fixed)

**Fixed**; covered by `Tests/Language/NativeOutcomes`.

```rux
import Io::PrintLine;

func Main() -> int {
    let a: int32 = 80;
    let b: int32 = 24;
    let wrapped: (int32, int32)? = (a, b);
    match wrapped {
        p? => PrintLine("{} {}", p.0, p.1),
        none => PrintLine("none")
    }
    return 0;
}
```

- **Expected:** `80 24`.
- **Actual:** a value that changes between runs, such as `382729312 62`, with exit code 0.
- **Notes:** the following fail the same way:
  - `.Some((a, b))`
  - `(int32, int32) ! bool = (a, b)` and `(int32, int32) | bool = (a, b)`
  - `return (a, b);` in a function returning `(int32, int32)?`

  Binding the tuple first (`let pair = (a, b); return pair;`) works.
- **Course workaround:** `Generics/GenericOutcome` binds the tuple before returning it.

## D25. An array of a native form does not convert to a slice argument (fixed)

**Fixed**; covered by `Tests/Language/Optional`.

```rux
func Plain(values: int32?[..]) -> uint { return values.length; }

func Main() -> int {
    let readings: int32?[4] = [ 3, none, 5, none ];
    let a = Plain(readings);
    return 0;
}
```

- **Expected:** accepted, as an `int32[4]` argument for an `int32[..]` parameter is.
- **Actual:** `no matching overload for 'Plain' with argument types (int32?[4])`. A generic
  `T[..]` parameter is refused the same way. `let view: int32?[..] = readings;` and
  `Plain(readings[..])` both work.

## D26. An annotation does not reach a generic variant's case constructor (fixed)

**Fixed**; covered by `Tests/Language/GenericEnumInstantiation`.

- **Reproducer:** `variant Reading<T> { Exact(T), Missing }`, then
  `let b: Reading<int32> = Reading::Exact(7);`.
- **Expected:** accepted, with `7` taking the type `int32` from the annotation.
- **Actual:** `cannot assign 'Reading<int>' to 'Reading<int32>'`. The other spellings fail too:
  - `Reading::Missing` gives `cannot assign 'Reading' to 'Reading<int32>'`.
  - `.Exact(7)` gives `'.Exact' must be written in full`.
- **Course workaround:** `Generics/GenericType` writes `Reading::Exact<int32>(7)`.

## D27. A missing type argument on a generic struct literal produces a cascade of errors (fixed)

**Fixed**; covered by the `GenericStructTypeArguments` golden.

- **Reproducer:** `struct Pair<T> { first: T; second: T; }`, then
  `let p = Pair { first: "l", second: "r" };`.
- **Expected:** one error.
- **Actual:**
  - the correct error, `struct initializer for 'Pair' requires 1 type argument, but 0 were
    provided`
  - nine spurious `type 'T' is not defined in this scope` errors at the declaration
  - `struct 'Pair' has no field 'first'` and the same for `'second'`
## D28. A mutating interface method can be called through a read-only borrow (fixed)

**Fixed**; covered by `Tests/Language/InterfaceReceivers`, the `InterfaceReceiverMutability` golden, and `InterfaceReferenceTests.cpp`.

```rux
interface Gauge { func Adjust(amount: int32); }

struct Knob { level: int32; }

extend Knob : Gauge {
    func Adjust(self: &var Knob, amount: int32) { self.level += amount; }
}

func Shared(gauge: &Gauge) { gauge.Adjust(1); }

func Main() -> int {
    let knob = Knob { level: 1 };
    Shared(knob);
    return knob.level as int;
}
```

- **Expected:** rejected, as it is with the concrete type `gauge: &Knob`: `cannot call 'Adjust'
  through immutable reference '&Knob'`.
- **Actual:** compiles, and the run exits with status 2. The caller's `let` value was changed.
  `let g: Gauge = knob; g.Adjust(1);` is accepted too, but changes only the copy.
- **Course workaround:** `Interfaces/InterfaceParameter` uses `&var Gauge` for the mutating case.

## D29. An array literal does not convert its elements to a declared interface element type (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.

- **Reproducer:** two structs implementing `Shape`, then `let shapes: Shape[2] = [square, circle];`.
- **Expected:** each element is converted to `Shape`.
- **Actual:** `array element 2 has type 'Circle', but element 1 established element type
  'Square'`. This has the same cause as D2: an array literal ignores its declared element type.
- **Course workaround:** `Interfaces/InterfaceValue` names each element as a `Shape` first.

## D30. An unsuffixed tuple index argument is not adapted, and the `[]` diagnostic is misleading (fixed)

**Fixed**; covered by the `IndexerOperator` golden and `Tests/Language/Indexer`.

```rux
struct Grid { cells: int32[4]; }

extend Grid {
    func [](self: &Grid, at: (uint, uint)) -> int32 { return self.cells[at.0 * 2 + at.1]; }
}

func Main() -> int {
    let g = Grid { cells: [1, 2, 3, 4] };
    return g[(1, 1)] as int;
}
```

- **Expected:** the run exits with status 4, as it does with `g[(1u, 1u)]`.
- **Actual:** `type 'Grid' cannot be indexed`, with the help `declare 'func []' on 'Grid'`, even
  though `func []` is declared. Any index whose type matches no `[]` gets this message, for
  example `week[1]` when `[]` takes a `Day`. The setter `g[(1, 1)] = 7` is accepted.
## D31. A conditional expression with mismatched branch types is accepted and crashes (fixed)

**Fixed**; covered by the `ConditionalBranchTypes` golden.

```rux
import Io::PrintLine;

func Main() -> int {
    let n = 1;
    let x = n > 2 ? "big" : 7;
    PrintLine("[{}]", x);
    return 0;
}
```

- **Expected:** a type-mismatch error, as `match` gives (`match arm type mismatch`).
- **Actual:** `rux check` passes. `rux run` prints `[` and then crashes with an access violation
  (0xC0000005).
- **Course workaround:** `ControlFlow/Ternary` states the same-type rule but quotes no diagnostic.

## D32. An inclusive range that ends at its type's maximum never terminates (fixed)

**Fixed**; covered by `Tests/Language/InclusiveRangeEnd`.

```rux
let start: uint8 = 250;
var passes = 0;
for v in start..=255 { passes += 1; if passes > 10 { break; } }
PrintLine("{}", passes);
```

- **Expected:** `6`.
- **Actual:** `11`. The loop variable wraps from 255 to 0, so without the guard the loop runs
  forever.
- **Course workaround:** no lesson iterates up to a type's maximum.

## D33. A range annotation with a narrower integer type rejects unsuffixed literals (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.

- **Reproducer:** `let r: int32..int32 = 1..5;`.
- **Expected:** accepted, with the literals taking `int32` from the annotation.
- **Actual:** `cannot assign 'int..int' to 'int32..int32'`. `int32..=int32` is rejected the
  same way.
- **Course workaround:** the lessons use `int` bounds, or typed variables as the bounds.

## D34. A nested loop may reuse its enclosing loop's label without a diagnostic (fixed)

**Fixed**; covered by the `LoopLabels` golden.

- **Reproducer:** `outer: for i in 0..3 { outer: for j in 0..3 { break outer; } }`.
- **Expected:** an error or a warning that the inner label shadows the outer one.
- **Actual:** accepted silently.

Related to D19: a match expression over an integer without `else` (`match n { 1 => 10, 3 => 30 }`)
compiles and yields `0` for an unmatched value. With text arms it yields an empty string. **Fixed**; covered by the `MatchExpressionCoverage` golden.
## D35. Inline array indexing is not bounds-checked, and the language reference does not say so (fixed)

**Fixed**; covered by `RuntimeCheckLoweringTests.cpp` and the `ArrayIndexRange` golden.

- **Reproducer:** `let primes: int32[4] = [2, 3, 5, 7]; var i: uint = 3; i += 3;`, then
  `PrintLine("{}", primes[i]);`.
- **Expected:** a decision recorded in `Docs/Language.md`. Either an out-of-range index stops the
  program, as `Collections::Array.Get` and `Set` report it, or unchecked indexing is documented as
  intended.
- **Actual:** the program reads whatever memory follows the array, prints it, and exits 0. A
  constant out-of-range index (`primes[7]`) is not diagnosed at compile time either.
- **Course impact:** `Sequences/Array` and `Collections/DynamicArray` warn about it.
## D36. A tuple element bound to `_` in a moving destructure is never destroyed (fixed)

**Fixed**; covered by `Tests/Language/PartialDestructure`, the `MovingPatternSplit` golden, and
`DestructuringLoweringTests.cpp`.

```rux
import Io::PrintLine;

struct Tag { id: int; }

extend Tag { func ~Tag(self: &var Tag) { PrintLine("drop {}", self.id); } }

func Main() -> int {
    let pair = (Tag { id: 1 }, Tag { id: 2 });
    let (_, second) <- pair;
    PrintLine("kept {}", second.id);
    return 0;
}
```

- **Expected:** `kept 2`, plus `drop 1` (at the destructure or at scope exit) and `drop 2`.
- **Actual:** `kept 2`, then `drop 2`. Tag 1 is never destroyed.
- **Course workaround:** `Ownership/PartialMove` binds both parts to names.

Related to D18: with a move-only subject, the struct pattern in
`match <-parcel { Parcel { label: l, contents: c } => ... }` binds zeroed values, and neither field
is ever destroyed. A plain struct pattern also binds wrong values: it gives `3 0` for
`Point { x: 3, y: 4 }`. **Fixed**; covered by `Tests/Language/PartialDestructure` and
`Tests/Language/TupleStructPatterns`.

Related language gap, not a defect: every partial move is rejected (`cannot move field '1' out
of droppable value 'parcel'`), and a struct cannot be destructured in `let`. A struct with
move-only fields therefore has no way to hand out a single field. Since D36, a moving `match`
pattern may split a struct that declares no `~T` of its own, destroying the fields it leaves
unbound at the top of the arm; a struct that declares `~T` cannot be split by a moving pattern
(`cannot split 'T' with a moving pattern, because it declares destructor '~T'`). Struct
destructuring in `let` remains open.
# Standard package defects

Defects in Packages/, found the same way.

## P1. `Text::StringBuilder` grows by exactly what is needed instead of doubling (fixed)

**Fixed**; covered by `Tests/Packages/Text/StringBuilder`.

- **Where:** `Packages/Text/Src/StringBuilder.rux`, in `ReserveBuilderStorage`, at
  `if MulChecked<uint>(*capacity, 2, @doubled) && doubled > required`.
- **Cause:** `Core::MulChecked` returns `true` when the multiplication overflows, so this condition
  is inverted. The buffer doubles only on overflow; otherwise it grows to exactly what is needed.
- **Reproducer:** `b.Append("0123456789abcdef")?; b.Append("xyz")?; PrintLine("{}", b.Capacity());`.
- **Expected:** `32`.
- **Actual:** `19`. Successive appends give capacities 16, 19, 26 and 35, so appending is quadratic.
- **Course impact:** `Text/StringBuilder` does not print the capacity.

## P2. `Io::ReadLine` refuses any non-ASCII input (fixed)

**Fixed**; covered by `Tests/Packages/Io/ReadLine`, which reads UTF-8 and invalid lines through the new `ReadLineFrom`.

- **Where:** `Packages/Io/Src/ReadLine.rux`. It appends each byte with `builder.AppendAscii(ch)`,
  which fails for any byte above 0x7F. The failure surfaces as `IoErrorKind::Other`, although the
  file's own comment promises UTF-8 input.
- **Reproducer:** `printf 'h\xc3\xa9llo\n' | rux run` in `Text/Input`.
- **Expected:** `line 1 is "héllo", 6 bytes`.
- **Actual:** the read fails; the lesson prints "the input could not be read" and exits with status 1.
## D37. An unsuffixed literal wider than 64 bits silently becomes 0 when used as an operand (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals` and the `IntegerLiteralRange` golden.

```rux
import Core::int128;
import Io::PrintLine;

func Main() -> int {
    let a: int128 = -42;
    PrintLine("{}", a * 1000000000000000000000);      // prints 0
    PrintLine("{}", a + 1000000000000000000000);      // prints -42
    PrintLine("{}", a * 1000000000000000000000i128);  // prints -42000000000000000000000
    return 0;
}
```

- **Expected:** the literal takes `int128` from the other operand, so the first two lines print
  `-42000000000000000000000` and `999999999999999999958`. A literal that cannot be represented
  should at least be diagnosed.
- **Actual:** `0` and `-42`, with no diagnostic. As an initializer (`let b: int128 = 1000…;`)
  the same literal is correct.
- **Course workaround:** `Numbers/WideInteger` gives the literal an `i128` suffix.

## D38. Implicit integer widening stops at 64 bits (fixed)

**Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.

- **Reproducers:**
  - `let y: int128 = x;` with `x: int64` gives `cannot assign 'int64' to 'int128'`.
  - `let c: uint128 = 7 * 3;` gives `cannot assign 'int' to 'uint128'`.
- **Notes:** `let a: uint64 = 7 * 3;` is accepted. `TypeRef::IsAssignableTo` in
  `Compiler/Types/Type.cpp` lists widening widths only up to 64.
- **Course workaround:** `Numbers/WideInteger` teaches an explicit `as` as the rule.

## P3. A precision placeholder on NaN or infinity prints a 309-digit number (fixed)

**Fixed**; covered by `Tests/Packages/Format/Scientific`, which also covers the new `{:e}` and `{:E}` scientific notation.

- **Reproducer:** `PrintLine("{:.3} {:.3}", nan, inf);` where `nan = 0.0 / 0.0` and
  `inf = 1.0 / 0.0`.
- **Expected:** `NaN Inf`. That is what `PrintLine("{} {}", nan, inf)` prints.
- **Actual:** two 309-digit integers followed by `.000`. `PrintLine("{:e} {:e}", nan, inf)`
  prints nothing at all.

## P4. Core's checked-arithmetic documentation contradicts its behaviour (fixed)

**Fixed**; covered by `Tests/Packages/Core/Arithmetic`, `Tests/Packages/Core/Conversion`, `Tests/Language/AllocationMath` and `PackageDocumentationStyleTests.cpp`.

- **Where:** the `@returns` tags of `AddChecked`, `SubChecked`, `MulChecked` and
  `ConvertChecked`.
- **Problem:** the tags say "leaving `result` unwritten". The prose above them, and the actual
  behaviour, write `result` in both cases.

Note: `{:b}` on a negative signed integer prints a minus sign and the magnitude, not
two's-complement bits. This may be intended, but it is undocumented. **Fixed** (documented as intended in `FormatBase` and the Text and Format READMEs); covered by `Tests/Packages/Format/Integers`.
## D39. Initializing an optional pointer with `null` crashes the compiler (fixed)

**Fixed**; covered by the `NullNativeTargets` golden and `Tests/Language/Pointers`.

```rux
func Main() -> int {
    let q: (*int)? = null;
    return 0;
}
```

- **Expected:** either a diagnostic, or a build in which `q` is present and holds a null pointer.
- **Actual:** `rux check` passes. `rux build` prints `Compiling … (Debug, Windows x86-64)` and
  then exits with code -1073740791 (0xC0000409, a stack buffer overrun), with no message.
- **Course workaround:** `Memory/OptionalPointer` goes through a binding,
  `let p: *int = null; let q: (*int)? = p;`, which builds and yields a present optional.

## P5. Stale package documentation (fixed)

**Fixed**; covered by `Tests/Packages/Allocator/Arena` and `PackageDocumentationStyleTests.cpp`. `BytesUsed` now sums every block.

- The header comment in `Packages/Allocator/Src/Box.rux` says a failure is reported through an
  out-parameter, but `Create` returns `Box<T> ! AllocError`.
- `Arena::BytesUsed` counts only the current block, which its documentation does not say.
## D40. A writable slice is rejected by a variadic interface parameter (fixed)

**Fixed**; covered by `Tests/Language/VariadicInterface`.

```rux
import Io::PrintLine;

func Show(text: char8[..]) {
    PrintLine("{}", text);
}

func Main() -> int {
    var buffer: char8[4];
    buffer[0] = c8'a';
    buffer[1] = c8'b';
    Show(buffer[..2]);              // accepted: var char8[..] converts to char8[..]
    PrintLine("{}", buffer[..2]);   // rejected
    return 0;
}
```

- **Expected:** accepted, as passing the same slice to `Show` is.
- **Actual:** `argument 2 to 'PrintLine' has type 'var char8[..]', but variadic parameter 'args'
  requires 'Display'`. The `var T[..]` → `T[..]` conversion is not considered before the interface
  conversion.
- **Course workaround:** `Files/File` and `Files/AtomicFile` bind a read-only view first.
## P6. `JsonEventReader::Failure()` reports an offset within the buffer, not within the document (fixed)

**Fixed**; covered by `Tests/Packages/Json/Events`. A failing source is now reported as `JsonParseError::Source`, with its `IoError` on `JsonEventReader::SourceError`.

- **Where:** `Packages/Json/Src/Events.rux`. `Refill` resets `position` to 0 whenever it drops
  consumed bytes, and `Failure()` builds its offset from `position`.
- **Reproducer:** a `Reader` that serves 4 bytes per read, over the text `[1, 2, 3, 4, 5}`. Read
  events until `JsonEvent::Error`, then print `reader.Failure().Offset()`.
- **Expected:** `14`, the position of the `}`.
- **Actual:** `2`.
- **Related:** a source `Read` failure other than end of stream is reported as
  `Lexical(UnexpectedByte)`, so the caller never receives the source's `IoError`.
- **Course workaround:** `DataFormats/JsonStream` prints only the failure's reason, not its offset.

Language note: a string literal cannot span lines, and literals cannot be concatenated. Multi-line
documents in lessons are therefore built line by line with a `StringBuilder`.
## D41. A `when` condition can read a Core field that is private when read as a value (fixed)

**Fixed**; covered by `Tests/Language/BuiltinAssert` and `ConditionalEvaluationTests.cpp`.

- **Reproducer:** `import Core::{ #build };`, then compare two forms:
  - `PrintLine("{}", #build.debugAssertions);` is rejected.
  - `when #build.debugAssertions { … } else { … }` is accepted.
- **Expected:** the same answer for both forms. Either the field is readable, or the `when` is
  rejected. `Build.debugAssertions` and `debugInfo` have no `pub` in `Packages/Core/Src/Build.rux`.
- **Actual:** the value read fails with `struct field 'debugAssertions' is private to package
  'rux/core'`, while the `when` is accepted and works. `Tests/Language/BuiltinAssert` relies on the
  `when` form.
- **Course impact:** `CompileTime/BuildMode` uses the `when` form, and breaks if the leak is
  closed without making the field `pub`.

## D42. A single-item import of a `#` name does not parse (fixed)

**Fixed**; covered by `Tests/Language/Int`.

- **Reproducer:** `import Core::#source;`.
- **Expected:** accepted, the same as `import Core::{ #source };`.
- **Actual:** `expected a module path segment after '::' before '#'`, followed by three cascading
  errors. `Docs/NativeOutcomes.md` lists this as a pitfall; it would be better fixed.

## D43. `#source.column` reports the column of `.column`, not of the expression (fixed)

**Fixed**; covered by `ConditionalEvaluationTests.cpp` and `Tests/Packages/Core/Source`.

- **Reproducer:** `    let column = #source.column;`, where the `#` is at column 18.
- **Expected:** `18`. Core documents the value as "the one-based column of the reading
  expression".
- **Actual:** `25`, the column of the `.`.
- **Course workaround:** `CompileTime/SourceLocation` leaves the column out.

## D44. An unknown `intrinsic func` is accepted and fails only at load time (fixed)

**Fixed**; covered by `IntrinsicDeclarationTests.cpp` and the `IntrinsicDeclarations` golden.

- **Reproducer:** `intrinsic func Frobnicate(value: int) -> int;`, then a `Main` that prints
  `"a"` and then `Frobnicate(2)`.
- **Expected:** a diagnostic that `Frobnicate` is not a compiler intrinsic. `intrinsic type int7`
  is already rejected this way.
- **Actual:** check and build pass. `rux run` prints nothing, not even `a`, and exits 127.
  Running the executable directly gives 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND).

## D45. A field the compiler does not supply on a user-declared `#target` struct reads garbage (fixed)

**Fixed**; covered by `IntrinsicDeclarationTests.cpp` and the `IntrinsicDeclarations` golden.

- **Reproducer:** `struct Target { pointerBits: uint; wordSize: uint; }`,
  `intrinsic #target: Target;`, then `PrintLine("{}", #target.wordSize);`.
- **Expected:** a compile error for an unknown context field.
- **Actual:** accepted, and prints `16974340`.

## D46. The signature of an intrinsic function is not enforced (fixed)

**Fixed**; covered by `IntrinsicDeclarationTests.cpp` and the `IntrinsicDeclarations` golden.

- **Reproducer:** a provider declares `pub intrinsic func Assert(condition: int, message: char8[..]);`,
  and `Main` calls `Assert(256, "x");`.
- **Expected:** rejected. `Docs/Architecture.md` says an inline intrinsic "is held to the signature
  it emits for".
- **Actual:** accepted. The call fails with `Assertion failed: x` and exit code 132, apparently
  because 256 is truncated to a zero byte.

**Minor:** the caret for an `#Error` or `#Warn` diagnostic points at the `(`, not at the `#` or
the start of the call. **Fixed**; covered by `ConditionalFoldingTests.cpp`.

**Stale test:** the TODO in `Tests/Packages/Core/Config/Src/Main.rux` says `#config.Has` and
`#config.Get` crash with 0xC0000139. They now work in executables, so that test body can be
re-enabled. **Fixed**; covered by `Tests/Packages/Core/Config`.
## D47. The AArch64 assembler rejects the `ble` spelling of `b.le` (fixed)

**Fixed**; covered by `AArch64AssemblerTests.cpp`, `AsmParserTests.cpp` and `Tests/Language/AsmAArch64`.

- **Reproducer:** `asm func F(n: int64) -> int64 { cmp x0, #0 \n ble done \n done: \n ret }` inside
  `when #target.arch { .AArch64 => {...}, else => {} }`, built with
  `rux build --target linux-aarch64`.
- **Expected:** builds. GNU as and LLVM accept `ble` as an alias of `b.le`.
- **Actual:** `unknown instruction 'ble'; did you mean 'bl'?`.
- **Course workaround:** `Platform/AsmArm` writes `b.le`.

Related to D40: a `var int[..]` view, or a `buffer[..n]` view taken from a `*var char8`, is
refused by `PrintLine`'s `{}` arguments in the same way as a writable `char8[..]`. **Fixed** for the `char8` views; covered by `Tests/Language/VariadicInterface`. A `var int[..]` is still refused because `int[..]` itself implements no `Display`, which is a package decision rather than this defect.

Related to D2: an unsuffixed character array literal does not take its element type from the
annotation. `var letters: char8[3] = ['a', 'b', 'c'];` gives `cannot assign 'char32[3]' to
'char8[3]'`, so each element needs a `c8` prefix. **Fixed**; covered by `Tests/Language/ExpectedTypeLiterals`.
## D48. A named `const` is rejected as an array length or repeat count (fixed)

**Fixed**; covered by `Tests/Language/Const`, `SemanticTests.cpp`, and the `ConstantInitializers` golden.

```rux
import Io::PrintLine;

const Limit: uint = 100;

func Main() -> int {
    var a: bool[Limit] = [false; Limit];
    PrintLine("{}", a.length);
    return 0;
}
```

- **Expected:** prints `100`. `Docs/Language.md` asks only for "a non-negative compile-time
  integer".
- **Actual:** `array length must be a non-negative compile-time integer` at `bool[Limit]`, and
  `array repeat count must be a non-negative compile-time integer` at `[false; Limit]`, followed by
  two cascading errors.
- **Cause:** `EvalConstInt` in `Compiler/Semantic/Analysis/ConstantChecking.cpp` folds only
  literals, unary and binary expressions. It never resolves a named constant.
- **Course workaround:**
  - `Projects/Prime` writes `[false; 100]` and reads the limit back from `.length`.
  - `Basics/Const` no longer claims that a constant can size an array.
## D49. A workspace root refuses registry dependencies in `rux check` and `rux doc` (fixed)

**Fixed**; covered by `PackageDependencyGraphTests.cpp`, `CompilerDriverTests.cpp` and `CliWorkspaceProcessTests.cpp`.

- **Reproducer:** a root manifest with `[Workspace] Packages = ["App", "Greeter"]`, where App
  depends on `Io = { Namespace = "Rux", Version = "*" }`. Run `rux check` at the root.
- **Expected:** Io resolves from the package cache, as it does when App is checked on its own.
- **Actual:** `package 'Rux/Io' is not a local workspace member; registry dependencies are
  disabled`.
- **Cause:** `localDependenciesOnly = true` for workspaces in `Cli/CmdCheck.cpp:216` and
  `Cli/CmdDoc.cpp:129`. A user workspace whose members use the standard library cannot be checked
  from its root.
- **Course workaround:** `Check.ps1` checks each workspace member from its own directory.

## D50. `rux build` and `rux run` at a workspace root print an empty package name and fail (fixed)

**Fixed**; covered by `CliWorkspaceProcessTests.cpp`.

- **Actual:** `Compiling  v (Debug, Windows x86-64)`, followed by `source directory '…\Src' does
  not exist`.
- **Expected:** a clear message that a workspace has nothing of its own to build, or a build of
  its members.
- **Related:** at a workspace root, `rux fmt --check` examines only the root's `Src` and reports
  "no source files were examined". `rux lint` at the same root does visit the members.

## D51. `rux test` resolves dependencies from local paths only (fixed)

**Fixed**; covered by `PackageDependencyGraphTests.cpp`, `CompilerDriverTests.cpp` and `CliWorkspaceProcessTests.cpp`.

- **Where:** `Cli/Testing/TestExecution.cpp:36`.
- **Problem:** a test package cannot import `Core::Assert`, and cannot depend on a package that
  uses `Io`. It fails with `package 'Rux/Io' is not a local workspace member; registry
  dependencies are disabled`. This makes `rux test` unusable for user packages built on the
  standard library.
- **Course workaround:** `Packages/Tooling` tests a dependency-free library, and reports failure
  through exit codes instead of `Assert`.

## D52. A shared library exports every public function of its dependencies (fixed)

**Fixed**; covered by `RcuLinkGraphTests.cpp`, `LirReachabilityTests.cpp` and `CompilerDriverTargetTests.cpp`.

- **Reproducer:** a `Type = "SharedLibrary"` package with an `Io` dependency and two `pub func`
  declarations. Build it, then run `llvm-readobj --coff-exports` on the DLL.
- **Expected:** 2 exports.
- **Actual:** 526 exports, including `Arena::Arena` and `BufferedReader::Read`, in a 1.11 MB DLL.
- **Course workaround:** `Packages/SharedLibrary` has no dependencies.

## D53. A module path that starts with its own package name resolves inconsistently (fixed)

**Fixed**; covered by `SemanticVisibilityTests.cpp` and `Tests/Language/ModulePackagePrefix`.

- **Reproducer:** in package `Mod2`, declare `module Mod2::Rectangle { func Area(...) }`.
  - `import Mod2::Rectangle::Area;` works.
  - `import Mod2::Mod2::Rectangle::Area;` also works.
  - `import Mod2::Rectangle;` followed by `Rectangle::Area(3, 4)` fails with `name 'Rectangle' was
    not found in package 'Mod2'`, and suggests `import Mod2::Mod2::Rectangle`.
- **Problem:** item imports and module imports disagree about where the module lives.
- **Course workaround:** `Packages/Module` uses module paths that do not repeat the package name.

## D54. A grouped import of a private item reports the error twice, with an internal package name (fixed)

**Fixed**; covered by `SemanticVisibilityTests.cpp` and `SemanticNameDiagnosticsTests.cpp`.

- **Reproducer:** `import Tally::{ Counter, Clamp };` where `Clamp` is private.
- **Actual:** the error is printed twice, and names the package `local:D:/…/Tally/Rux.toml` rather
  than `Tally`.

## T1. `rux fmt` reorders manifest keys against the documented manifest order (fixed)

**Resolved** without a formatter change: `Docs/Manifest.md` and every repository manifest already put `Description` before `Authors`, the order `rux fmt` writes; the course manifests should follow it.

`rux fmt` puts `Description` before `Authors`, while `Docs/Manifest.md` examples and every course
manifest put `Authors` first. As a result, `rux fmt --check` fails on every course package. Either
the formatter or the documented order should change. The course keeps its order until this is
decided.

## D55. A non-literal argument to `#config.Has` or `#config.Get` compiles and fails at load time (fixed)

**Fixed**; covered by the `CompilerParameterArguments` golden.

- **Reproducer:** `let name = "ANOTHER_UNDEFINED_NAME";`, then `#config.Has(name)` in an executable.
- **Expected:** a compile error, because Core documents the argument as a string literal resolved while compiling.
- **Actual:** check and build pass, and the program exits 127 before `Main` runs.
- **Found by:** re-enabling `Tests/Packages/Core/Config`, which now passes literals only.
