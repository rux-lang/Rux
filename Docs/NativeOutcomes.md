# Native Sums, Optionals, and Fallibles

This page records why the native outcome forms are shaped the way they are, where the compiler implements them, and how code written against the retired `Result`/`Option` protocols migrates. The user-facing rules themselves are in the [language guide](Language.md#native-sums-optionals-and-fallibles); this page is the rationale and the map behind them. Revisit a decision here before changing the behavior that depends on it.

## Forms and Terms

| Form                    | Purpose                                                                                     | Inspection                                                              |
| ----------------------- | ------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| `variant Event { ... }` | Alternatives distinguished by meaningful case names, even with identical payload types.     | Named case patterns.                                                    |
| `A \| B`                | One value of a set of distinct resolved types: a _sum_.                                     | Typed patterns, qualified case patterns, `is`.                          |
| `T ! E`                 | A successful `T` or a failed `E`: a _fallible_. Either payload may be any well-formed type. | `.Success(p)`, `.Failure(p)`, `catch`, `?`, `? else`.                   |
| `! E`                   | Exactly `() ! E`: successful completion or a failed `E`.                                    | The same operations as every fallible.                                  |
| `T?`                    | A present `T` or absence: an _optional_, preserving every level.                            | `value?`, `.Some(p)`, `none`, typed presence patterns, `is`, `?`, `??`. |
| `()`                    | The built-in empty tuple, the _unit_, whose only value is also `()`.                        | The pattern `()` or a binding.                                          |
| `union Storage { ... }` | Untagged overlapping storage.                                                               | Field access.                                                           |

Diagnostics, lint messages, and these docs use one vocabulary: a _member_ is one type of a sum; a _channel_ is the success or failure side of one fallible; a _level_ is one optional or fallible nesting step; the _native constructors_ are `.Success`, `.Failure`, and `.Some`; the _presence suffix_ is the pattern form `p?`; the _typed presence pattern_ is `v: T` on an optional subject.

## Design Decisions

| #   | Topic                     | Decision                                                                                                                                                                                                                         | Reason                                                                                                           |
| --- | ------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------- |
| D1  | Terminology               | `A \| B` is a _sum type_; `union` keeps naming overlapping storage.                                                                                                                                                              | Distinct storage contracts stay distinct in docs and diagnostics.                                                |
| D2  | Optional model            | `T?` is compiler-owned and non-collapsing; `T??` is distinct. There is no `None` type.                                                                                                                                           | Iterators and maps must tell a missing element from a present absent value.                                      |
| D3  | Optional construction     | `none` is outer absence; `.Some(value)` explicitly selects presence.                                                                                                                                                             | Every nested state is constructible without temporaries.                                                         |
| D4  | Generic sums              | `T \| U`, `E \| Timeout`, and repeated outer constructors are legal; sums normalize after substitution and dependent operations are rechecked.                                                                                   | Sum identity is a set of types, not left/right provenance; instantiation-time diagnostics are the accepted cost. |
| D5  | Member order              | Canonical order derives from the stable fully qualified resolved identity.                                                                                                                                                       | Independently compiled objects agree on the same concrete type.                                                  |
| D6  | Unit success              | The unit is the empty tuple `()`; `! E` is exactly `() ! E`.                                                                                                                                                                     | One generic fallible model, with no library type recognized by name.                                             |
| D7  | First-class construction  | `.Success(value)` and `.Failure(error)` are values; `return` and `fail` remain convenient control flow.                                                                                                                          | Storing, passing, and transforming a failure needs no helper call.                                               |
| D8  | Nesting                   | Nested fallibles and optional error payloads are preserved; nested `!` and directly written optional errors are grouped.                                                                                                         | Composition never erases a channel or turns an inner failure into an outer one.                                  |
| D9  | Conversion                | Identity first; otherwise exactly one permitted construction or widening route, and competing interpretations are rejected.                                                                                                      | Expected-type convenience never silently chooses forwarding over nesting.                                        |
| D10 | Type binding              | `*T?` is `*(T?)`; `(*T)?` is an optional pointer; references follow the same rule.                                                                                                                                               | It matches every other postfix suffix.                                                                           |
| D11 | Matching                  | Ordinary `match` covers success, failure, and presence patterns; there is no combined `match ... catch`.                                                                                                                         | One compositional matching model works for nested values.                                                        |
| D12 | Recovery                  | `catch` is postfix-tight and processes one outer fallible level.                                                                                                                                                                 | Recovery has a small, locally visible subject.                                                                   |
| D13 | Error conversion          | `value? else (e => expression)` binds the complete error and may use local context; error channels stay explicit.                                                                                                                | Adding a filename or other context needs no one-argument helper and no implicit conversion.                      |
| D14 | Inspection                | `is` tests exact sum membership or one optional level; interface tests remain separate and unimplemented; no flow narrowing.                                                                                                     | Type membership, interface implementation, and payload extraction stay separate questions.                       |
| D15 | Discard checks            | Directly discarded fallibles and never-read fallible locals are diagnosed; an exhaustive match may discard either channel deliberately.                                                                                          | Practical diagnostics, not a proof that every stored error is handled.                                           |
| D16 | Representation            | Channels stay semantically distinct; a shared layout may use only payload-preserving niches and never rewrites an inner tag.                                                                                                     | Borrowed payload access and pass-through need every nested payload addressable in place.                         |
| D17 | Tooling                   | Every syntax form is supported by the formatter, dumps, and lint traversal.                                                                                                                                                      | Every fixture survives the workspace tooling.                                                                    |
| D18 | Migration                 | Conflicting identifiers were renamed before `none`, `fail`, and `catch` were reserved; the shape-recognized `Result`/`Option` protocols coexisted until the packages migrated, then were removed.                                | Coexistence alone could not prevent keyword breakage, and removal had to wait for an audited migration.          |
| D19 | Public errors             | Exact sum widening is available; named error variants keep a public abstraction and original causes.                                                                                                                             | An alias of a sum hides no dependency type and does not stabilize exhaustive callers.                            |
| D20 | Presence suffix           | A tight postfix `?` on a primary pattern is exact shorthand for `.Some(pattern)`; `value??` peels two levels.                                                                                                                    | The common present case needs no constructor name or repeated payload type, mirroring expression `?`.            |
| D21 | Extension of native forms | Native forms are not `extend` targets and implement no interface; reusable operations are generic functions whose parameters spell the forms, and inference descends through them.                                               | Anonymous compiler-owned types have no declaring package to own a method set.                                    |
| D22 | Optional widening         | `A?` widens to `(A \| B)?` by injection or subset widening of the payload while preserving absence; presence is never constructed inside an existing optional.                                                                   | It mirrors fallible channel widening, and identity still wins at the outer level.                                |
| D23 | Collapse-safe `else`      | An `else` arm is never diagnosed as unreachable; only typed, qualified, native, literal, and unit arms covered by earlier unguarded arms are.                                                                                    | `x: T => ..., else => ...` must stay valid when `T = U`.                                                         |
| D24 | Free-name binding         | An identifier pattern naming a type, alias, generic parameter, enum, variant, interface, constant, function, or module, or a case of the subject, is rejected instead of binding; shadowing a variable or parameter stays legal. | With typed patterns, `Options =>` is the obvious slip, and a bare binding silently matches everything.           |
| D25 | Fallible `Main`           | `Main` may return `! E` or `int ! E`; a failure runs ordinary cleanup and exits with status 1 without printing.                                                                                                                  | Every executable test is an integer `Main`; otherwise each top-level `?` needs a wrapper.                        |

After the migration, failure and absence are only `T ! E` and `T?`. A variant with `Success`/`Error` or `Some`/`None` cases is an ordinary variant whatever it is called: `?` and `??` reject it, a `Next` returning a `Some`/`None` variant is rejected at its declaration, and the case names are consulted only to word those diagnostics.

## Conversion Routes

Where a typed value meets a known native destination, aliases are resolved and sums normalized, exact identity wins, and otherwise the permitted routes are enumerated by the tag selections they produce. Exactly one route must apply; none is an incompatible type and several are an ambiguous construction that must be written out. Context flows into conditional and match arms but never invents a destination sum or chooses an error type.

| Destination      | Permitted implicit route                                                                                                                                                            |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Sum `A \| B`     | Inject an exact member or widen a subset sum; no conversion is searched inside members. An unsuffixed numeric literal targets the one member of its kind, independent of its value. |
| Optional `P?`    | Construct outer presence when the source converts uniquely to `P`, or widen the payload while preserving absence.                                                                   |
| Fallible `P ! E` | Construct outer success when the source converts uniquely to `P`, or preserve an existing fallible's outer channel by channel widening.                                             |
| Anything else    | Ordinary conversions only.                                                                                                                                                          |

The required counterexample is `R = int32 ! E` converted to `(int32 | R) ! E`: forwarding `R` and storing it as successful data are both possible, so the bare conversion is rejected, `.Success(r)` stores it, and `r?` forwards it. For the same reason a fallible source takes a success or presence route only when its error has no route into the destination's error channel. The neighbouring cases are `R` into `R ! F` with `E` not in `F` (unique success), into `int32 ! (E | F)` (unique channel widening), into `R?` and `(int32 ! E)?` (presence), and into `int32? ! E` (incompatible, because channel widening never constructs presence inside a channel).

`fail error;` explicitly selects the enclosing outer failure and converts only its operand. `fail`, `return`, `break`, `continue`, `Panic`, and no-return calls are diverging forms that never decide a match or `??` result type, and they are accepted only as a statement, a whole arm body, a whole mapper body, or the whole right operand of `??`.

## Representation and Ownership

- Every native level is a tagged aggregate with an 8-byte tag at offset 0 and each case's payload at `AlignUp(8, payloadAlign)`. Absent is tag 0 and present tag 1; success is 0 and failure 1; a sum member's tag is its canonical index. Tags are not a stable public ABI.
- A zero-sized payload reserves no bytes anywhere: `sizeof(())` is 0 with alignment 1, `! E` is a tag plus `E`, and `()?` is the tag alone. Semantic layout, code-generation layout, both frame planners, and both call emitters read the same record.
- A borrowed subject is matched in place: bindings alias the payloads where they lie, and a subset binding reads the original tag through the recorded mapping. Such a view can be read and matched but not re-borrowed, passed by reference, stored, or moved; only a single-member binding writes through. An owned subset binding remaps tags, copying or moving with its subject.
- `?`, mapped `?`, `catch`, and `??` consume their evaluated operand. A named copyable operand is copied and a named move-only one is written `(<-value)?`; a borrowed outcome is never an operand. The continuing payload and the outgoing error move with their move operations, so a payload whose move is prohibited is rejected at the operator, or at instantiation for a type parameter. Propagation, mapping, and `fail` capture the outgoing payload before running defers.
- A mapper's path always leaves through the failure exit, so a local moved only inside the mapper is still owned on the continuing path; the move only suppresses its destruction on that exit.
- Equality compares tags level by level, then the active payload. A comparison supplies an expected type only to an operand without one of its own (`none`, a native constructor, an unsuffixed literal), so `opt == 5i32` and `sum == member` are rejected.

## Discard Diagnostics and Their Limits

An expression statement producing a fallible, a bare-expression arm of a match statement producing one, and `let _ = outcome;` are errors; a fallible local that nothing reads is a warning, because many fixtures hold a fallible only to observe its destruction. Reading, passing, returning, storing, or overwriting a fallible counts as a use, and nothing is followed through fields, containers, sums, optionals, or later writes. These are practical checks, not eventual-handling proofs. A two-channel match with `.Success(_) => {}` and `.Failure(_) => {}` discards any fallible deliberately; `catch { else => {} }` does so for a unit success only.

## Public Error Boundaries

Signatures name their error channels explicitly. Widening an internal error sum is convenient, but adding a member changes the public contract and can break exhaustive callers, and an alias hides none of its members. Prefer a named public error variant where callers should depend on a stable domain boundary, and wrap lower-level errors explicitly with their causes and context. There is no base error interface, automatic erasure, implicit conversion, or inferred error set.

## Non-Goals and Their Equivalents

Deliberately not introduced: optional chaining `?.`, a brace-less `catch fallback`, `try` blocks, inferred error sets, `From`-style implicit error conversion, `if let`/`guard let`, flow narrowing, closures, exceptions, fallible loops, and `extend` or interface implementations on native forms.

| Wanted                              | Write                                                                            |
| ----------------------------------- | -------------------------------------------------------------------------------- |
| `let else`                          | `let v = option ?? return none;` or `let v = result catch { _ => return ...; };` |
| A bare `catch fallback`             | `result catch { else => fallback }`                                              |
| `From` conversion                   | `value? else (e => Wrap(e, context))`                                            |
| `unwrap`/`expect` in a test         | `let value = result catch { e => Panic(message) };`                              |
| A method on an optional or fallible | A generic free function, such as `Core::Succeeded` and `Core::Failed`            |
| Absence turned into a failure       | `option ?? fail NotFound {}`                                                     |

## Where the Compiler Implements Them

- **Type model** (`Compiler/Types/Type.h`, `Type.cpp`): kinds `Sum`, `Optional`, and `Fallible`; `MakeSum` flattens, sorts by canonical spelling, deduplicates, and reduces singletons, and `Renormalize` repeats that after substitution; `MakeUnit` is the empty tuple; `MakeNoneValue` is the placeholder type of a contextual `none`. `MangledSpelling` escapes `?`, `|`, and `!` as `_O`, `_S`, and `_F` in linker names.
- **Conversions** (`Compiler/Types/NativeConversion.{h,cpp}`): `ClassifyNativeConversion(source, destination)` returns identity, a route of steps, incompatible, or ambiguous; `TypeRef::IsAssignableTo` defers to it whenever a native type is involved.
- **Layout** (`Compiler/Types/NativeLayout.{h,cpp}`): `ComputeNativeLayout` is the one layout record; semantic `LayoutOfTypeRef` and code generation's `RuntimeNativeLayout`, `RuntimeSizeOf`, `SizeOf`, and `AlignOf` all use it.
- **Pattern selection** (`Compiler/Types/NativeSelection.h`): `ClassifyNativeSelection(subject, annotation)` decides whether a typed pattern binds the whole subject, sum members, or one presence level.
- **Syntax** (`Syntax/Ast/Ast.h`, `Parser/ParserType.cpp`, `ParserExpr.cpp`, `ParserStmt.cpp`): `SumTypeExpr`, `OptionalTypeExpr`, `FallibleTypeExpr`; `CatchExpr`, `MappedTryExpr`, `NativeConstructExpr`, `NoneExpr`, `DivergeExpr`; `FailStmt`; `TypedPattern`, `PresencePattern`, `NonePattern`.
- **Semantic analysis**: `TypeResolution.cpp` resolves the forms; `Assignments.cpp` routes conversions and records `nativeConversions`; `Statements.cpp` checks `return`, `fail`, and unit completion; `Propagation.cpp`, `Recovery.cpp`, and `Iteration.cpp` check `?`, `??`, `catch`, mapping, and `Next`; `PatternCoverage.cpp` checks native coverage, rechecked per instantiation by `ValidateDeferredPatternChecks`.
- **Lowering**: `AstToHirNative.cpp` builds every level as a variant-form `HirEnumConstructExpr` with the native tag, every native pattern as a `HirEnumPattern` with that tag, widening as a match that rebuilds each alternative, and `?`, `??`, and `catch` as matches whose exits are ordinary returns. HirToLir treats natives as aggregate enums with an `int64` tag, and both back ends pass them as aggregates.

## Migrating from Result and Option

Code written against shape-recognized `Result`/`Option` variants migrates leaf APIs first, because a legacy variant and a native form never convert into each other implicitly.

| Old usage                                                                 | Native form                                                                                    |
| ------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| `Result<T, E>`                                                            | `T ! E`                                                                                        |
| `Result<Unit, E>`                                                         | `! E`; native `()` and a library `Unit` are unrelated types                                    |
| `Result<Result<T, E1>, E2>`                                               | `(T ! E1) ! E2`                                                                                |
| `return Result::Success(value)`                                           | `return value;` when unambiguous, otherwise `return .Success(value);`                          |
| `return Result::Error(error)`                                             | `fail error;` or `return .Failure(error);`                                                     |
| `Option<T>`, `Option<Option<T>>`                                          | `T?`, `T??`                                                                                    |
| `Option::Some(value)`, `Option::None`                                     | `.Some(value)` (or a plain value when unambiguous), `none`                                     |
| Option matching                                                           | `value?` or `.Some(pattern)`, and `none`                                                       |
| `IsSome`/`IsNone`                                                         | `value is T`, or a presence match                                                              |
| `ValueOr` on an option / on a result                                      | `option ?? fallback` / `result catch { else => fallback }`                                     |
| `IsSuccess`/`IsError`                                                     | A two-arm match, or `Core::Succeeded`/`Core::Failed`                                           |
| Assertion followed by extraction                                          | `let value = result catch { e => Panic(message) };`                                            |
| Status enum with a success member (`-> IoError` with `None = 0`)          | `! E` with the success member, `IsOk`, and `IsError` removed                                   |
| Output slot beside a status (`Write(bytes, taken: *var uint) -> IoError`) | The slot becomes the success payload: `uint ! IoError`                                         |
| Output slot beside an option (`Replace(..., error: *var E) -> Option<V>`) | `V? ! E`                                                                                       |
| Iterator `Next -> Option<Item>`                                           | `Next -> Item?`                                                                                |
| An interface requirement returning a status                               | Change the requirement and every implementation together; an implementation must match exactly |

Idioms that worked well during the first-party migration:

- A fallible test `Main` (`func Main() -> ! E` or `-> int ! E`) lets every step use `?`; a failure exits with status 1 and fails the test.
- `call() catch { else => Panic(message) }` for an expected success inside a non-fallible function, and a per-file helper for an expected failure:

  ```rux
  func Refusal<T, E>(outcome: T ! E) -> E {
      return match <-outcome {
          .Success(_) => Panic("expected a failure"),
          .Failure(error) => error
      };
  }
  ```

- Cleanup whose outcome does not matter writes `Close(file) catch { else => {} };`, because a bare fallible statement is an error.
- An assertion that a failed constructor "comes back empty" is dropped: no value comes back.

Pitfalls:

- `==` binds tighter than `??`: write `a == (b ?? c)`.
- `opt == none` and `opt == .Some(v)` need `==` on the payload; where the payload has none, test `opt is T` or match.
- A named move-only value as a `match` or `catch` subject is written `<-name`.
- A block arm `{ ... }` completes with `()`, so it is valid only for a unit success unless it ends by leaving.
- A helper taking `&(T ! E)` needs a place to borrow from: call it on a named local, or take the fallible by value.

## Regression Coverage

The executable fixtures `Tests/Language/SumType`, `Fallible`, `Optional`, `None`, `OptionalIteration`, `Fail`, `Catch`, `ErrorMapping`, `GenericSum`, and `NativeOutcomes` exercise the forms end to end; `NativeOutcomes` is the reference for the target idioms. The `NativeOutcomeSpellings`, `NativeOutcomeMisuse`, and `FallibleDiscard` goldens pin the rejected spellings, and the `Native*` unit tests cover each compiler stage. Changing any decision above means updating the affected fixtures and goldens in the same change.
