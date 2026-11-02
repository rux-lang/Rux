// Function literals: `func(x: int32) -> bool { return x > 100; }` written as a value. A literal captures nothing, so
// it is checked as an ordinary function whose body sees the module-level names of the file it is written in and none
// of the locals or parameters around it. Parameter and result types left out are taken from the function type the
// literal's destination expects; what was taken is recorded as the type of the `InferredTypeExpr` that stood for it,
// so everything that resolves the literal's signature afterwards, lowering included, reads the same types.

#include "Semantic/Analysis/AnalysisContext.h"

#include <format>
#include <string>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
/// The scope a file's declarations are visible in: the first scope, walking out from `scope`, that looks through to a
/// module's declarations. A literal written outside any function is already there.
Scope *FileScopeOf(Scope *scope) {
    for (Scope *candidate = scope; candidate != nullptr; candidate = candidate->Parent()) {
        if (candidate->Declarations() != nullptr) {
            return candidate;
        }
    }
    return scope;
}
} // namespace

std::string AnalysisContext::FunctionLiteralName(const FuncLiteralExpr &literal) const {
    std::string owner;
    if (currentFunctionLiteral) {
        if (const auto known = functionLiterals.find(currentFunctionLiteral); known != functionLiterals.end()) {
            owner = known->second;
        }
    }
    else if (currentFunctionDecl) {
        const auto module = functionModulePaths.find(currentFunctionDecl);
        const std::string prefix =
            module != functionModulePaths.end() && !module->second.empty() ? module->second + "::" : "";
        owner = currentImpl ? prefix + currentImpl->typeName + "::" + currentFunctionDecl->name
                            : prefix + currentFunctionDecl->name;
    }
    // The position keeps two literals of one function apart without a counter, so checking a body again, as a generic
    // instantiation does, names each literal the same way.
    return std::format("{}$literal{}_{}", owner, literal.location.line, literal.location.column);
}

bool AnalysisContext::ReportCapture(const SourceLocation location, const std::string &name) {
    if (literalSites.empty()) {
        return false;
    }
    const LiteralSite &site = literalSites.back();
    if (name == "self" ? !site.inImpl : site.scope == nullptr) {
        return false;
    }
    if (name != "self") {
        const Symbol *symbol = site.scope->Lookup(name);
        if (!symbol || symbol->kind != Symbol::Kind::Var || AstCast<const GlobalDecl>(symbol->declaration) ||
            AstCast<const ExternVarDecl>(symbol->declaration)) {
            return false;
        }
    }
    EmitError(location, std::format("function literals cannot capture '{}'", name),
              {"a function literal is an ordinary function with no environment, so it sees module-level names but no "
               "local or parameter of the function it is written in"},
              "pass it as an argument or use a named function");
    return true;
}

TypeRef AnalysisContext::CheckFunctionLiteral(const FuncLiteralExpr &literal) {
    if (!literal.function || !literal.function->body) {
        return TypeRef::MakeUnknown();
    }
    const FuncDecl &function = *literal.function;
    std::optional<TypeRef> expected;
    if (const auto found = expectedExpressionTypes.find(&literal);
        found != expectedExpressionTypes.end() && found->second->kind == TypeRef::Kind::Func) {
        expected = found->second;
    }
    // A function type lists its parameters and then its result.
    const bool shapeFits = expected && (*expected)->inner.size() == function.params.size() + 1;
    bool complete = true;
    for (std::size_t index = 0; index < function.params.size(); ++index) {
        const Param &parameter = function.params[index];
        if (!AstCast<const InferredTypeExpr>(parameter.type.get())) {
            continue;
        }
        if (shapeFits) {
            typeNodeTypes.insert_or_assign(parameter.type.get(), (*expected)->inner[index]);
            continue;
        }
        complete = false;
        EmitError(parameter.location, std::format("function literal parameter '{}' has no type", parameter.name),
                  {expected ? std::format("the expected type '{}' takes {} parameter{}, and the literal has {}",
                                          expected->ToString(), (*expected)->inner.size() - 1,
                                          (*expected)->inner.size() == 2 ? "" : "s", function.params.size())
                            : std::string("nothing here expects a function type that would supply it")},
                  std::format("write its type, as in 'func({}: int32) {{ ... }}'", parameter.name));
    }
    if (function.returnType && AstCast<const InferredTypeExpr>(function.returnType->get()) && shapeFits) {
        typeNodeTypes.insert_or_assign(function.returnType->get(), (*expected)->inner.back());
    }
    if (!complete) {
        return TypeRef::MakeUnknown();
    }
    const std::string name = FunctionLiteralName(literal);
    functionLiterals.insert_or_assign(&literal, name);

    // The body is checked as a function of its own, from the file's scope, with nothing of the enclosing body's
    // control flow, error handling or unsafe context in force.
    literalSites.push_back({currentScope, inImpl});
    Scope *const savedScope = std::exchange(currentScope, FileScopeOf(currentScope));
    const FuncLiteralExpr *savedLiteral = std::exchange(currentFunctionLiteral, &literal);
    const int savedLoopDepth = std::exchange(loopDepth, 0);
    auto savedLabels = std::exchange(activeLabels, {});
    auto savedDeferred = std::exchange(checkingDeferred, std::nullopt);
    auto savedLoopChecked = std::exchange(loopCheckedExpressions, {});
    auto savedDefaultParameters = std::exchange(defaultValueParameters, {});
    const bool savedInImpl = std::exchange(inImpl, false);
    const ImplDecl *savedImpl = std::exchange(currentImpl, nullptr);
    const TypeRef savedSelfType = std::exchange(currentSelfType, TypeRef::MakeUnknown());
    const TypeRef savedExtendedType = std::exchange(currentExtendedType, TypeRef::MakeUnknown());
    auto savedHandlers = std::exchange(throwHandlerFrames, {});
    auto savedMarks = std::exchange(throwMarkFrames, {});
    const Expr *savedOutcome = std::exchange(outcomeOperand, nullptr);
    auto savedUnsafeFrames = std::exchange(unsafeBlockFrames, {});
    const FuncDecl *savedUnsafeFunction = std::exchange(unsafeFunction, nullptr);
    const Expr *savedCallee = std::exchange(calleeBeingChecked, nullptr);
    const Expr *savedAddress = std::exchange(addressOperand, nullptr);
    const IndexExpr *savedIndexTarget = std::exchange(indexAssignmentTarget, nullptr);
    const bool savedPlainTarget = std::exchange(checkingPlainAssignmentTarget, false);
    const bool savedProjectionRoot = std::exchange(checkingBorrowProjectionRoot, false);
    auto savedPartRead = std::exchange(partReadPath, std::nullopt);
    // A literal is lowered with the function it is written in, so its body is checked even where that function's
    // module reuses its code and only a generic body would be.
    const bool savedDeclarationsOnly = std::exchange(checkingDeclarationsOnly, false);

    CheckFuncDecl(function);

    checkingDeclarationsOnly = savedDeclarationsOnly;
    partReadPath = std::move(savedPartRead);
    checkingBorrowProjectionRoot = savedProjectionRoot;
    checkingPlainAssignmentTarget = savedPlainTarget;
    indexAssignmentTarget = savedIndexTarget;
    addressOperand = savedAddress;
    calleeBeingChecked = savedCallee;
    unsafeFunction = savedUnsafeFunction;
    unsafeBlockFrames = std::move(savedUnsafeFrames);
    outcomeOperand = savedOutcome;
    throwMarkFrames = std::move(savedMarks);
    throwHandlerFrames = std::move(savedHandlers);
    currentExtendedType = savedExtendedType;
    currentSelfType = savedSelfType;
    currentImpl = savedImpl;
    inImpl = savedInImpl;
    defaultValueParameters = std::move(savedDefaultParameters);
    loopCheckedExpressions = std::move(savedLoopChecked);
    checkingDeferred = std::move(savedDeferred);
    activeLabels = std::move(savedLabels);
    loopDepth = savedLoopDepth;
    currentFunctionLiteral = savedLiteral;
    currentScope = savedScope;
    literalSites.pop_back();
    return FunctionType(function);
}
} // namespace Rux::SemanticDetail
