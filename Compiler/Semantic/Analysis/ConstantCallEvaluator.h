#pragma once

// Compile-time evaluation of the constants whose initializers call functions.
//
// Semantic analysis evaluates them once every body is checked, over the checked syntax tree and the facts checking
// recorded for it: the type of each expression, the callable each call selected, the case each pattern names. HIR does
// not exist yet when analysis needs the value, and the facts already say everything evaluation has to decide, so no
// name, overload or conversion is resolved a second time here. Scalars are computed with `Evaluation::TypedConstant`,
// which holds every integer and float at the target's exact width and encoding.

#include "Evaluation/TypedConstant.h"
#include "Semantic/Analysis/AnalysisContext.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::SemanticDetail {
struct EvaluatedValue;

/// Storage a local, a parameter or a temporary owns while a function runs. References name a part of one.
using ValueCell = std::shared_ptr<EvaluatedValue>;

/// A place: a cell and the element indices leading from its value to a part of it.
struct ValuePlace {
    ValueCell root;
    std::vector<std::size_t> path;
};

/// One value while a constant is computed.
struct EvaluatedValue {
    enum class Kind : std::uint8_t {
        /// Storage declared without a value and not yet assigned.
        Uninitialized,
        /// `()`, what a function without a result returns.
        Unit,
        /// A number, a bool or a character; a character is held as the unsigned integer of its width.
        Scalar,
        Struct,
        Tuple,
        Array,
        /// A case of an enum or a variant; `elements` holds its payload.
        Case,
        /// A function named as a value.
        Function,
        /// A `&T` or `&var T` naming `place`.
        Reference,
    };

    // Values are copied and held on the native stack at every level of evaluation, so what most values lack lives
    // behind shared, immutable pointers and a value stays small.
    Kind kind = Kind::Uninitialized;
    TypeRef type;
    std::shared_ptr<const Evaluation::TypedConstant> scalar;
    std::vector<EvaluatedValue> elements;
    /// A struct's field names, in declaration order, parallel to `elements`.
    std::shared_ptr<const std::vector<std::string>> fieldNames;
    const EnumDecl *caseDeclaration = nullptr;
    const EnumDecl::Variant *selectedCase = nullptr;
    const FuncDecl *function = nullptr;
    /// The type arguments of a generic function named as a value; shared, since values are copied often.
    std::shared_ptr<const std::unordered_map<std::string, TypeRef>> functionSubstitutions;
    std::shared_ptr<ValuePlace> place;

    /// The position of the struct field `name` in `elements`.
    [[nodiscard]] std::optional<std::size_t> FieldIndex(const std::string_view name) const {
        if (!fieldNames)
            return std::nullopt;
        const auto found = std::ranges::find(*fieldNames, name);
        return found == fieldNames->end() ? std::nullopt
                                          : std::optional{static_cast<std::size_t>(found - fieldNames->begin())};
    }
};

class ConstantCallEvaluator {
public:
    /// Evaluation stops with an error after this many statements, loop iterations and calls.
    static constexpr std::uint64_t StepLimit = 4'000'000;
    /// Evaluation stops with an error when calls nest deeper than this.
    static constexpr std::size_t DepthLimit = 64;
    /// Evaluation stops with an error when it has used this much native stack, which only expressions nested far
    /// deeper than any written by hand reach before the depth limit. It fits the smallest thread stack a host gives.
    static constexpr std::uintptr_t StackBudget = 384 * 1024;

    explicit ConstantCallEvaluator(AnalysisContext &analysis);

    /// Compute `constant`'s value and record it in the facts, or report why it has none.
    void Evaluate(const ConstDecl &constant);

private:
    /// How a statement or an expression finished. Anything but `Normal` unwinds to whatever handles it.
    enum class Completion : std::uint8_t {
        Normal,
        Break,
        Continue,
        Return,
        Failed,
    };

    struct Frame {
        const FuncDecl *function = nullptr;
        std::string file;
        /// Where the names the function or constant is written with resolve.
        Scope *lookup = nullptr;
        std::unordered_map<std::string, TypeRef> substitutions;
        std::vector<std::unordered_map<std::string, ValueCell>> scopes;
    };

    /// The first operation a function or one of its callees performs that no constant may use.
    struct Offense {
        std::string description;
        std::string file;
        SourceLocation location;
        /// The functions from the one examined down to the one performing the operation.
        std::vector<std::string> chain;
    };

    struct Failure {
        std::string reason;
        std::string file;
        SourceLocation location;
        std::vector<std::string> chain;
        bool forbidden = false;
        bool limit = false;
        /// Reported elsewhere: a constant this one reads failed, and that constant reports why.
        bool silent = false;
    };

    // Driving one constant (ConstantCallEvaluator.cpp).
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> ConstantValue(const ConstDecl &constant);
    [[nodiscard, gnu::noinline]] std::optional<ComputedConstant> ToCompileTimeValue(const EvaluatedValue &value) const;
    [[gnu::cold, gnu::noinline]] void Report(const ConstDecl &constant);
    [[nodiscard]] static SourceLocation SiteOf(const ConstDecl &constant);
    [[nodiscard, gnu::cold, gnu::noinline]] std::nullopt_t Fail(SourceLocation location, std::string reason);
    [[nodiscard, gnu::cold, gnu::noinline]] std::nullopt_t Unsupported(SourceLocation location, std::string_view what);
    [[nodiscard]] bool Step(SourceLocation location);
    [[nodiscard, gnu::noinline]] bool StackExhausted(SourceLocation location);
    [[nodiscard]] std::string CurrentFile() const;
    [[nodiscard]] std::vector<std::string> CurrentChain() const;
    /// The function evaluation is in, quoted, as a diagnostic names what did something.
    [[nodiscard]] std::string Actor() const;
    [[nodiscard]] std::string FunctionName(const FuncDecl &function) const;

    // What a function may not use (ConstantCallPurity.cpp).
    [[nodiscard, gnu::noinline]] const std::optional<Offense> &PurityOf(const FuncDecl &function);
    [[nodiscard]] std::optional<Offense> ScanFunction(const FuncDecl &function);
    [[nodiscard]] std::optional<Offense> ScanBlock(const Block &block, const FuncDecl &owner);
    [[nodiscard]] std::optional<Offense> ScanStatement(const Stmt &statement, const FuncDecl &owner);
    [[nodiscard]] std::optional<Offense> ScanExpression(const Expr &expression, const FuncDecl &owner);
    [[nodiscard]] std::optional<Offense> ScanCallee(const Decl *callee, const Expr &site, const FuncDecl &owner);
    [[nodiscard]] std::optional<Offense> ScanArms(const std::vector<MatchExpr::Arm> &arms, const FuncDecl &owner);
    [[nodiscard]] std::optional<Offense> ScanPattern(const Pattern &pattern, const FuncDecl &owner);
    [[nodiscard]] Offense OffenseAt(const FuncDecl &owner, SourceLocation location, std::string description) const;
    void EnsureBodyChecked(const FuncDecl &function);

    // Types and values (ConstantCallEvaluator.cpp).
    [[nodiscard]] TypeRef Substitute(const TypeRef &type) const;
    [[nodiscard]] TypeRef TypeOf(const Expr &expression) const;
    [[nodiscard]] TypeRef DeclaredType(const TypeExpr &type) const;
    [[nodiscard]] static TypeRef ValueType(const TypeRef &type);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> Coerce(EvaluatedValue value, const TypeRef &target,
                                                                      SourceLocation location);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> DefaultValue(const TypeRef &type,
                                                                            SourceLocation location);
    [[nodiscard, gnu::noinline]] std::vector<std::pair<std::string, TypeRef>> StructFields(const TypeRef &type);
    [[nodiscard]] static EvaluatedValue MakeScalar(Evaluation::TypedConstant value, TypeRef type);
    [[nodiscard]] static EvaluatedValue MakeBool(bool value);
    [[nodiscard]] static EvaluatedValue MakeUnit();
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue>
    MakeInteger(std::string_view decimal, const TypeRef &type, SourceLocation location);
    [[nodiscard]] static TypeRef ScalarModelType(const TypeRef &type);
    [[nodiscard]] std::optional<bool> Truth(const EvaluatedValue &value, SourceLocation location);
    [[nodiscard, gnu::noinline]] std::optional<bool> ValuesEqual(const EvaluatedValue &left,
                                                                 const EvaluatedValue &right, SourceLocation location);

    // Places.
    [[nodiscard]] ValueCell *LookupLocal(const std::string &name);
    void Declare(const std::string &name, EvaluatedValue value);
    [[nodiscard, gnu::noinline]] std::optional<ValuePlace> EvaluatePlace(const Expr &expression);
    [[nodiscard]] EvaluatedValue *Resolve(const ValuePlace &place);
    [[nodiscard, gnu::noinline]] std::optional<ValuePlace> Follow(ValuePlace place);

    // Expressions (ConstantCallEvaluator.cpp).
    [[nodiscard]] std::optional<EvaluatedValue> EvaluateExpression(const Expr &expression);
    [[nodiscard]] std::optional<EvaluatedValue> EvaluateAs(const Expr &expression, const TypeRef &target);
    /// The expressions `EvaluateExpression` meets least, kept out of its frame: it is on every path of a call.
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateOther(const Expr &expression);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateLiteral(const LiteralExpr &literal);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateName(const Expr &expression);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateUnary(const UnaryExpr &unary);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateBinary(const BinaryExpr &binary);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue>
    Arithmetic(TokenKind op, const EvaluatedValue &left, const EvaluatedValue &right, SourceLocation location);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateAssignment(const AssignExpr &assignment);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateIncrement(const Expr &operand, TokenKind op,
                                                                                 bool prefix, SourceLocation location);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateCast(const CastExpr &cast);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue>
    CastValue(const EvaluatedValue &value, const TypeRef &target, SourceLocation location);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateField(const FieldExpr &field);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateIndex(const IndexExpr &index);
    [[nodiscard, gnu::noinline]] std::optional<std::size_t> ElementIndex(const EvaluatedValue &array,
                                                                         const Expr &index);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateStructInit(const StructInitExpr &initializer);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateArray(const Expr &expression);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateTuple(const TupleExpr &tuple);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue>
    EvaluateCase(const EnumDecl &declaration, const EnumDecl::Variant &selected, const TypeRef &type);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateBlock(const Block &block);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateMatch(const MatchExpr &match);

    // Calls (ConstantCallEvaluator.cpp).
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> EvaluateCall(const CallExpr &call);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue>
    Invoke(const FuncDecl &function, std::unordered_map<std::string, TypeRef> substitutions, const Expr *receiver,
           const std::vector<ExprPtr> &arguments, SourceLocation site);
    [[nodiscard, gnu::noinline]] std::optional<EvaluatedValue> Argument(const Expr &argument,
                                                                        const TypeRef &parameterType);

    // Statements and patterns (ConstantCallStatements.cpp).
    [[nodiscard]] Completion ExecuteBlock(const Block &block, std::optional<EvaluatedValue> *yielded = nullptr);
    [[nodiscard]] Completion ExecuteStatement(const Stmt &statement, std::optional<EvaluatedValue> *yielded);
    /// Loops, matches and the rarer statements, kept out of `ExecuteStatement`'s frame.
    [[nodiscard, gnu::noinline]] Completion ExecuteCompound(const Stmt &statement);
    [[nodiscard, gnu::noinline]] Completion ExecuteLet(const LetStmt &let);
    [[nodiscard, gnu::noinline]] Completion ExecuteIf(const IfStmt &statement);
    [[nodiscard]] Completion ExecuteLoopBody(const Block &body, const std::string &label, bool &leave);
    [[nodiscard, gnu::noinline]] Completion ExecuteFor(const ForStmt &statement);
    [[nodiscard]] Completion Abrupt();
    [[nodiscard, gnu::noinline]] std::optional<bool> Matches(const Pattern &pattern, const EvaluatedValue &value);
    [[nodiscard, gnu::noinline]] std::optional<bool> MatchLiteral(const Token &token, const EvaluatedValue &value,
                                                                  SourceLocation location);
    void PushScope();
    void PopScope();

    AnalysisContext &context;
    std::vector<Frame> frames;
    /// The file of the constant being computed, where evaluation outside every function reports.
    std::string constantFile;
    std::optional<Failure> failure;
    /// Set while a `break`, `continue` or `return` unwinds, with the label or value it carries.
    Completion unwinding = Completion::Normal;
    std::string unwindingLabel;
    std::optional<EvaluatedValue> returned;
    std::uint64_t steps = 0;
    std::size_t callDepth = 0;
    /// Where the native stack stood when evaluation of the current constant began.
    std::uintptr_t stackBase = 0;
    /// Why each constant queued for evaluation of its own failed, when another constant read it first.
    std::unordered_map<const ConstDecl *, Failure> constantFailures;
    /// Constants already computed, and those being computed, which hold no value yet.
    std::unordered_map<const ConstDecl *, std::optional<EvaluatedValue>> constants;
    std::unordered_map<const FuncDecl *, std::optional<Offense>> purity;
    std::unordered_set<const FuncDecl *> scanning;
};
} // namespace Rux::SemanticDetail
