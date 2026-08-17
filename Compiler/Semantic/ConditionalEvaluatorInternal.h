#pragma once

#include "Semantic/ConditionalCompilation.h"
#include "Semantic/SemanticVersion.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux {
namespace ConditionalEvaluation {
[[nodiscard]] std::optional<std::string> ParseStringLiteral(std::string_view text);
} // namespace ConditionalEvaluation

/// The evaluator's private state, split out so the compile-time expression machinery does not appear in the header
/// every semantic file includes.
///
/// Evaluation is name-sensitive: the same expression means different things depending on which file, module, and
/// function it appears in, which is why the source context is set before each evaluation rather than passed in.
class ConditionalEvaluator::Impl {
public:
    Impl(const CompileTimeContext &inputContext, const std::vector<Module *> &modules);

    void SetSourceContext(std::string_view file, std::string_view modulePath, std::string_view function);
    void SetImports(const Module &module);
    void RegisterDeclarations(const std::vector<DeclPtr> &decls);
    void RegisterConstant(const ConstDecl &decl);

    [[nodiscard]] CompileTimeEvaluation Evaluate(const Expr &expr);
    [[nodiscard]] CompileTimeEvaluation EvaluateConstant(std::string_view name);
    [[nodiscard]] CompileTimeConditionEvaluation EvaluateCondition(const Expr *condition, SourceLocation location);
    [[nodiscard]] CompileTimeMatchEvaluation
    SelectMatchArm(const Expr &subject, const std::vector<std::vector<const Expr *>> &arms, SourceLocation location);

private:
    using EnumValue = CompileTimeEnumValue;
    using Value = CompileTimeValue;

    const CompileTimeContext &context;
    std::vector<Diagnostic> diags;
    std::string currentFile;
    std::string currentFunction;
    std::string currentModulePath;
    std::unordered_map<std::string, const Expr *> constExprs;
    std::unordered_map<std::string, std::uint32_t> constSignedIntegerWidths;
    std::unordered_map<std::string, std::uint32_t> constUnsignedIntegerWidths;
    std::unordered_map<std::string, std::vector<std::string>> enumVariants;
    std::unordered_set<std::string> constsInProgress;
    std::unordered_set<std::string> builtinEnumNames;
    std::unordered_set<std::string> programEnumNames;
    std::unordered_set<std::string> localIntrinsics;
    std::unordered_set<std::string> ruxImports;
    bool ruxGlobImport = false;
    bool reportedError = false;

    void BeginEvaluation();
    [[nodiscard]] std::vector<Diagnostic> TakeDiagnostics();
    void EmitError(SourceLocation location, std::string message);
    void EmitWarning(SourceLocation location, std::string message);
    void RegisterConstantImpl(const ConstDecl &decl);
    void CollectCompileTimeDecls(const std::vector<DeclPtr> &decls);
    void SetRuxImportsForModule(const Module &module);
    void CollectRuxImports(const std::vector<DeclPtr> &decls);
    [[nodiscard]] bool RequireRuxImport(const std::string &name, SourceLocation location);
    [[nodiscard]] std::optional<std::string> IntrinsicArgument(const IntrinsicExpr &expr, bool allowEnum = false);
    [[nodiscard]] bool TargetHasFeature(std::string_view name) const;
    [[nodiscard]] static std::optional<std::string_view> CompilerParamRoot(const Expr &expr);
    [[nodiscard]] std::optional<Value> EvalCompilerParamField(std::string_view root, std::string_view field,
                                                              SourceLocation location);
    [[nodiscard]] std::optional<ParsedSemanticVersion> EvalSemanticVersion(const Expr &expr);
    [[nodiscard]] std::optional<Value> EvalCompilerParamCall(std::string_view root, std::string_view member,
                                                             const CallExpr &call);
    [[nodiscard]] std::optional<Value> EvalConstantReference(const IdentExpr &expr);
    [[nodiscard]] std::optional<Value> Eval(const Expr &expr);
    [[nodiscard]] std::optional<bool> EnumEquals(const EnumValue &left, const EnumValue &right,
                                                 SourceLocation location);
    [[nodiscard]] std::optional<Value> EvalEnumComparison(const BinaryExpr &expr, const EnumValue &left,
                                                          const EnumValue &right);
    [[nodiscard]] std::optional<std::uint32_t> SignedIntegerWidth(const Expr &expr) const;
    [[nodiscard]] std::optional<std::uint32_t> UnsignedIntegerWidth(const Expr &expr) const;
    [[nodiscard]] static std::string JoinVariants(const std::vector<std::string> &variants);
    [[nodiscard]] std::optional<Value> EvalBinary(const BinaryExpr &expr);
    [[nodiscard]] bool EvalCondition(const Expr *condition, SourceLocation location);
    [[nodiscard]] static std::string FormatValue(const Value &value);
    [[nodiscard]] std::optional<bool> ArmMatches(const Value &subject, const Expr &pattern, SourceLocation location,
                                                 Value *evaluatedPattern = nullptr);
    [[nodiscard]] int SelectMatchArmImpl(const Expr &subject, const std::vector<std::vector<const Expr *>> &arms,
                                         SourceLocation location);
};
} // namespace Rux
