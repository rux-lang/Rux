# Compiler architecture

Rux is split into CMake component targets whose dependencies follow the
compilation pipeline. Folder ownership and target ownership are intentionally
the same.

```text
Source -> Lexer -> Syntax -> SemanticModel
                                |
                                v
                          AST-to-HIR -> HIR passes
                                |
                                v
                          HIR-to-LIR -> LIR passes
                                |
                                v
                         x86-64 CodeGen -> RCU Object -> Linker
```

`Target` describes the output machine. `System` describes the host running the
compiler. Code generation and linking must use target data rather than host
preprocessor checks.

The principal ownership namespaces currently enforced at cross-platform and
orchestration boundaries are `Rux::Target`, `Rux::System`, and `Rux::Driver`.
New standalone tools use `Rux::Formatting` and `Rux::Linting`. Existing language
model types remain in `Rux` while those large APIs are migrated incrementally;
new code must not add declarations to `Misc` or recreate a generic `Utils`
component.

The build exposes focused targets such as `RuxSyntax`, `RuxSemantic`, `RuxHir`,
`RuxLir`, `RuxLowering`, `RuxCodeGenX86_64`, `RuxObjectRcu`, `RuxLinker`, and
`RuxDriver`. `RuxCore` is an interface-only compatibility aggregation target.

Formatter and linter are internal compiler components exposed through the
single `rux` application:

```text
rux fmt  -> RuxFormatter -> RuxSyntax
rux lint -> RuxLinter    -> RuxSyntax + RuxSemantic
rux      -> RuxDriver
```

Operating-system APIs are confined to `Compiler/System`; the CI isolation
guard is `Tools/PlatformIsolation/Check.sh`.
