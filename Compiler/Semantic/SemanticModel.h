#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Semantic/CompileTimeContext.h"
#include "Semantic/Type.h"
#include "Syntax/Ast/Ast.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rux {
using SemanticDiagnostic = Diagnostic;

struct SemanticSymbol {
    enum class Kind {
        Var,
        Func,
        Type,
        Const,
        Module,
        Interface,
    };

    Kind kind = Kind::Var;
    std::string name;
    std::string sourceName;
    SourceLocation location;
    std::string resolvedType;
    bool isMut = false;
};

// The callable selected for an accepted CallExpr. Declaration pointers refer
// into the analyzed AST and therefore have the same lifetime requirements as
// the model's node-keyed type facts.
struct ResolvedCallableBinding {
    enum class DispatchKind {
        Direct,
        Method,
        Interface,
        Indirect,
        EnumVariant,
    };

    DispatchKind dispatch = DispatchKind::Indirect;
    const Decl *selectedDeclaration = nullptr;
    const EnumDecl::Variant *selectedVariant = nullptr;
    std::unordered_map<std::string, TypeRef> substitutions;
    std::optional<TypeRef> receiverType;
    std::optional<std::size_t> variadicBoundary;
    CallingConvention callingConvention = CallingConvention::Default;
    std::string importedSymbolOverride;
    // Final linker-visible name for direct calls. Interface and indirect
    // dispatch do not have one statically selected target.
    std::string linkerName;
};

// Final linker-visible identity of a declaration that emits or imports a
// symbol. Accepted generic calls record their concrete instance separately.
struct ResolvedSymbolIdentity {
    std::string linkerName;
};

// Final identity and slot targets of an emitted interface vtable.
struct ResolvedVtableIdentity {
    std::string linkerName;
    std::vector<std::string> entries;
};

// Persistent output of semantic analysis. Besides diagnostics and exported
// symbols it owns the ordered, validated module view and resolved type facts
// consumed by lowering. The model does not own the AST: every Module supplied
// to SemanticAnalyzer must outlive the model and remain unchanged while its
// node-keyed facts are queried.
struct SemanticModel {
    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    std::vector<const Module *> modules;
    CompileTimeContext compileTimeContext;

    SemanticModel(std::vector<SemanticDiagnostic> inputDiagnostics, std::vector<SemanticSymbol> inputSymbols,
                  std::vector<const Module *> inputModules, CompileTimeContext inputCompileTimeContext,
                  std::unordered_map<const Expr *, TypeRef> inputExpressionTypes,
                  std::unordered_map<const TypeExpr *, TypeRef> inputTypeNodeTypes,
                  std::unordered_map<const Pattern *, TypeRef> inputPatternTypes,
                  std::unordered_map<const CallExpr *, ResolvedCallableBinding> inputCallableBindings,
                  std::unordered_map<const Decl *, ResolvedSymbolIdentity> inputSymbolIdentities,
                  std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> inputVtableIdentities);

    [[nodiscard]] bool HasErrors() const noexcept;

    // Returns null when analysis did not accept the node with a resolved type.
    // Returned pointers remain valid for the lifetime of this model.
    [[nodiscard]] const TypeRef *TryGetType(const Expr &expression) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const TypeExpr &typeNode) const noexcept;
    [[nodiscard]] const TypeRef *TryGetType(const Pattern &pattern) const noexcept;

    // Returns null for rejected calls and nodes outside the analyzed modules.
    // Returned pointers remain valid for the lifetime of this model.
    [[nodiscard]] const ResolvedCallableBinding *TryGetCallableBinding(const CallExpr &call) const noexcept;

    // Returns null for declarations that do not emit/import a symbol and for
    // nodes outside the analyzed modules.
    [[nodiscard]] const ResolvedSymbolIdentity *TryGetSymbolIdentity(const Decl &declaration) const noexcept;

    // Returns null for extend blocks that do not emit an interface vtable.
    [[nodiscard]] const ResolvedVtableIdentity *TryGetVtableIdentity(const ImplDecl &declaration) const noexcept;

private:
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> callableBindings;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> vtableIdentities;
};
} // namespace Rux
