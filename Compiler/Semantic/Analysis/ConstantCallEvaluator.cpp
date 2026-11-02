// Compile-time evaluation of constants whose initializers call functions: values, expressions and calls.
//
// Evaluation runs once every body is checked, so it reads only what checking recorded. A failure is a value: the
// first one is kept in `failure`, every evaluation step returns nothing once it is set, and the constant reports it.

#include "Semantic/Analysis/ConstantCallEvaluator.h"

#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <utility>

namespace Rux::SemanticDetail {
using Evaluation::CheckedFailure;
using Evaluation::TypedConstant;

namespace {
std::string_view OperatorSpelling(const TokenKind op) {
    switch (op) {
    case TokenKind::Plus:
    case TokenKind::PlusAssign:
    case TokenKind::PlusPlus:
        return "+";
    case TokenKind::Minus:
    case TokenKind::MinusAssign:
    case TokenKind::MinusMinus:
        return "-";
    case TokenKind::Star:
    case TokenKind::StarAssign:
        return "*";
    case TokenKind::Slash:
    case TokenKind::SlashAssign:
        return "/";
    case TokenKind::Percent:
    case TokenKind::PercentAssign:
        return "%";
    case TokenKind::Amp:
        return "&";
    case TokenKind::Pipe:
        return "|";
    case TokenKind::Caret:
        return "^";
    case TokenKind::Tilde:
        return "~";
    case TokenKind::Bang:
        return "!";
    case TokenKind::LessLess:
    case TokenKind::LessLessAssign:
        return "<<";
    case TokenKind::GreaterGreater:
    case TokenKind::GreaterGreaterAssign:
        return ">>";
    case TokenKind::GreaterGreaterGreater:
    case TokenKind::GreaterGreaterGreaterAssign:
        return ">>>";
    case TokenKind::Equal:
        return "==";
    case TokenKind::BangEqual:
        return "!=";
    case TokenKind::Less:
        return "<";
    case TokenKind::LessEqual:
        return "<=";
    case TokenKind::Greater:
        return ">";
    case TokenKind::GreaterEqual:
        return ">=";
    default:
        return "?";
    }
}

/// The operator a compound assignment applies, or the token itself for any other.
TokenKind CompoundOperator(const TokenKind op) {
    switch (op) {
    case TokenKind::PlusAssign:
        return TokenKind::Plus;
    case TokenKind::MinusAssign:
        return TokenKind::Minus;
    case TokenKind::StarAssign:
        return TokenKind::Star;
    case TokenKind::SlashAssign:
        return TokenKind::Slash;
    case TokenKind::PercentAssign:
        return TokenKind::Percent;
    case TokenKind::AmpAssign:
        return TokenKind::Amp;
    case TokenKind::PipeAssign:
        return TokenKind::Pipe;
    case TokenKind::CaretAssign:
        return TokenKind::Caret;
    case TokenKind::LessLessAssign:
        return TokenKind::LessLess;
    case TokenKind::GreaterGreaterAssign:
        return TokenKind::GreaterGreater;
    case TokenKind::GreaterGreaterGreaterAssign:
        return TokenKind::GreaterGreaterGreater;
    default:
        return op;
    }
}

bool IsComparison(const TokenKind op) {
    return op == TokenKind::Equal || op == TokenKind::BangEqual || op == TokenKind::Less ||
           op == TokenKind::LessEqual || op == TokenKind::Greater || op == TokenKind::GreaterEqual;
}

bool IsShift(const TokenKind op) {
    return op == TokenKind::LessLess || op == TokenKind::GreaterGreater || op == TokenKind::GreaterGreaterGreater;
}

/// The chain of calls, as a diagnostic names it: every name for a short chain, the ends of a long one.
std::string ChainText(const std::vector<std::string> &chain) {
    const auto quoted = [](const std::string &name) { return std::format("'{}'", name); };
    std::string text;
    const auto append = [&](const std::string &part) { text += (text.empty() ? "" : " -> ") + part; };
    if (chain.size() <= 6) {
        for (const std::string &name : chain)
            append(quoted(name));
        return text;
    }
    for (std::size_t index = 0; index < 3; ++index)
        append(quoted(chain[index]));
    append("...");
    for (std::size_t index = chain.size() - 2; index < chain.size(); ++index)
        append(quoted(chain[index]));
    return std::format("{}, {} calls deep", text, chain.size());
}

/// Whether reading `expression` can name existing storage, so a part of it is read in place rather than copied whole.
bool NamesStorage(const Expr &expression) {
    return AstCast<const IdentExpr>(&expression) || AstCast<const SelfExpr>(&expression) ||
           AstCast<const FieldExpr>(&expression) || AstCast<const IndexExpr>(&expression);
}
} // namespace

void AnalysisContext::EvaluateConstantCalls() {
    // A body that failed to check has no facts to run, and the build stops at its errors anyway.
    if (constantsToEvaluate.empty() || std::ranges::any_of(diags, [](const SemanticDiagnostic &diagnostic) {
            return diagnostic.severity == Diagnostic::Severity::Error;
        }))
        return;
    const std::string savedFile = currentFile;
    const std::string savedPackage = currentPackage;
    Scope *const savedScope = currentScope;
    ConstantCallEvaluator evaluator(*this);
    // Checking a reused module's bodies for one constant can queue more of them, which are evaluated in turn.
    for (std::size_t index = 0; index < constantsToEvaluate.size(); ++index)
        evaluator.Evaluate(*constantsToEvaluate[index]);
    currentScope = savedScope;
    currentPackage = savedPackage;
    currentFile = savedFile;
}

ConstantCallEvaluator::ConstantCallEvaluator(AnalysisContext &analysis)
    : context(analysis) {
}

void ConstantCallEvaluator::Evaluate(const ConstDecl &constant) {
    if (context.outputFacts.evaluatedConstants.contains(&constant))
        return;
    const auto info = context.declarationInfos.find(&constant);
    constantFile = info != context.declarationInfos.end() ? info->second.sourceName : context.currentFile;
    failure.reset();
    frames.clear();
    unwinding = Completion::Normal;
    returned.reset();
    steps = 0;
    // Evaluation recurses on the native stack, which grows down on every target; a local marks where it starts.
    const char base = 0;
    stackBase = reinterpret_cast<std::uintptr_t>(&base);
    callDepth = 0;
    if (const auto failed = constantFailures.find(&constant); failed != constantFailures.end()) {
        failure = failed->second;
        Report(constant);
        return;
    }
    const std::optional<EvaluatedValue> value = ConstantValue(constant);
    if (!value) {
        if (failure && !failure->silent)
            Report(constant);
        return;
    }
    std::optional<ComputedConstant> compileTime = ToCompileTimeValue(*value);
    if (!compileTime) {
        failure = Failure{std::format("its value of type '{}' is not a compile-time value", value->type.ToString()),
                          constantFile,
                          SiteOf(constant),
                          {}};
        Report(constant);
        return;
    }
    context.outputFacts.evaluatedConstants.emplace(&constant, std::move(*compileTime));
}

std::optional<EvaluatedValue> ConstantCallEvaluator::ConstantValue(const ConstDecl &constant) {
    if (const auto known = constants.find(&constant); known != constants.end()) {
        if (known->second)
            return *known->second;
        // Analysis reports a constant whose initializer reads itself.
        failure = Failure{"its initializer depends on itself", CurrentFile(), constant.location, {}};
        failure->silent = true;
        return std::nullopt;
    }
    if (constantFailures.contains(&constant)) {
        failure = Failure{"a constant it reads cannot be computed", CurrentFile(), constant.location, {}};
        failure->silent = true;
        return std::nullopt;
    }
    TypeRef type = TypeRef::MakeUnknown();
    if (const auto checked = context.checkedConstantTypes.find(&constant);
        checked != context.checkedConstantTypes.end())
        type = checked->second;
    if (const auto literal = context.evaluatedAssociatedConstants.find(&constant);
        literal != context.evaluatedAssociatedConstants.end()) {
        const auto parsed = Evaluation::ParseConstant(literal->second.literal, literal->second.type);
        if (!parsed)
            return Unsupported(constant.location, std::format("the value of '{}'", constant.name));
        return MakeScalar(*parsed, literal->second.type);
    }
    if (!constant.value)
        return Unsupported(constant.location, std::format("the compiler-supplied constant '{}'", constant.name));
    if (type.IsUnknown())
        return Unsupported(constant.location, std::format("the constant '{}', which was never checked", constant.name));

    constants.emplace(&constant, std::nullopt);
    Frame frame;
    const auto info = context.declarationInfos.find(&constant);
    frame.file = info != context.declarationInfos.end() ? info->second.sourceName : CurrentFile();
    frame.lookup = info != context.declarationInfos.end() ? info->second.scope : nullptr;
    frame.scopes.emplace_back();
    frames.push_back(std::move(frame));
    std::optional<EvaluatedValue> value = EvaluateAs(*constant.value, type);
    frames.pop_back();
    if (!value) {
        constants.erase(&constant);
        // A constant queued for evaluation of its own reports its failure itself; one that reads it stays quiet.
        if (failure && !failure->silent && context.queuedConstantEvaluations.contains(&constant) && !frames.empty()) {
            constantFailures.emplace(&constant, *failure);
            failure->silent = true;
        }
        return std::nullopt;
    }
    constants[&constant] = *value;
    return value;
}

std::optional<ComputedConstant> ConstantCallEvaluator::ToCompileTimeValue(const EvaluatedValue &value) const {
    ComputedConstant result;
    result.type = value.type;
    const auto all = [&](ComputedConstant::Kind kind) -> std::optional<ComputedConstant> {
        result.kind = kind;
        for (const EvaluatedValue &element : value.elements) {
            auto converted = ToCompileTimeValue(element);
            if (!converted)
                return std::nullopt;
            result.elements.push_back(std::move(*converted));
        }
        return result;
    };
    switch (value.kind) {
    case EvaluatedValue::Kind::Scalar:
        result.kind = ComputedConstant::Kind::Scalar;
        result.literal = value.scalar->ToLiteral();
        return result;
    case EvaluatedValue::Kind::Struct:
        result.fieldNames = value.fieldNames ? *value.fieldNames : std::vector<std::string>{};
        return all(ComputedConstant::Kind::Struct);
    case EvaluatedValue::Kind::Tuple:
        return all(ComputedConstant::Kind::Tuple);
    case EvaluatedValue::Kind::Array:
        return all(ComputedConstant::Kind::Array);
    case EvaluatedValue::Kind::Case:
        result.caseDeclaration = value.caseDeclaration;
        result.selectedCase = value.selectedCase;
        return all(ComputedConstant::Kind::Case);
    default:
        return std::nullopt;
    }
}

SourceLocation ConstantCallEvaluator::SiteOf(const ConstDecl &constant) {
    if (!constant.value)
        return constant.location;
    if (const auto *call = AstCast<const CallExpr>(constant.value.get()))
        return CallSiteLocation(*call);
    return constant.value->location;
}

void ConstantCallEvaluator::Report(const ConstDecl &constant) {
    std::string message =
        std::format("constant '{}' cannot be computed at compile time: {}", constant.name, failure->reason);
    if (!failure->chain.empty())
        message += std::format(" (in {})", ChainText(failure->chain));
    std::vector<std::string> notes;
    if (!failure->file.empty() && !(failure->file == constantFile && failure->location == SiteOf(constant)))
        notes.push_back(std::format("evaluation stopped at '{}':{}:{}", failure->file, failure->location.line,
                                    failure->location.column));
    std::optional<std::string> help;
    if (failure->forbidden) {
        help = "a constant may call only functions that use no raw pointers, 'unsafe', extern functions, allocators, "
               "thread-local or module variables, and that call only such functions";
    }
    else if (failure->limit) {
        help = std::format("evaluating a constant may take at most {} steps and nest calls at most {} deep; compute "
                           "the value at run time with 'let' instead",
                           StepLimit, DepthLimit);
    }
    const std::string savedFile = context.currentFile;
    context.currentFile = constantFile;
    context.EmitError(SiteOf(constant), std::move(message), std::move(notes), std::move(help));
    context.currentFile = savedFile;
}

std::nullopt_t ConstantCallEvaluator::Fail(const SourceLocation location, std::string reason) {
    if (!failure)
        failure = Failure{std::move(reason), CurrentFile(), location, CurrentChain()};
    return std::nullopt;
}

std::nullopt_t ConstantCallEvaluator::Unsupported(const SourceLocation location, const std::string_view what) {
    return Fail(location, std::format("{} is not supported in compile-time evaluation", what));
}

bool ConstantCallEvaluator::Step(const SourceLocation location) {
    if (failure)
        return false;
    if (++steps <= StepLimit)
        return true;
    static_cast<void>(Fail(location, std::format("evaluation took more than {} steps", StepLimit)));
    failure->limit = true;
    return false;
}

bool ConstantCallEvaluator::StackExhausted(const SourceLocation location) {
    // The call depth bounds how deep evaluation recurses; this bounds the native stack that takes, whatever a body's
    // expressions nest to, so a constant never overflows the stack of the thread analyzing it.
    const char here = 0;
    if (stackBase - reinterpret_cast<std::uintptr_t>(&here) <= StackBudget)
        return false;
    static_cast<void>(Fail(location, "calls and expressions nest too deeply"));
    failure->limit = true;
    return true;
}

std::string ConstantCallEvaluator::CurrentFile() const {
    return frames.empty() ? constantFile : frames.back().file;
}

std::string ConstantCallEvaluator::Actor() const {
    for (auto frame = frames.rbegin(); frame != frames.rend(); ++frame)
        if (frame->function)
            return std::format("'{}'", FunctionName(*frame->function));
    return "the initializer";
}

std::vector<std::string> ConstantCallEvaluator::CurrentChain() const {
    std::vector<std::string> chain;
    for (const Frame &frame : frames)
        if (frame.function)
            chain.push_back(FunctionName(*frame.function));
    return chain;
}

std::string ConstantCallEvaluator::FunctionName(const FuncDecl &function) const {
    if (const auto owner = context.methodImpls.find(&function); owner != context.methodImpls.end() && owner->second)
        return std::format("{}.{}", owner->second->typeName, function.name);
    return function.name;
}

// Types and values.

TypeRef ConstantCallEvaluator::Substitute(const TypeRef &type) const {
    if (frames.empty() || frames.back().substitutions.empty())
        return type;
    return context.SubstituteTypeParameters(type, frames.back().substitutions);
}

TypeRef ConstantCallEvaluator::TypeOf(const Expr &expression) const {
    const auto found = context.expressionTypes.find(&expression);
    return found == context.expressionTypes.end() ? TypeRef::MakeUnknown() : Substitute(found->second);
}

TypeRef ConstantCallEvaluator::DeclaredType(const TypeExpr &type) const {
    const auto found = context.typeNodeTypes.find(&type);
    return found == context.typeNodeTypes.end() ? TypeRef::MakeUnknown() : Substitute(found->second);
}

TypeRef ConstantCallEvaluator::ValueType(const TypeRef &type) {
    return type->kind == TypeRef::Kind::Reference && !type->inner.empty() ? type->inner.front() : type;
}

/// The type a scalar is computed at: a character is the unsigned integer of its width, which holds its code point.
TypeRef ConstantCallEvaluator::ScalarModelType(const TypeRef &type) {
    if (!type.IsChar())
        return type;
    const PrimitiveInfo *primitive = FindPrimitive(type->kind);
    for (const PrimitiveInfo &candidate : PrimitiveCatalog())
        if (primitive && candidate.category == PrimitiveCategory::UnsignedInt && candidate.bits == primitive->bits)
            return TypeRef::MakePrimitive(candidate.kind);
    return type;
}

EvaluatedValue ConstantCallEvaluator::MakeScalar(TypedConstant value, TypeRef type) {
    EvaluatedValue result;
    result.kind = EvaluatedValue::Kind::Scalar;
    result.type = type;
    result.scalar = std::make_shared<const TypedConstant>(value);
    return result;
}

EvaluatedValue ConstantCallEvaluator::MakeBool(const bool value) {
    return MakeScalar(*Evaluation::ParseConstant(value ? "true" : "false", TypeRef::MakeBool()), TypeRef::MakeBool());
}

EvaluatedValue ConstantCallEvaluator::MakeUnit() {
    EvaluatedValue result;
    result.kind = EvaluatedValue::Kind::Unit;
    result.type = TypeRef::MakeUnit();
    return result;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::MakeInteger(const std::string_view decimal, const TypeRef &type,
                                                                 const SourceLocation location) {
    const auto parsed = Evaluation::ParseConstant(decimal, ScalarModelType(type));
    if (!parsed)
        return Fail(location, std::format("{} does not fit '{}'", decimal, type.ToString()));
    return MakeScalar(*parsed, type);
}

std::optional<bool> ConstantCallEvaluator::Truth(const EvaluatedValue &value, const SourceLocation location) {
    if (value.kind == EvaluatedValue::Kind::Scalar)
        if (const auto truth = value.scalar->BooleanValue())
            return truth;
    return Unsupported(location, std::format("a condition of type '{}'", value.type.ToString()));
}

std::optional<EvaluatedValue> ConstantCallEvaluator::Coerce(EvaluatedValue value, const TypeRef &target,
                                                            const SourceLocation location) {
    if (target.IsUnknown() || value.type == target)
        return value;
    if (target.IsSum() || target.IsOptional() || target.IsFallible() || target.IsSlice() ||
        target->kind == TypeRef::Kind::Opaque)
        return Unsupported(location, std::format("a value converted to '{}'", target.ToString()));
    switch (value.kind) {
    case EvaluatedValue::Kind::Scalar: {
        if (!(target.IsNumeric() || target.IsBool() || target.IsChar()))
            return Unsupported(location, std::format("a value converted to '{}'", target.ToString()));
        const auto converted = Evaluation::CastConstant(*value.scalar, ScalarModelType(target));
        if (!converted)
            return Unsupported(location, std::format("a value converted to '{}'", target.ToString()));
        return MakeScalar(*converted, target);
    }
    case EvaluatedValue::Kind::Array:
    case EvaluatedValue::Kind::Tuple: {
        const bool array = value.kind == EvaluatedValue::Kind::Array;
        if ((array && target->kind != TypeRef::Kind::Array) || (!array && target->kind != TypeRef::Kind::Tuple) ||
            (!array && target->inner.size() != value.elements.size()) || target->inner.empty())
            return Unsupported(location, std::format("a value converted to '{}'", target.ToString()));
        for (std::size_t index = 0; index < value.elements.size(); ++index) {
            auto element = Coerce(std::move(value.elements[index]),
                                  array ? target->inner.front() : target->inner[index], location);
            if (!element)
                return std::nullopt;
            value.elements[index] = std::move(*element);
        }
        value.type = target;
        return value;
    }
    default:
        // Structs, cases and functions keep their value; the destination only names the same type another way.
        value.type = target;
        return value;
    }
}

std::vector<std::pair<std::string, TypeRef>> ConstantCallEvaluator::StructFields(const TypeRef &type) {
    std::vector<std::pair<std::string, TypeRef>> fields;
    const auto structure = context.structDecls.find(context.NamedBaseTypeName(type));
    if (structure == context.structDecls.end())
        return fields;
    // A field's type is spelled where the struct is declared, so it is resolved in that scope.
    Scope *const savedScope = context.currentScope;
    const std::string savedFile = context.currentFile;
    if (const auto info = context.declarationInfos.find(structure->second);
        info != context.declarationInfos.end() && info->second.scope) {
        context.currentScope = info->second.scope;
        context.currentFile = info->second.sourceName;
    }
    for (const StructDecl::Field &field : structure->second->fields)
        fields.emplace_back(field.name, context.StructFieldType(type, field.name));
    context.currentFile = savedFile;
    context.currentScope = savedScope;
    return fields;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::DefaultValue(const TypeRef &type, const SourceLocation location) {
    EvaluatedValue value;
    value.type = type;
    if (type.IsInteger() || type.IsFloat() || type.IsChar())
        return MakeInteger("0", type, location);
    if (type.IsBool())
        return MakeScalar(*Evaluation::ParseConstant("false", type), type);
    if (type->kind == TypeRef::Kind::Array && type->arrayLength && !type->inner.empty()) {
        if (*type->arrayLength > StepLimit)
            return Unsupported(location, std::format("an array of {} elements", *type->arrayLength));
        auto element = DefaultValue(type->inner.front(), location);
        if (!element)
            return std::nullopt;
        value.kind = EvaluatedValue::Kind::Array;
        value.elements.assign(static_cast<std::size_t>(*type->arrayLength), *element);
        return value;
    }
    if (type->kind == TypeRef::Kind::Tuple) {
        value.kind = EvaluatedValue::Kind::Tuple;
        for (const TypeRef &element : type->inner) {
            auto part = DefaultValue(element, location);
            if (!part)
                return std::nullopt;
            value.elements.push_back(std::move(*part));
        }
        return value;
    }
    if (const auto fields = StructFields(type); !fields.empty()) {
        value.kind = EvaluatedValue::Kind::Struct;
        std::vector<std::string> names;
        for (const auto &[name, fieldType] : fields) {
            auto part = DefaultValue(fieldType, location);
            if (!part)
                return std::nullopt;
            names.push_back(name);
            value.elements.push_back(std::move(*part));
        }
        value.fieldNames = std::make_shared<const std::vector<std::string>>(std::move(names));
        return value;
    }
    // Anything else waits for its first assignment, which semantic analysis makes before any read.
    return value;
}

std::optional<bool> ConstantCallEvaluator::ValuesEqual(const EvaluatedValue &left, const EvaluatedValue &right,
                                                       const SourceLocation location) {
    if (left.kind != right.kind)
        return Unsupported(location,
                           std::format("comparing '{}' with '{}'", left.type.ToString(), right.type.ToString()));
    switch (left.kind) {
    case EvaluatedValue::Kind::Scalar: {
        const auto equal = Arithmetic(TokenKind::Equal, left, right, location);
        return equal ? Truth(*equal, location) : std::nullopt;
    }
    case EvaluatedValue::Kind::Case:
        if (left.selectedCase != right.selectedCase)
            return false;
        [[fallthrough]];
    case EvaluatedValue::Kind::Struct:
    case EvaluatedValue::Kind::Tuple:
    case EvaluatedValue::Kind::Array:
        if (left.elements.size() != right.elements.size())
            return false;
        for (std::size_t index = 0; index < left.elements.size(); ++index) {
            const auto equal = ValuesEqual(left.elements[index], right.elements[index], location);
            if (!equal || !*equal)
                return equal;
        }
        return true;
    case EvaluatedValue::Kind::Function:
        return left.function == right.function;
    case EvaluatedValue::Kind::Unit:
        return true;
    default:
        return Unsupported(location, std::format("comparing values of type '{}'", left.type.ToString()));
    }
}

// Places.

ValueCell *ConstantCallEvaluator::LookupLocal(const std::string &name) {
    if (frames.empty())
        return nullptr;
    auto &scopes = frames.back().scopes;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        if (const auto found = scope->find(name); found != scope->end())
            return &found->second;
    return nullptr;
}

void ConstantCallEvaluator::Declare(const std::string &name, EvaluatedValue value) {
    frames.back().scopes.back().insert_or_assign(name, std::make_shared<EvaluatedValue>(std::move(value)));
}

EvaluatedValue *ConstantCallEvaluator::Resolve(const ValuePlace &place) {
    EvaluatedValue *value = place.root.get();
    for (const std::size_t index : place.path) {
        if (value == nullptr || index >= value->elements.size())
            return nullptr;
        value = &value->elements[index];
    }
    return value;
}

std::optional<ValuePlace> ConstantCallEvaluator::Follow(ValuePlace place) {
    // A reference is a place; reading or writing through one reaches what it names, however many it passes.
    for (int hops = 0; hops < 64; ++hops) {
        const EvaluatedValue *value = Resolve(place);
        if (value == nullptr)
            return Fail(SourceLocation{}, "a reference names storage that no longer exists");
        if (value->kind != EvaluatedValue::Kind::Reference)
            return place;
        place = *value->place;
    }
    return Fail(SourceLocation{}, "a reference names itself");
}

std::optional<ValuePlace> ConstantCallEvaluator::EvaluatePlace(const Expr &expression) {
    if (failure)
        return std::nullopt;
    if (const auto *move = AstCast<const MoveExpr>(&expression))
        return EvaluatePlace(*move->operand);
    const IdentExpr *identifier = AstCast<const IdentExpr>(&expression);
    if (identifier || AstCast<const SelfExpr>(&expression)) {
        if (!context.constantReferences.contains(&expression) && !context.globalReferences.contains(&expression))
            if (ValueCell *cell = LookupLocal(identifier ? identifier->name : "self"))
                return Follow(ValuePlace{*cell, {}});
    }
    else if (const auto *field = AstCast<const FieldExpr>(&expression); field && NamesStorage(*field->object)) {
        auto object = EvaluatePlace(*field->object);
        if (!object)
            return std::nullopt;
        const EvaluatedValue *value = Resolve(*object);
        if (value && value->kind == EvaluatedValue::Kind::Struct) {
            if (const auto position = value->FieldIndex(field->field)) {
                object->path.push_back(*position);
                return Follow(std::move(*object));
            }
        }
        if (value && value->kind == EvaluatedValue::Kind::Tuple) {
            std::size_t index = 0;
            const auto [end, error] =
                std::from_chars(field->field.data(), field->field.data() + field->field.size(), index);
            if (error == std::errc{} && end == field->field.data() + field->field.size() &&
                index < value->elements.size()) {
                object->path.push_back(index);
                return Follow(std::move(*object));
            }
        }
        return Unsupported(field->location, std::format("the member '{}' as a place", field->field));
    }
    else if (const auto *index = AstCast<const IndexExpr>(&expression);
             index && NamesStorage(*index->object) && !context.indexOperators.contains(index)) {
        auto object = EvaluatePlace(*index->object);
        if (!object)
            return std::nullopt;
        const EvaluatedValue *array = Resolve(*object);
        if (!array || array->kind != EvaluatedValue::Kind::Array)
            return Unsupported(index->location, "indexing this value");
        const auto element = ElementIndex(*array, *index->index);
        if (!element)
            return std::nullopt;
        object->path.push_back(*element);
        return Follow(std::move(*object));
    }
    else if (const auto *unary = AstCast<const UnaryExpr>(&expression); unary && unary->op == TokenKind::Star) {
        static_cast<void>(Fail(unary->location, std::format("{} reads through a raw pointer", Actor())));
        failure->forbidden = true;
        return std::nullopt;
    }
    // Anything else is a value with no storage of its own; a temporary holds it for as long as it is used.
    auto value = EvaluateExpression(expression);
    if (!value)
        return std::nullopt;
    return ValuePlace{std::make_shared<EvaluatedValue>(std::move(*value)), {}};
}

// Expressions.

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateAs(const Expr &expression, const TypeRef &target) {
    auto value = EvaluateExpression(expression);
    if (!value)
        return std::nullopt;
    return Coerce(std::move(*value), ValueType(target), expression.location);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateExpression(const Expr &expression) {
    if (failure || unwinding != Completion::Normal)
        return std::nullopt;
    if (const auto *literal = AstCast<const LiteralExpr>(&expression))
        return EvaluateLiteral(*literal);
    if (AstCast<const IdentExpr>(&expression) || AstCast<const PathExpr>(&expression) ||
        AstCast<const SelfExpr>(&expression))
        return EvaluateName(expression);
    if (const auto *move = AstCast<const MoveExpr>(&expression))
        return EvaluateExpression(*move->operand);
    if (const auto *unary = AstCast<const UnaryExpr>(&expression))
        return EvaluateUnary(*unary);
    if (const auto *postfix = AstCast<const PostfixExpr>(&expression))
        return EvaluateIncrement(*postfix->operand, postfix->op, false, postfix->location);
    if (const auto *binary = AstCast<const BinaryExpr>(&expression))
        return EvaluateBinary(*binary);
    if (const auto *assignment = AstCast<const AssignExpr>(&expression))
        return EvaluateAssignment(*assignment);
    if (const auto *call = AstCast<const CallExpr>(&expression))
        return EvaluateCall(*call);
    if (const auto *index = AstCast<const IndexExpr>(&expression))
        return EvaluateIndex(*index);
    if (const auto *field = AstCast<const FieldExpr>(&expression))
        return EvaluateField(*field);
    if (const auto *initializer = AstCast<const StructInitExpr>(&expression))
        return EvaluateStructInit(*initializer);
    if (AstCast<const ArrayExpr>(&expression) || AstCast<const ArrayRepeatExpr>(&expression))
        return EvaluateArray(expression);
    if (const auto *tuple = AstCast<const TupleExpr>(&expression))
        return EvaluateTuple(*tuple);
    if (const auto *cast = AstCast<const CastExpr>(&expression))
        return EvaluateCast(*cast);
    if (const auto *block = AstCast<const BlockExpr>(&expression); block && !block->isUnsafe)
        return EvaluateBlock(*block->block);
    if (const auto *match = AstCast<const MatchExpr>(&expression))
        return EvaluateMatch(*match);
    return EvaluateOther(expression);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateOther(const Expr &expression) {
    if (const auto *test = AstCast<const IsExpr>(&expression)) {
        if (!test->pattern)
            return Unsupported(test->location, "an 'is' test of a type");
        auto subject = EvaluateExpression(*test->operand);
        if (!subject)
            return std::nullopt;
        const auto matched = Matches(*test->pattern, *subject);
        return matched ? std::optional{MakeBool(*matched)} : std::nullopt;
    }
    if (const auto *block = AstCast<const BlockExpr>(&expression)) {
        static_cast<void>(Fail(block->location, std::format("{} uses an 'unsafe' block", Actor())));
        failure->forbidden = true;
        return std::nullopt;
    }
    if (const auto *query = AstCast<const TypeQueryExpr>(&expression)) {
        const auto value = context.typeQueryValues.find(query);
        if (value == context.typeQueryValues.end())
            return Unsupported(query->location, "the layout of a type parameter");
        return MakeInteger(std::to_string(value->second), TypeRef::MakeUInt64(), query->location);
    }
    if (AstCast<const EnumShorthandExpr>(&expression) || AstCast<const NativeConstructExpr>(&expression) ||
        AstCast<const NoneExpr>(&expression)) {
        const auto contextual = context.contextualCases.find(&expression);
        if (contextual == context.contextualCases.end())
            return Unsupported(expression.location, "a native optional or fallible value");
        auto value = EvaluateCase(*contextual->second.declaration, *contextual->second.selectedCase,
                                  ValueType(TypeOf(expression)));
        if (const auto *construct = AstCast<const NativeConstructExpr>(&expression); construct && value) {
            auto payload = EvaluateExpression(*construct->operand);
            if (!payload)
                return std::nullopt;
            value->elements.push_back(std::move(*payload));
        }
        return value;
    }
    if (const auto *diverge = AstCast<const DivergeExpr>(&expression)) {
        switch (diverge->kind) {
        case DivergeExpr::Kind::Return: {
            std::optional<EvaluatedValue> value = diverge->value ? EvaluateExpression(*diverge->value) : MakeUnit();
            if (!value)
                return std::nullopt;
            returned = std::move(value);
            unwinding = Completion::Return;
            return std::nullopt;
        }
        case DivergeExpr::Kind::Break:
        case DivergeExpr::Kind::Continue:
            unwinding = diverge->kind == DivergeExpr::Kind::Break ? Completion::Break : Completion::Continue;
            unwindingLabel = diverge->label;
            return std::nullopt;
        case DivergeExpr::Kind::Fail:
            return Fail(diverge->location,
                        std::format("{} throws an error of type '{}'", Actor(),
                                    diverge->value ? ValueType(TypeOf(*diverge->value)).ToString() : "()"));
        }
    }
    if (AstCast<const IntrinsicExpr>(&expression))
        return Unsupported(expression.location, "a compiler-supplied value");
    // A call a `try` marks runs as any other; an error it throws is an error at the constant, whatever would handle it
    // at run time.
    if (const auto *marked = AstCast<const TryMarkExpr>(&expression);
        marked && marked->form != TryMarkExpr::Form::Optional)
        return EvaluateExpression(*marked->operand);
    return Unsupported(expression.location, "this expression");
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateLiteral(const LiteralExpr &literal) {
    const TypeRef type = ValueType(TypeOf(literal));
    const Token &token = literal.token;
    switch (token.kind) {
    case TokenKind::BoolLiteral: {
        const auto parsed = Evaluation::ParseConstant(token.text, type.IsBool() ? type : TypeRef::MakeBool());
        if (!parsed)
            return Unsupported(literal.location, "this literal");
        return MakeScalar(*parsed, type.IsBool() ? type : TypeRef::MakeBool());
    }
    case TokenKind::IntLiteral:
    case TokenKind::FloatLiteral: {
        std::string_view text = token.text;
        text.remove_suffix(NumericLiteralSuffixOf(text).size());
        const auto parsed = Evaluation::ParseConstant(text, ScalarModelType(type));
        if (!parsed)
            return Unsupported(literal.location, std::format("the literal '{}' as '{}'", token.text, type.ToString()));
        return MakeScalar(*parsed, type);
    }
    case TokenKind::CharLiteral: {
        const auto codePoint = Lexer::DecodeCharLiteralCodePoint(token.text);
        if (!codePoint)
            return Unsupported(literal.location, "this character literal");
        return MakeInteger(std::to_string(*codePoint), type, literal.location);
    }
    case TokenKind::NullKeyword:
        static_cast<void>(Fail(literal.location, std::format("{} uses a raw pointer", Actor())));
        failure->forbidden = true;
        return std::nullopt;
    default:
        return Unsupported(literal.location, "a string");
    }
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateName(const Expr &expression) {
    if (const auto constant = context.constantReferences.find(&expression);
        constant != context.constantReferences.end()) {
        auto value = ConstantValue(*constant->second);
        return value ? Coerce(std::move(*value), ValueType(TypeOf(expression)), expression.location) : std::nullopt;
    }
    if (const auto constant = context.associatedConstants.find(&expression);
        constant != context.associatedConstants.end()) {
        auto value = ConstantValue(*constant->second);
        return value ? Coerce(std::move(*value), ValueType(TypeOf(expression)), expression.location) : std::nullopt;
    }
    if (const auto global = context.globalReferences.find(&expression); global != context.globalReferences.end()) {
        const auto *variable = AstCast<const GlobalDecl>(global->second);
        static_cast<void>(Fail(expression.location,
                               variable
                                   ? std::format("{} reads {} variable '{}'", Actor(),
                                                 variable->isThreadLocal ? "thread-local" : "module", variable->name)
                                   : std::format("{} reads an extern variable", Actor())));
        failure->forbidden = true;
        return std::nullopt;
    }
    const auto *identifier = AstCast<const IdentExpr>(&expression);
    if (identifier || AstCast<const SelfExpr>(&expression)) {
        if (ValueCell *cell = LookupLocal(identifier ? identifier->name : "self")) {
            const auto place = Follow(ValuePlace{*cell, {}});
            if (!place)
                return std::nullopt;
            return *Resolve(*place);
        }
    }
    const TypeRef type = ValueType(TypeOf(expression));
    // A function named as a value.
    if (const auto reference = context.functionReferenceBindings.find(&expression);
        reference != context.functionReferenceBindings.end()) {
        if (const auto *function = AstCast<const FuncDecl>(reference->second.selectedDeclaration)) {
            EvaluatedValue value;
            value.kind = EvaluatedValue::Kind::Function;
            value.type = type;
            value.function = function;
            if (!reference->second.substitutions.empty()) {
                std::unordered_map<std::string, TypeRef> substitutions;
                for (const auto &[name, argument] : reference->second.substitutions)
                    substitutions.emplace(name, Substitute(argument));
                value.functionSubstitutions =
                    std::make_shared<const std::unordered_map<std::string, TypeRef>>(std::move(substitutions));
            }
            return value;
        }
    }
    if (identifier && type->kind == TypeRef::Kind::Func && !frames.empty() && frames.back().lookup) {
        const Symbol *symbol = frames.back().lookup->Lookup(identifier->name);
        if (symbol && symbol->kind == Symbol::Kind::Func && symbol->funcOverloads.size() == 1) {
            EvaluatedValue value;
            value.kind = EvaluatedValue::Kind::Function;
            value.type = type;
            value.function = symbol->funcOverloads.front();
            return value;
        }
    }
    // A unit case named by its type, `Color::Red`.
    if (const auto *path = AstCast<const PathExpr>(&expression); path && path->segments.size() >= 2) {
        if (const auto resolved = context.LookupCase(context.NamedBaseTypeName(type), path->segments.back());
            resolved && resolved->declaration && resolved->selectedCase)
            return EvaluateCase(*resolved->declaration, *resolved->selectedCase, type);
    }
    return Unsupported(expression.location, "this name");
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateCase(const EnumDecl &declaration,
                                                                  const EnumDecl::Variant &selected,
                                                                  const TypeRef &type) {
    EvaluatedValue value;
    value.kind = EvaluatedValue::Kind::Case;
    value.type = type;
    value.caseDeclaration = &declaration;
    value.selectedCase = &selected;
    return value;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateUnary(const UnaryExpr &unary) {
    switch (unary.op) {
    case TokenKind::PlusPlus:
    case TokenKind::MinusMinus:
        return EvaluateIncrement(*unary.operand, unary.op, true, unary.location);
    case TokenKind::At:
    case TokenKind::Star:
        static_cast<void>(Fail(unary.location, std::format("{} {}", Actor(),
                                                           unary.op == TokenKind::At ? "takes an address with '@'"
                                                                                     : "reads through a raw pointer")));
        failure->forbidden = true;
        return std::nullopt;
    default:
        break;
    }
    const TypeRef type = ValueType(TypeOf(unary));
    // A minus and the literal under it are one written value: `-128i8` is in range where `128i8` is not.
    if (const auto *literal = AstCast<const LiteralExpr>(unary.operand.get());
        literal && unary.op == TokenKind::Minus && literal->token.kind == TokenKind::IntLiteral && type.IsInteger()) {
        std::string_view text = literal->token.text;
        text.remove_suffix(NumericLiteralSuffixOf(text).size());
        if (const auto parsed = Evaluation::ParseConstant(std::format("-{}", text), type))
            return MakeScalar(*parsed, type);
    }
    auto operand = EvaluateExpression(*unary.operand);
    if (!operand)
        return std::nullopt;
    if (operand->kind != EvaluatedValue::Kind::Scalar)
        return Unsupported(unary.location,
                           std::format("operator '{}' on '{}'", OperatorSpelling(unary.op), operand->type.ToString()));
    const auto result = Evaluation::EvaluateUnaryChecked(unary.op, *operand->scalar);
    if (!result.value) {
        if (result.failure == CheckedFailure::Overflow)
            return Fail(unary.location,
                        std::format("'{}' overflows '{}'", OperatorSpelling(unary.op), operand->type.ToString()));
        return Unsupported(unary.location,
                           std::format("operator '{}' on '{}'", OperatorSpelling(unary.op), operand->type.ToString()));
    }
    return MakeScalar(*result.value, operand->type);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateBinary(const BinaryExpr &binary) {
    if (binary.op == TokenKind::AmpAmp || binary.op == TokenKind::PipePipe) {
        const auto left = EvaluateExpression(*binary.left);
        const auto truth = left ? Truth(*left, binary.left->location) : std::nullopt;
        if (!truth)
            return std::nullopt;
        if (*truth == (binary.op == TokenKind::PipePipe))
            return MakeBool(*truth);
        const auto right = EvaluateExpression(*binary.right);
        const auto rightTruth = right ? Truth(*right, binary.right->location) : std::nullopt;
        return rightTruth ? std::optional{MakeBool(*rightTruth)} : std::nullopt;
    }
    if (binary.op == TokenKind::QuestionQuestion)
        return Unsupported(binary.location, "the coalescing operator");
    auto left = EvaluateExpression(*binary.left);
    if (!left)
        return std::nullopt;
    auto right = EvaluateExpression(*binary.right);
    if (!right)
        return std::nullopt;
    if ((binary.op == TokenKind::Equal || binary.op == TokenKind::BangEqual) &&
        (left->kind != EvaluatedValue::Kind::Scalar || right->kind != EvaluatedValue::Kind::Scalar)) {
        if (!context.aggregateEqualities.contains(&binary) && !context.variantEqualities.contains(&binary) &&
            left->kind != EvaluatedValue::Kind::Case)
            return Unsupported(binary.location, "a declared operator '=='");
        const auto equal = ValuesEqual(*left, *right, binary.location);
        if (!equal)
            return std::nullopt;
        return MakeBool(*equal == (binary.op == TokenKind::Equal));
    }
    return Arithmetic(binary.op, *left, *right, binary.location);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::Arithmetic(const TokenKind op, const EvaluatedValue &left,
                                                                const EvaluatedValue &right,
                                                                const SourceLocation location) {
    if (left.kind != EvaluatedValue::Kind::Scalar || right.kind != EvaluatedValue::Kind::Scalar)
        return Unsupported(location, std::format("operator '{}' on '{}'", OperatorSpelling(op), left.type.ToString()));
    TypedConstant lhs = *left.scalar;
    TypedConstant rhs = *right.scalar;
    TypeRef resultType = left.type;
    // Operands of different widths meet in the wider one, as analysis typed them; a shift keeps its left operand's.
    if (!IsShift(op) && !(left.type == right.type)) {
        const bool widenRight = lhs.Width() >= rhs.Width();
        const TypeRef &target = widenRight ? left.type : right.type;
        auto converted = Evaluation::CastConstant(widenRight ? rhs : lhs, ScalarModelType(target));
        if (!converted)
            return Unsupported(location, std::format("operator '{}' on '{}' and '{}'", OperatorSpelling(op),
                                                     left.type.ToString(), right.type.ToString()));
        (widenRight ? rhs : lhs) = *converted;
        resultType = target;
    }
    const auto result = Evaluation::EvaluateBinaryChecked(op, lhs, rhs);
    if (!result.value) {
        switch (result.failure) {
        case CheckedFailure::Overflow:
            return Fail(location, std::format("'{}' overflows '{}'", OperatorSpelling(op), resultType.ToString()));
        case CheckedFailure::DivisionByZero:
            return Fail(location, std::format("'{}' divides by zero", OperatorSpelling(op)));
        case CheckedFailure::ShiftOutOfRange:
            return Fail(location,
                        std::format("a shift by {} is out of range for '{}'", rhs.ToLiteral(), resultType.ToString()));
        case CheckedFailure::Unsupported:
            break;
        }
        return Unsupported(location, std::format("operator '{}' on '{}'", OperatorSpelling(op), resultType.ToString()));
    }
    if (IsComparison(op))
        return MakeScalar(*result.value, TypeRef::MakeBool());
    return MakeScalar(*result.value, resultType);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateAssignment(const AssignExpr &assignment) {
    if (const auto *index = AstCast<const IndexExpr>(assignment.target.get());
        index && context.indexAssignments.contains(index))
        return Unsupported(assignment.location, "a declared operator '[]='");
    const TokenKind op = CompoundOperator(assignment.op);
    if (assignment.op == TokenKind::Assign || assignment.op == TokenKind::MoveArrow) {
        auto value = EvaluateAs(*assignment.value, ValueType(TypeOf(*assignment.target)));
        if (!value)
            return std::nullopt;
        const auto place = EvaluatePlace(*assignment.target);
        if (!place)
            return std::nullopt;
        EvaluatedValue *destination = Resolve(*place);
        if (!destination)
            return Unsupported(assignment.location, "this assignment");
        *destination = std::move(*value);
        return MakeUnit();
    }
    const auto place = EvaluatePlace(*assignment.target);
    if (!place)
        return std::nullopt;
    const EvaluatedValue *current = Resolve(*place);
    if (!current)
        return Unsupported(assignment.location, "this assignment");
    const EvaluatedValue old = *current;
    auto operand = EvaluateExpression(*assignment.value);
    if (!operand)
        return std::nullopt;
    auto result = Arithmetic(op, old, *operand, assignment.location);
    if (!result)
        return std::nullopt;
    result = Coerce(std::move(*result), old.type, assignment.location);
    if (!result)
        return std::nullopt;
    EvaluatedValue *destination = Resolve(*place);
    if (!destination)
        return Unsupported(assignment.location, "this assignment");
    *destination = std::move(*result);
    return MakeUnit();
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateIncrement(const Expr &operand, const TokenKind op,
                                                                       const bool prefix,
                                                                       const SourceLocation location) {
    const auto place = EvaluatePlace(operand);
    if (!place)
        return std::nullopt;
    const EvaluatedValue *current = Resolve(*place);
    if (!current || current->kind != EvaluatedValue::Kind::Scalar)
        return Unsupported(location, std::format("operator '{}{}'", OperatorSpelling(op), OperatorSpelling(op)));
    const EvaluatedValue old = *current;
    const auto one = MakeInteger("1", old.type, location);
    if (!one)
        return std::nullopt;
    auto result = Arithmetic(op == TokenKind::PlusPlus ? TokenKind::Plus : TokenKind::Minus, old, *one, location);
    if (!result)
        return std::nullopt;
    EvaluatedValue *destination = Resolve(*place);
    *destination = *result;
    return prefix ? *result : old;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateCast(const CastExpr &cast) {
    auto value = EvaluateExpression(*cast.operand);
    if (!value)
        return std::nullopt;
    return CastValue(*value, ValueType(TypeOf(cast)), cast.location);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::CastValue(const EvaluatedValue &value, const TypeRef &target,
                                                               const SourceLocation location) {
    if (value.type == target)
        return value;
    if (target->kind == TypeRef::Kind::Pointer) {
        static_cast<void>(Fail(location, std::format("{} makes a raw pointer", Actor())));
        failure->forbidden = true;
        return std::nullopt;
    }
    if (value.kind != EvaluatedValue::Kind::Scalar || !(target.IsNumeric() || target.IsBool() || target.IsChar()))
        return Unsupported(location, std::format("a cast from '{}' to '{}'", value.type.ToString(), target.ToString()));
    TypedConstant source = *value.scalar;
    if (target.IsChar()) {
        // A character cast checks the value it is given; a float converts as it would to 'uint64' first.
        if (source.GetKind() == TypedConstant::Kind::Floating) {
            const auto whole = Evaluation::CastConstant(source, TypeRef::MakeUInt64());
            if (!whole)
                return Unsupported(location, "this character cast");
            source = *whole;
        }
        const bool negative = source.GetKind() == TypedConstant::Kind::SignedInteger && source.Bits().IsNegative();
        const auto codePoint = negative ? std::nullopt : source.Bits().ToUnsigned();
        const PrimitiveInfo *primitive = FindPrimitive(target->kind);
        const std::uint64_t bits = primitive ? primitive->bits : 32;
        // The same words a cast of a written constant is reported with.
        const bool scalar = bits > 16;
        if (scalar && codePoint && *codePoint >= 0xD800 && *codePoint <= 0xDFFF)
            return Fail(location, std::format("cast from '{}' to '{}' uses invalid surrogate code point U+{:04X}",
                                              value.type.ToString(), target.ToString(), *codePoint));
        const std::uint64_t limit = bits == 8 ? 0xFF : bits == 16 ? 0xFFFF : 0x10FFFF;
        if (!codePoint || *codePoint > limit)
            return Fail(location, std::format("cast from '{}' to '{}' is outside the target type's range",
                                              value.type.ToString(), target.ToString()));
        return MakeInteger(std::to_string(*codePoint), target, location);
    }
    const auto converted = Evaluation::CastConstant(source, ScalarModelType(target));
    if (!converted)
        return Unsupported(location, std::format("a cast from '{}' to '{}'", value.type.ToString(), target.ToString()));
    return MakeScalar(*converted, target);
}

std::optional<std::size_t> ConstantCallEvaluator::ElementIndex(const EvaluatedValue &array, const Expr &index) {
    const auto value = EvaluateExpression(index);
    if (!value)
        return std::nullopt;
    if (value->kind != EvaluatedValue::Kind::Scalar)
        return Unsupported(index.location, "this index");
    const TypedConstant &scalar = *value->scalar;
    const bool negative = scalar.GetKind() == TypedConstant::Kind::SignedInteger && scalar.Bits().IsNegative();
    const auto position = negative ? std::nullopt : scalar.Bits().ToUnsigned();
    if (!position || *position >= array.elements.size())
        return Fail(index.location, std::format("index {} is out of range for an array of {} elements",
                                                scalar.ToLiteral(), array.elements.size()));
    return static_cast<std::size_t>(*position);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateIndex(const IndexExpr &index) {
    if (context.indexOperators.contains(&index))
        return Unsupported(index.location, "a declared operator '[]'");
    // A constant table is read where it is, not copied for every element.
    if (const auto constant = context.constantReferences.find(index.object.get());
        constant != context.constantReferences.end()) {
        if (!ConstantValue(*constant->second))
            return std::nullopt;
        const EvaluatedValue &array = *constants.at(constant->second);
        if (array.kind == EvaluatedValue::Kind::Array) {
            const auto element = ElementIndex(array, *index.index);
            return element ? std::optional{array.elements[*element]} : std::nullopt;
        }
    }
    if (NamesStorage(*index.object)) {
        const auto place = EvaluatePlace(index);
        return place ? std::optional{*Resolve(*place)} : std::nullopt;
    }
    auto array = EvaluateExpression(*index.object);
    if (!array)
        return std::nullopt;
    if (array->kind != EvaluatedValue::Kind::Array)
        return Unsupported(index.location, std::format("indexing '{}'", array->type.ToString()));
    const auto element = ElementIndex(*array, *index.index);
    return element ? std::optional{std::move(array->elements[*element])} : std::nullopt;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateField(const FieldExpr &field) {
    std::optional<EvaluatedValue> object;
    if (NamesStorage(*field.object)) {
        const auto place = EvaluatePlace(*field.object);
        if (!place)
            return std::nullopt;
        object = *Resolve(*place);
    }
    else {
        object = EvaluateExpression(*field.object);
    }
    if (!object)
        return std::nullopt;
    if (object->kind == EvaluatedValue::Kind::Array && field.field == "length")
        return MakeInteger(std::to_string(object->elements.size()), ValueType(TypeOf(field)), field.location);
    if (object->kind == EvaluatedValue::Kind::Struct) {
        if (const auto position = object->FieldIndex(field.field))
            return std::move(object->elements[*position]);
    }
    if (object->kind == EvaluatedValue::Kind::Tuple) {
        std::size_t index = 0;
        const auto [end, error] = std::from_chars(field.field.data(), field.field.data() + field.field.size(), index);
        if (error == std::errc{} && index < object->elements.size())
            return std::move(object->elements[index]);
    }
    return Unsupported(field.location, std::format("the member '{}' of '{}'", field.field, object->type.ToString()));
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateStructInit(const StructInitExpr &initializer) {
    const TypeRef type = ValueType(TypeOf(initializer));
    const auto valueOf = [&](const std::string &name) -> const Expr * {
        const auto field = std::ranges::find(initializer.fields, name, &StructInitExpr::Field::name);
        return field == initializer.fields.end() ? nullptr : field->value.get();
    };
    // A case with named fields is built from its fields in declaration order.
    if (const auto [declaration, selected] = context.LookupEnumVariantInitializer(initializer.typeName);
        declaration && selected) {
        auto value = EvaluateCase(*declaration, *selected, type);
        for (const auto &field : selected->namedFields) {
            const Expr *part = valueOf(field.name);
            if (!part)
                return Unsupported(initializer.location, "a case with a field left out");
            auto element = EvaluateExpression(*part);
            if (!element)
                return std::nullopt;
            value->elements.push_back(std::move(*element));
        }
        return value;
    }
    const auto fields = StructFields(type);
    if (fields.empty())
        return Unsupported(initializer.location, std::format("a value of type '{}'", type.ToString()));
    EvaluatedValue value;
    value.kind = EvaluatedValue::Kind::Struct;
    value.type = type;
    std::vector<std::string> names;
    for (const auto &[name, fieldType] : fields) {
        const Expr *part = valueOf(name);
        if (!part)
            return Unsupported(initializer.location,
                               std::format("a '{}' with field '{}' left out", type.ToString(), name));
        auto element = EvaluateAs(*part, fieldType);
        if (!element)
            return std::nullopt;
        names.push_back(name);
        value.elements.push_back(std::move(*element));
    }
    value.fieldNames = std::make_shared<const std::vector<std::string>>(std::move(names));
    return value;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateArray(const Expr &expression) {
    const TypeRef type = ValueType(TypeOf(expression));
    if (type->kind != TypeRef::Kind::Array || type->inner.empty())
        return Unsupported(expression.location, std::format("a value of type '{}'", type.ToString()));
    const TypeRef &elementType = type->inner.front();
    EvaluatedValue value;
    value.kind = EvaluatedValue::Kind::Array;
    value.type = type;
    if (const auto *array = AstCast<const ArrayExpr>(&expression)) {
        for (const ExprPtr &element : array->elements) {
            auto part = EvaluateAs(*element, elementType);
            if (!part)
                return std::nullopt;
            value.elements.push_back(std::move(*part));
        }
        return value;
    }
    const auto &repeat = *AstCast<const ArrayRepeatExpr>(&expression);
    const std::uint64_t count = type->arrayLength.value_or(0);
    // Each element is one step, so a huge repeat counts against the limit as the loop filling it would.
    steps += count;
    if (!Step(expression.location))
        return std::nullopt;
    auto part = EvaluateAs(*repeat.value, elementType);
    if (!part)
        return std::nullopt;
    value.elements.assign(static_cast<std::size_t>(count), *part);
    return value;
}

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateTuple(const TupleExpr &tuple) {
    const TypeRef type = ValueType(TypeOf(tuple));
    EvaluatedValue value;
    value.kind = EvaluatedValue::Kind::Tuple;
    value.type = type;
    for (std::size_t index = 0; index < tuple.elements.size(); ++index) {
        auto part = EvaluateAs(*tuple.elements[index], type->kind == TypeRef::Kind::Tuple && index < type->inner.size()
                                                           ? type->inner[index]
                                                           : TypeRef::MakeUnknown());
        if (!part)
            return std::nullopt;
        value.elements.push_back(std::move(*part));
    }
    return value;
}

// Calls.

std::optional<EvaluatedValue> ConstantCallEvaluator::EvaluateCall(const CallExpr &call) {
    const auto found = context.callableBindings.find(&call);
    if (found == context.callableBindings.end())
        return Unsupported(CallSiteLocation(call), "this call");
    const ResolvedCallableBinding &binding = found->second;
    std::unordered_map<std::string, TypeRef> substitutions;
    for (const auto &[name, type] : binding.substitutions)
        substitutions.emplace(name, Substitute(type));
    const SourceLocation site = CallSiteLocation(call);
    const auto *method = AstCast<const FieldExpr>(call.callee.get());
    switch (binding.dispatch) {
    case ResolvedCallableBinding::DispatchKind::EnumVariant: {
        const auto *declaration = AstCast<const EnumDecl>(binding.selectedDeclaration);
        if (!declaration || !binding.selectedVariant)
            return Unsupported(site, "this case");
        const TypeRef calleeType = TypeOf(*call.callee);
        auto value = EvaluateCase(*declaration, *binding.selectedVariant, ValueType(TypeOf(call)));
        for (std::size_t index = 0; index < call.args.size(); ++index) {
            const bool typed = calleeType->kind == TypeRef::Kind::Func && index + 1 < calleeType->inner.size();
            auto payload =
                typed ? EvaluateAs(*call.args[index], calleeType->inner[index]) : EvaluateExpression(*call.args[index]);
            if (!payload)
                return std::nullopt;
            value->elements.push_back(std::move(*payload));
        }
        return value;
    }
    case ResolvedCallableBinding::DispatchKind::Direct:
    case ResolvedCallableBinding::DispatchKind::Constructor:
    case ResolvedCallableBinding::DispatchKind::Method: {
        if (const auto *external = AstCast<const ExternFuncDecl>(binding.selectedDeclaration)) {
            static_cast<void>(Fail(site, std::format("{} calls extern function '{}'", Actor(), external->name)));
            failure->forbidden = true;
            return std::nullopt;
        }
        const auto *function = AstCast<const FuncDecl>(binding.selectedDeclaration);
        if (!function)
            return Unsupported(site, "this call");
        const bool receives = binding.dispatch == ResolvedCallableBinding::DispatchKind::Method &&
                              function->Receiver() != nullptr && method != nullptr;
        return Invoke(*function, std::move(substitutions), receives ? method->object.get() : nullptr, call.args, site);
    }
    case ResolvedCallableBinding::DispatchKind::Indirect: {
        auto callee = EvaluateExpression(*call.callee);
        if (!callee)
            return std::nullopt;
        if (callee->kind != EvaluatedValue::Kind::Function || !callee->function)
            return Unsupported(site, "a call of this value");
        return Invoke(*callee->function,
                      callee->functionSubstitutions ? *callee->functionSubstitutions
                                                    : std::unordered_map<std::string, TypeRef>{},
                      nullptr, call.args, site);
    }
    case ResolvedCallableBinding::DispatchKind::Constrained: {
        // The bound's operation is the method the substituted type's witness names.
        if (!method)
            return Unsupported(site, "this call of a bound's operation");
        TypeRef receiverType = TypeOf(*method->object);
        while ((receiverType->kind == TypeRef::Kind::Reference || receiverType->kind == TypeRef::Kind::Pointer) &&
               !receiverType->inner.empty())
            receiverType = receiverType->inner.front();
        const auto witness =
            context.constraintWitnesses.find(ConstraintWitnessKey(binding.constraintInterface, receiverType));
        if (witness == context.constraintWitnesses.end() ||
            binding.constraintOperationIndex >= witness->second.operations.size() ||
            !witness->second.operations[binding.constraintOperationIndex])
            return Unsupported(site, "this call of a bound's operation");
        const FuncDecl &target = *witness->second.operations[binding.constraintOperationIndex];
        return Invoke(target, context.MethodTypeSubstitutions(receiverType),
                      target.Receiver() ? method->object.get() : nullptr, call.args, site);
    }
    case ResolvedCallableBinding::DispatchKind::Interface:
        return Unsupported(site, "a call through an interface");
    }
    return Unsupported(site, "this call");
}

std::optional<EvaluatedValue> ConstantCallEvaluator::Argument(const Expr &argument, const TypeRef &parameterType) {
    if (parameterType->kind == TypeRef::Kind::Reference) {
        auto place = EvaluatePlace(argument);
        if (!place)
            return std::nullopt;
        EvaluatedValue reference;
        reference.kind = EvaluatedValue::Kind::Reference;
        reference.type = parameterType;
        reference.place = std::make_shared<ValuePlace>(std::move(*place));
        return reference;
    }
    return EvaluateAs(argument, parameterType);
}

std::optional<EvaluatedValue> ConstantCallEvaluator::Invoke(const FuncDecl &function,
                                                            std::unordered_map<std::string, TypeRef> substitutions,
                                                            const Expr *receiver, const std::vector<ExprPtr> &arguments,
                                                            const SourceLocation site) {
    if (!Step(site))
        return std::nullopt;
    if (callDepth >= DepthLimit) {
        static_cast<void>(Fail(site, std::format("calls nest more than {} deep", DepthLimit)));
        failure->limit = true;
        return std::nullopt;
    }
    if (StackExhausted(site))
        return std::nullopt;
    if (const auto &offense = PurityOf(function)) {
        std::vector<std::string> chain = CurrentChain();
        chain.insert(chain.end(), offense->chain.begin(), offense->chain.end());
        failure = Failure{offense->description, offense->file, offense->location, std::move(chain)};
        failure->forbidden = true;
        return std::nullopt;
    }
    if (!function.body) {
        if (!function.intrinsicName.empty())
            return Unsupported(site, std::format("a call to compiler-supplied function '{}'", function.name));
        return Unsupported(site, std::format("a call to '{}', which has no body", function.name));
    }
    if (std::ranges::any_of(function.params, [](const Param &parameter) { return parameter.isVariadic; }))
        return Unsupported(site, std::format("a call to variadic function '{}'", function.name));

    Frame frame;
    frame.function = &function;
    frame.substitutions = std::move(substitutions);
    if (const auto info = context.declarationInfos.find(&function); info != context.declarationInfos.end()) {
        frame.file = info->second.sourceName;
        frame.lookup = info->second.scope;
    }
    frame.scopes.emplace_back();
    const auto parameterType = [&](const Param &parameter) {
        const auto found = context.typeNodeTypes.find(parameter.type.get());
        const TypeRef declared = found == context.typeNodeTypes.end() ? TypeRef::MakeUnknown() : found->second;
        return frame.substitutions.empty() ? declared : context.SubstituteTypeParameters(declared, frame.substitutions);
    };
    // Arguments are evaluated where the call is written; defaults, where the function is.
    std::size_t argument = 0;
    std::vector<std::pair<const Param *, std::optional<EvaluatedValue>>> bound;
    for (const Param &parameter : function.params) {
        const TypeRef type = parameterType(parameter);
        if (type->kind == TypeRef::Kind::Pointer)
            return Unsupported(site, "a raw pointer parameter");
        if (parameter.IsReceiver()) {
            if (!receiver)
                return Unsupported(site, std::format("a call to method '{}' without its receiver", function.name));
            auto value = Argument(*receiver, type);
            if (!value)
                return std::nullopt;
            bound.emplace_back(&parameter, std::move(value));
            continue;
        }
        if (argument < arguments.size()) {
            auto value = Argument(*arguments[argument++], type);
            if (!value)
                return std::nullopt;
            bound.emplace_back(&parameter, std::move(value));
            continue;
        }
        bound.emplace_back(&parameter, std::nullopt);
    }
    frames.push_back(std::move(frame));
    ++callDepth;
    const auto leave = [&] {
        --callDepth;
        frames.pop_back();
    };
    for (auto &[parameter, value] : bound) {
        if (!value) {
            if (!parameter->defaultValue) {
                leave();
                return Unsupported(site, std::format("a call to '{}' with an argument left out", function.name));
            }
            value = EvaluateAs(**parameter->defaultValue, parameterType(*parameter));
            if (!value) {
                leave();
                return std::nullopt;
            }
        }
        Declare(parameter->name, std::move(*value));
    }
    const Completion completion = ExecuteBlock(*function.body);
    std::optional<EvaluatedValue> result;
    if (completion == Completion::Return) {
        result = std::move(returned);
        returned.reset();
        unwinding = Completion::Normal;
    }
    else if (completion == Completion::Normal) {
        result = MakeUnit();
    }
    else if (completion != Completion::Failed) {
        static_cast<void>(Unsupported(site, "a 'break' or 'continue' that leaves a function"));
    }
    TypeRef returnType = TypeRef::MakeUnknown();
    if (function.returnType) {
        returnType = DeclaredType(**function.returnType);
        if (returnType->kind == TypeRef::Kind::Reference) {
            leave();
            return Unsupported(site, std::format("a call to '{}', which returns a reference", function.name));
        }
    }
    leave();
    if (!result)
        return std::nullopt;
    if (result->kind == EvaluatedValue::Kind::Unit)
        return result;
    return Coerce(std::move(*result), returnType, site);
}
} // namespace Rux::SemanticDetail
