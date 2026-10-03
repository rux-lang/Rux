# Compiler Defects

Known compiler bugs that shape how package and test code is written. Each entry says what goes wrong, how to reproduce it, and the workaround code currently relies on. Do not "tidy away" a workaround while its defect is open; when a defect is fixed, remove its entry and the workarounds it names in the same change.

## An expected type does not reach the arms of a `match`

A `match` expression takes its result type from its first arm rather than from the type its context expects, whether that is a `let` annotation or the enclosing function's return type. When the first arm is a contextual `none`, the result is typed `opaque?` and every later arm is rejected:

```rux
func Kind(outcome: int32 ! bool) -> int32? {
    let reported: int32? = match outcome {
        .Success(_) => none,
        .Failure(_) => .Some(1i32)
    };
    return reported;
}
```

reports `error: match arm type mismatch: expected 'opaque?', found 'int32?'` on the second arm, and `return match outcome { ... }` with the same arms reports the same error.

**Workaround:** put an arm whose type is complete first. `Tests/Packages/Io/TextRead` orders its arms this way.

## Native temporaries are not reused across a frame

Lowering gives every native temporary its own stack slot instead of reusing slots whose lifetimes do not overlap, so a function that builds many native values has a large frame. In a recursive function the cost multiplies by the recursion depth: the Json parser's `ParseValue` frame grew by about 9 KB per nesting level while it still contained the number and string parsing.

**Workaround:** keep native-heavy work out of recursive functions. `Packages/Json/Src` splits `ParseNumber` and `ParseText` out of `ParseValue` so the recursive frame stays small.
