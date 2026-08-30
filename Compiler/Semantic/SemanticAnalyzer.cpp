// The semantic pass entry point drives conditional folding, indexing, and checks in sibling Semantic*.cpp files.

#include "Semantic/SemanticAnalyzer.h"

#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/ConditionalCompilation.h"
#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/PrimitiveConstants.h"
#include "Semantic/Type.h"
#include "Target/Layout.h"
#include "Target/Target.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
using Layout::AlignUp;

using SemanticDetail::Scope;
using SemanticDetail::SemanticAnalyzerContext;
using SemanticDetail::Symbol;

class SemanticAnalyzerImplementation final : public SemanticAnalyzerContext {
public:
    SemanticAnalyzerImplementation(
        std::vector<const Module *> &inputModules, std::vector<DepPackage> &inputDependencies,
        const std::string &inputPackageName, std::vector<SemanticDiagnostic> &inputDiagnostics,
        std::vector<SemanticSymbol> &inputSymbols, const CompileTimeContext &inputContext,
        std::unordered_map<const Expr *, TypeRef> &inputExpressionTypes,
        std::unordered_map<const TypeExpr *, TypeRef> &inputTypeNodeTypes,
        std::unordered_map<const Pattern *, TypeRef> &inputPatternTypes,
        std::unordered_map<const EnumPattern *, ResolvedCasePattern> &inputCasePatterns,
        std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> &inputVariantEqualities,
        std::unordered_map<std::string, VariantEqualityPlan> &inputVariantEqualityPlans,
        std::unordered_map<const Expr *, ValueConsumption> &inputValueConsumptions,
        std::unordered_map<const Expr *, ValueCopy> &inputValueCopies,
        std::unordered_map<const CallExpr *, ResolvedCallableBinding> &inputCallableBindings,
        std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> &inputDefaultConstructors,
        std::unordered_map<const Decl *, ResolvedSymbolIdentity> &inputSymbolIdentities,
        std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> &inputVtableIdentities,
        std::unordered_map<std::string, ResolvedTypeLayout> &inputTypeLayouts,
        std::unordered_map<const TypeQueryExpr *, std::uint64_t> &inputSizeOfValues)
        : SemanticAnalyzerContext(inputModules, inputDependencies, inputPackageName, inputDiagnostics, inputSymbols,
                                  inputContext, inputExpressionTypes, inputTypeNodeTypes, inputPatternTypes,
                                  inputCasePatterns, inputVariantEqualities, inputVariantEqualityPlans,
                                  inputValueConsumptions, inputValueCopies, inputCallableBindings,
                                  inputDefaultConstructors, inputSymbolIdentities, inputVtableIdentities,
                                  inputTypeLayouts, inputSizeOfValues) {
    }

private:
    struct DeferredGenericCall {
        const FuncDecl *callee;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    struct PendingGenericInstantiation {
        const FuncDecl *decl;
        std::unordered_map<std::string, TypeRef> substitutions;
    };

    /// An enum a generic body composes out of its own parameters -- `Option<V>` written inside a container -- noted
    /// against the function that writes it, so it can be composed again where the parameters are finally types.
    struct DeferredEnumInstantiation {
        const EnumDecl *decl;
        std::vector<TypeRef> typeArgs;
    };

    std::unordered_map<const FuncDecl *, std::vector<DeferredGenericCall>> deferredGenericCalls;
    std::unordered_map<const FuncDecl *, std::vector<DeferredEnumInstantiation>> deferredEnumInstantiations;
    std::vector<PendingGenericInstantiation> pendingGenericInstantiations;
    std::unordered_map<const FuncDecl *, std::unordered_set<std::string>> validatedGenericInstantiations;
    std::unordered_set<const FuncDecl *> reportedRunawayInstantiations;
    std::unordered_set<std::string> activeLayoutTypes;

    void ApplyModuleImports(const Module &mod) override {
        currentFile = mod.name;
        for (const auto &decl : mod.items) {
            ApplyDeclImports(*decl);
        }
    }

    void ApplyModuleImportsInScope(const Module &mod, Scope &scope) override {
        Scope *savedScope = currentScope;
        currentScope = &scope;
        ApplyModuleImports(mod);
        currentScope = savedScope;
    }

    void ApplyDeclImports(const Decl &decl) {
        if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
            CheckUseDecl(*useDecl);
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            Scope *savedScope = currentScope;
            currentScope = &ModuleScopeFor(modDecl->name, *currentScope);
            for (const auto &item : modDecl->items) {
                ApplyDeclImports(*item);
            }
            currentScope = savedScope;
        }
    }

    Scope &ModuleScopeFor(const std::string &name, Scope &parent) {
        return programIndex.ModuleScopeFor(name, parent);
    }

    // Type resolution
    std::string GenericTypeName(const NamedTypeExpr &type) {
        std::vector<TypeRef> typeArgs;
        typeArgs.reserve(type.typeArgs.size());
        for (const auto &typeArg : type.typeArgs) {
            typeArgs.push_back(ResolveType(*typeArg));
        }
        return TypeRef::InstantiationName(type.name, typeArgs);
    }

    std::string BaseTypeName(const std::string &name) const override {
        const std::size_t pos = name.find('<');
        return pos == std::string::npos ? name : name.substr(0, pos);
    }

    /// The type parameters `name` declares, as seen from the file being checked.
    ///
    /// The file comes first because the struct and enum indexes are keyed by bare name across every package: a
    /// program declaring its own `Option` displaces `Core`'s, and `Core`'s own `extend Option<T>` would then read the
    /// wrong arity and reject its own parameter. An `extend` block is written beside the type it extends, so the
    /// declaration in the same file is the one it means.
    const std::vector<TypeParameter> *AggregateTypeParams(const std::string &name) const override {
        if (const std::vector<TypeParameter> *local = programIndex.TypeParamsIn(currentFile, name)) {
            return local;
        }
        if (const auto structure = structDecls.find(name); structure != structDecls.end()) {
            return &structure->second->typeParams;
        }
        if (const auto enumeration = enumDecls.find(name); enumeration != enumDecls.end()) {
            return &enumeration->second->typeParams;
        }
        return nullptr;
    }

    std::vector<std::string> ImplTypeParams(const ImplDecl &decl) const {
        std::vector<std::string> params;
        const auto *target = dynamic_cast<const NamedTypeExpr *>(decl.extendedType.get());
        if (!target) {
            return params;
        }
        const std::vector<TypeParameter> *typeParams = AggregateTypeParams(target->name);
        if (!typeParams) {
            return params;
        }

        const std::size_t count = std::min(typeParams->size(), target->typeArgs.size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto *arg = dynamic_cast<const NamedTypeExpr *>(target->typeArgs[i].get());
            if (arg && arg->typeArgs.empty() && arg->name == (*typeParams)[i].name) {
                params.push_back(arg->name);
            }
        }
        return params;
    }

    TypeRef ParseTypeRefFromString(std::string str) const override {
        auto trim = [](std::string &s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(str);
        if (str.empty()) {
            return TypeRef::MakeUnknown();
        }

        if (str == "?") {
            return TypeRef::MakeUnknown();
        }
        if (str == "opaque") {
            return TypeRef::MakeOpaque();
        }
        if (const auto primitive = PrimitiveTypeFromName(str)) {
            return *primitive;
        }

        if (str[0] == '*' || str[0] == '&') {
            // Pointer and reference names render writability as `var` in front of the inner type, so reading a name
            // back has to take the qualifier off and restore it on that inner type.
            const bool isReference = str[0] == '&';
            std::string pointee = str.substr(1);
            trim(pointee);
            const bool writable = pointee.starts_with("var ");
            if (writable) {
                pointee = pointee.substr(4);
                trim(pointee);
            }
            TypeRef inner = ParseTypeRefFromString(std::move(pointee));
            inner.isMut = writable;
            return isReference ? TypeRef::MakeReference(std::move(inner)) : TypeRef::MakePointer(std::move(inner));
        }

        if (str.size() >= 2 && str.compare(str.size() - 2, 2, "[]") == 0) {
            return TypeRef::MakeArray(ParseTypeRefFromString(str.substr(0, str.size() - 2)));
        }

        if (str[0] == '(' && str.back() == ')') {
            std::vector<TypeRef> elems;
            std::string content = str.substr(1, str.size() - 2);
            std::size_t start = 0;
            int depth = 0;
            for (std::size_t i = 0; i < content.size(); ++i) {
                if (content[i] == '<' || content[i] == '(') {
                    depth++;
                }
                else if (content[i] == '>' || content[i] == ')') {
                    depth--;
                }
                else if (content[i] == ',' && depth == 0) {
                    elems.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
                    start = i + 1;
                }
            }
            if (start < content.size()) {
                elems.push_back(ParseTypeRefFromString(content.substr(start)));
            }
            return TypeRef::MakeTuple(elems);
        }

        const auto rangeElement = [&](const std::string_view prefix) {
            return ParseTypeRefFromString(str.substr(prefix.size(), str.size() - prefix.size() - 1));
        };
        if (str.rfind("Range<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(rangeElement("Range<"));
        }
        if (str.rfind("RangeInclusive<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(rangeElement("RangeInclusive<"), true, true, true);
        }
        if (str.rfind("RangeFrom<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(rangeElement("RangeFrom<"), true, false);
        }
        if (str.rfind("RangeTo<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(rangeElement("RangeTo<"), false, true);
        }
        if (str.rfind("RangeToInclusive<", 0) == 0 && str.back() == '>') {
            return TypeRef::MakeRange(rangeElement("RangeToInclusive<"), false, true, true);
        }
        if (str == "RangeFull") {
            return TypeRef::MakeRangeFull();
        }

        // A type parameter in scope is that parameter, not a type that happens to share its name. This path reads a
        // type back out of a printed name, where `T` and a struct called `T` look alike, so the parameter list is the
        // only thing that tells them apart -- and ResolveType answers the same way for the same spelling written out.
        for (const auto &parameter : currentTypeParams) {
            if (parameter == str) {
                return TypeRef::MakeTypeParam(str);
            }
        }

        return TypeRef::MakeNamed(str);
    }

    std::vector<TypeRef> ParseTypeArgsFromTypeName(const std::string &typeName) const override {
        std::vector<TypeRef> args;
        const std::size_t pos = typeName.find('<');
        if (pos == std::string::npos || typeName.back() != '>') {
            return args;
        }
        std::string content = typeName.substr(pos + 1, typeName.size() - pos - 2);
        std::size_t start = 0;
        int depth = 0;
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '<' || content[i] == '(') {
                depth++;
            }
            else if (content[i] == '>' || content[i] == ')') {
                depth--;
            }
            else if (content[i] == ',' && depth == 0) {
                args.push_back(ParseTypeRefFromString(content.substr(start, i - start)));
                start = i + 1;
            }
        }
        if (start < content.size()) {
            args.push_back(ParseTypeRefFromString(content.substr(start)));
        }
        return args;
    }

    static TypeRef StringLiteralElementType(const Token &tok) {
        if (tok.text.starts_with("c16\"")) {
            return TypeRef::MakeChar16();
        }
        if (tok.text.starts_with("c32\"")) {
            return TypeRef::MakeChar32();
        }
        return TypeRef::MakeChar8();
    }

    static TypeRef StringLiteralType(const Token &tok) {
        return TypeRef::MakeNamed(SliceTypeName(StringLiteralElementType(tok)));
    }

    // The text of a string-literal token, with the surrounding quotes and any
    // encoding prefix removed and the common escapes decoded, for use as a
    // human-readable diagnostic message.
    static std::string DecodeStringMessage(const std::string &text) {
        const std::size_t open = text.find('"');
        if (open == std::string::npos || text.size() < open + 2 || text.back() != '"') {
            return {};
        }
        const std::string_view body(text.data() + open + 1, text.size() - open - 2);
        std::string out;
        out.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '\\' || i + 1 == body.size()) {
                out.push_back(body[i]);
                continue;
            }
            switch (body[++i]) {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case '0':
                out.push_back('\0');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            default:
                out.push_back('\\');
                out.push_back(body[i]);
                break;
            }
        }
        return out;
    }

    // `#Error(message)` and `#Warn(message)` are compile-time directives: at each
    // live call site they emit a diagnostic with their message and produce no
    // runtime code. A live call is one the `when` fold kept, so a directive in a
    // branch that is not taken never fires. The message must be a string literal.
    void EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call) override {
        const bool isError = intrinsicName == "#Error";
        if (call.args.size() != 1 || !call.args[0]) {
            EmitError(call.location, std::format("'{}' expects exactly one string argument", intrinsicName));
            return;
        }
        const auto *literal = dynamic_cast<const LiteralExpr *>(call.args[0].get());
        if (!literal || literal->token.kind != TokenKind::StringLiteral) {
            EmitError(call.args[0]->location, std::format("'{}' message must be a string literal", intrinsicName));
            return;
        }
        std::string message = DecodeStringMessage(literal->token.text);
        if (isError) {
            EmitError(call.location, std::move(message));
        }
        else {
            EmitWarning(call.location, std::move(message));
        }
    }

    static TypeRef CharLiteralType(const Token &tok) {
        if (tok.text.starts_with("c8'")) {
            return TypeRef::MakeChar8();
        }
        if (tok.text.starts_with("c16'")) {
            return TypeRef::MakeChar16();
        }
        if (tok.text.starts_with("c32'")) {
            return TypeRef::MakeChar32();
        }
        return TypeRef::MakeChar();
    }

    static std::string NumericLiteralSuffix(const std::string_view text) {
        return std::string(NumericLiteralSuffixOf(text));
    }

    /// The type a suffix names, built from the width and signedness the suffix table records rather than from a
    /// second list of them here. A literal with no suffix takes the default: `int`, or `float64` when it has a point.
    static TypeRef SuffixedLiteralType(const Token &tok) {
        const NumericLiteralSuffixInfo *suffix = FindNumericLiteralSuffix(NumericLiteralSuffixOf(tok.text));
        if (!suffix) {
            return tok.kind == TokenKind::FloatLiteral ? TypeRef::MakeFloat64() : TypeRef::MakeInt();
        }
        if (suffix->isFloat) {
            for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
                if (primitive.bits == suffix->bits && primitive.category == PrimitiveCategory::Float) {
                    return TypeRef::MakePrimitive(primitive.kind);
                }
            }
            return TypeRef::MakeFloat64();
        }
        if (suffix->bits == 0) {
            return suffix->isSigned ? TypeRef::MakeInt() : TypeRef::MakeUInt();
        }
        for (const PrimitiveInfo &primitive : PrimitiveCatalog()) {
            const bool matches = primitive.bits == suffix->bits &&
                                 primitive.category ==
                                     (suffix->isSigned ? PrimitiveCategory::SignedInt : PrimitiveCategory::UnsignedInt);
            if (matches) {
                return TypeRef::MakePrimitive(primitive.kind);
            }
        }
        return TypeRef::MakeInt();
    }

    static std::optional<std::uint64_t> ParseUnsuffixedIntegerLiteral(const Token &tok) {
        if (tok.kind != TokenKind::IntLiteral || !NumericLiteralSuffix(tok.text).empty()) {
            return std::nullopt;
        }

        std::string text;
        text.reserve(tok.text.size());
        for (const char c : tok.text) {
            if (c != '_') {
                text.push_back(c);
            }
        }

        int base = 10;
        std::string_view digits(text);
        if (digits.size() > 2 && digits[0] == '0') {
            switch (digits[1]) {
            case 'x':
            case 'X':
                base = 16;
                digits.remove_prefix(2);
                break;
            case 'b':
            case 'B':
                base = 2;
                digits.remove_prefix(2);
                break;
            case 'o':
            case 'O':
                base = 8;
                digits.remove_prefix(2);
                break;
            default:
                break;
            }
        }
        if (digits.empty()) {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        const auto *first = digits.data();
        const auto *last = first + digits.size();
        const auto [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
    }

    static std::optional<std::uint64_t> ParseIntegerLiteralValue(const Token &tok) {
        if (tok.kind != TokenKind::IntLiteral) {
            return std::nullopt;
        }

        std::string text;
        text.reserve(tok.text.size());
        for (const char c : tok.text) {
            if (c != '_') {
                text.push_back(c);
            }
        }

        const std::string suffix = NumericLiteralSuffix(text);
        if (!suffix.empty()) {
            text.resize(text.size() - suffix.size());
        }

        int base = 10;
        std::string_view digits(text);
        if (digits.size() > 2 && digits[0] == '0') {
            switch (digits[1]) {
            case 'x':
            case 'X':
                base = 16;
                digits.remove_prefix(2);
                break;
            case 'b':
            case 'B':
                base = 2;
                digits.remove_prefix(2);
                break;
            case 'o':
            case 'O':
                base = 8;
                digits.remove_prefix(2);
                break;
            default:
                break;
            }
        }
        if (digits.empty()) {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        const auto *first = digits.data();
        const auto *last = first + digits.size();
        const auto [ptr, ec] = std::from_chars(first, last, value, base);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
    }

    /// The width and signedness `type` is range-checked at, with the target's pointer width filled in for `int` and
    /// `uint`.
    ///
    /// @return nullopt when `type` is not an integer
    std::optional<std::pair<std::uint32_t, bool>> IntegerRange(const TypeRef &type) const {
        if (!type.IsInteger()) {
            return std::nullopt;
        }
        const auto bits = PrimitiveBits(type.kind, static_cast<std::uint32_t>(context.target.pointer_size * 8));
        if (!bits) {
            return std::nullopt;
        }
        return std::pair{*bits, type.IsSigned()};
    }

    /// Constant-expression folding still evaluates in a machine word, so these two answer only for the widths that
    /// fit one. A literal is checked by `UnsuffixedIntegerLiteralFits` instead, which is exact at every width.
    std::optional<std::uint64_t> UnsignedIntegerMax(const TypeRef &type) const {
        const auto range = IntegerRange(type);
        if (!range || range->second || range->first > 64) {
            return std::nullopt;
        }
        return WideInteger::MaxValue(range->first, false).ToUnsigned();
    }

    std::optional<std::pair<std::int64_t, std::int64_t>> SignedIntegerRange(const TypeRef &type) const {
        const auto range = IntegerRange(type);
        if (!range || !range->second || range->first > 64) {
            return std::nullopt;
        }
        const auto maximum = WideInteger::MaxValue(range->first, true).ToUnsigned();
        const auto minMagnitude = WideInteger::MinMagnitude(range->first, true).ToUnsigned();
        if (!maximum || !minMagnitude) {
            return std::nullopt;
        }
        return std::pair{static_cast<std::int64_t>(~*minMagnitude + 1), static_cast<std::int64_t>(*maximum)};
    }

    /// Whether an unsuffixed literal is one `target` holds.
    ///
    /// The magnitude is decoded at the widest width there is and range-checked afterwards, so a literal too large for
    /// its target is told apart from one that is not a literal at all, and both answers are exact however wide the
    /// target is.
    bool UnsuffixedIntegerLiteralFits(const Expr &expr, const TypeRef &target) const {
        bool negative = false;
        const LiteralExpr *literal = dynamic_cast<const LiteralExpr *>(&expr);
        if (!literal) {
            if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr); unary && unary->op == TokenKind::Minus) {
                literal = dynamic_cast<const LiteralExpr *>(unary->operand.get());
            }
            if (!literal) {
                return false;
            }
            negative = true;
        }
        if (literal->token.kind != TokenKind::IntLiteral || !NumericLiteralSuffixOf(literal->token.text).empty()) {
            return false;
        }

        const auto range = IntegerRange(target);
        if (!range) {
            return false;
        }
        const auto magnitude = DecodeIntegerLiteral(literal->token.text, WideInteger::MaxBits);
        if (!magnitude) {
            return false;
        }
        return IntegerLiteralFits(*magnitude, negative, range->first, range->second);
    }

    static bool IsNullLiteral(const Expr &expr) {
        const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
        return literal && literal->token.kind == TokenKind::NullKeyword;
    }

    static bool IsUnsuffixedIntegerLiteral(const Expr &expr) {
        const LiteralExpr *literal = dynamic_cast<const LiteralExpr *>(&expr);
        if (!literal) {
            const auto *unary = dynamic_cast<const UnaryExpr *>(&expr);
            if (!unary || unary->op != TokenKind::Minus) {
                return false;
            }
            literal = dynamic_cast<const LiteralExpr *>(unary->operand.get());
        }
        return literal && literal->token.kind == TokenKind::IntLiteral &&
               NumericLiteralSuffix(literal->token.text).empty();
    }

    bool IsIntegerLiteralOutOfRangeFor(const Expr &expr, const TypeRef &targetType) const {
        return targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr) &&
               !UnsuffixedIntegerLiteralFits(expr, targetType);
    }

    // Explains why the address of an immutable place cannot initialize a
    // writable pointer. The types alone do not point at the required binding
    // change, so name it when possible.
    std::string ImmutableAddressOfHint(const Expr &expr, const TypeRef &targetType) {
        // Only a '*var T' target can reject a read-only '*T' source this way.
        if (targetType.kind != TypeRef::Kind::Pointer || targetType.inner.empty() || !targetType.inner[0].isMut) {
            return {};
        }
        const auto *addressOf = dynamic_cast<const UnaryExpr *>(&expr);
        if (!addressOf || addressOf->op != TokenKind::At) {
            return {};
        }
        const auto *ident = dynamic_cast<const IdentExpr *>(addressOf->operand.get());
        if (!ident) {
            return ": the addressed place is immutable and yields a read-only '*T'";
        }
        const Symbol *sym = currentScope->Lookup(ident->name);
        if (sym && sym->kind == Symbol::Kind::Const) {
            return std::format(": '{}' is a constant; a mutable pointer to it is not allowed", ident->name);
        }
        if (PlaceIsImmutable(*addressOf->operand)) {
            return std::format(": '@{0}' yields a read-only '*T'; declare '{0}' with 'var' for a '*var T'",
                               ident->name);
        }
        return {};
    }

    // Picks the diagnostic for a rejected assignment/conversion. An
    // unsuffixed integer literal that does not fit the target gets a
    // dedicated "out of range" message; taking the address of an immutable
    // place gets the reason appended; everything else uses `fallback`.
    // Keeps the wording consistent across let, return, assignment, const,
    // and field positions.
    std::string AssignmentErrorMessage(const Expr &expr, const TypeRef &targetType, std::string fallback) override {
        if (IsIntegerLiteralOutOfRangeFor(expr, targetType)) {
            return std::format("integer literal is out of range for type '{}'", targetType.ToString());
        }
        if (const std::string hint = ImmutableAddressOfHint(expr, targetType); !hint.empty()) {
            return fallback + hint;
        }
        return fallback;
    }

    // Folds a compile-time-constant integer expression (unsuffixed integer
    // literals combined with the integer operators) to its int64 value,
    // using the same two's-complement wrapping the generated code produces
    // at run time, so the folded value always matches what the program
    // computes. Returns nullopt when the expression is not such a constant,
    // so callers fall back to ordinary type checking. Division/modulo by
    // zero and the INT64_MIN / -1 overflow are left unfolded and keep their
    // runtime behavior; '**' is not folded (it lowers to a runtime helper
    // call).
    static std::optional<std::int64_t> EvalConstInt(const Expr &expr) {
        using I = std::int64_t;
        using U = std::uint64_t;

        if (const auto *lit = dynamic_cast<const LiteralExpr *>(&expr)) {
            if (lit->token.kind != TokenKind::IntLiteral || !NumericLiteralSuffix(lit->token.text).empty()) {
                return std::nullopt;
            }
            const auto v = ParseUnsuffixedIntegerLiteral(lit->token);
            if (!v || *v > static_cast<U>(std::numeric_limits<I>::max())) {
                return std::nullopt;
            }
            return static_cast<I>(*v);
        }

        if (const auto *un = dynamic_cast<const UnaryExpr *>(&expr)) {
            const auto v = EvalConstInt(*un->operand);
            if (!v) {
                return std::nullopt;
            }
            switch (un->op) {
            case TokenKind::Plus:
                return *v;
            case TokenKind::Minus:
                return static_cast<I>(0u - static_cast<U>(*v));
            case TokenKind::Tilde:
                return ~*v;
            default:
                return std::nullopt;
            }
        }

        if (const auto *bin = dynamic_cast<const BinaryExpr *>(&expr)) {
            const auto l = EvalConstInt(*bin->left);
            const auto r = EvalConstInt(*bin->right);
            if (!l || !r) {
                return std::nullopt;
            }
            const U lu = static_cast<U>(*l);
            const U ru = static_cast<U>(*r);
            switch (bin->op) {
            case TokenKind::Plus:
                return static_cast<I>(lu + ru);
            case TokenKind::Minus:
                return static_cast<I>(lu - ru);
            case TokenKind::Star:
                return static_cast<I>(lu * ru);
            case TokenKind::Slash:
                if (*r == 0 || (*l == std::numeric_limits<I>::min() && *r == -1)) {
                    return std::nullopt;
                }
                return *l / *r;
            case TokenKind::Percent:
                if (*r == 0 || (*l == std::numeric_limits<I>::min() && *r == -1)) {
                    return std::nullopt;
                }
                return *l % *r;
            case TokenKind::Amp:
                return *l & *r;
            case TokenKind::Pipe:
                return *l | *r;
            case TokenKind::Caret:
                return *l ^ *r;
            case TokenKind::LessLess:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return static_cast<I>(lu << static_cast<U>(*r));
            case TokenKind::GreaterGreater:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return *l >> *r;
            case TokenKind::GreaterGreaterGreater:
                if (*r < 0 || *r >= 64) {
                    return std::nullopt;
                }
                return static_cast<I>(lu >> static_cast<U>(*r));
            default:
                return std::nullopt;
            }
        }

        return std::nullopt;
    }

    bool ConstantFitsTarget(std::int64_t value, const TypeRef &target) const {
        if (const auto max = UnsignedIntegerMax(target)) {
            return value >= 0 && static_cast<std::uint64_t>(value) <= *max;
        }
        if (const auto range = SignedIntegerRange(target)) {
            return value >= range->first && value <= range->second;
        }
        return false;
    }

    static std::optional<std::uint64_t> EvalConstCharCastValue(const Expr &expr) {
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr)) {
            if (literal->token.kind == TokenKind::CharLiteral) {
                if (const auto codePoint = Lexer::DecodeCharLiteralCodePoint(literal->token.text)) {
                    return static_cast<std::uint64_t>(*codePoint);
                }
            }
            if (literal->token.kind == TokenKind::IntLiteral) {
                if (const auto value = ParseIntegerLiteralValue(literal->token)) {
                    return *value;
                }
            }
        }

        if (const auto value = EvalConstInt(expr)) {
            if (*value >= 0) {
                return static_cast<std::uint64_t>(*value);
            }
        }

        return std::nullopt;
    }

    bool CanAssignExprTo(const Expr &expr, const TypeRef &exprType, const TypeRef &targetType) override {
        if (targetType.kind == TypeRef::Kind::Reference) {
            TypeRef targetReferent = targetType.inner.front();
            TypeRef sourceReferent = exprType.kind == TypeRef::Kind::Reference && !exprType.inner.empty()
                                       ? exprType.inner.front()
                                       : exprType;
            targetReferent.isMut = false;
            sourceReferent.isMut = false;
            const bool interfaceView = TypeImplementsInterface(sourceReferent, targetReferent);
            if (!exprType.CanImplicitlyBorrowTo(targetType) && !interfaceView) {
                return false;
            }
            if (exprType.kind == TypeRef::Kind::Reference) {
                return !exprType.inner.empty() && (!targetType.inner.front().isMut || exprType.inner.front().isMut);
            }
            // An index that resolved to a declared `[]` is a call whose result is a temporary, so it is not a place a
            // reference can borrow, however much it looks like one.
            const bool addressable =
                dynamic_cast<const IdentExpr *>(&expr) || dynamic_cast<const SelfExpr *>(&expr) ||
                dynamic_cast<const FieldExpr *>(&expr) ||
                (dynamic_cast<const IndexExpr *>(&expr) && !IsIndexOperatorCall(expr)) ||
                (dynamic_cast<const UnaryExpr *>(&expr) && static_cast<const UnaryExpr &>(expr).op == TokenKind::Star);
            if (!addressable) {
                return false;
            }
            return targetType.inner.empty() || !targetType.inner.front().isMut || !PlaceIsImmutable(expr);
        }
        if (exprType.kind == TypeRef::Kind::Array && exprType.arrayLength && !exprType.inner.empty()) {
            if (const auto sliceElement = SliceElementType(targetType)) {
                if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr)) {
                    for (const auto &element : array->elements) {
                        const TypeRef elementType = CheckExpr(*element);
                        if (!CanAssignExprTo(*element, elementType, *sliceElement)) {
                            return false;
                        }
                    }
                    return true;
                }
                if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expr)) {
                    const TypeRef elementType = CheckExpr(*repeat->value);
                    return CanAssignExprTo(*repeat->value, elementType, *sliceElement);
                }
                return exprType.inner[0].IsAssignableTo(*sliceElement);
            }
        }

        if (const auto *array = dynamic_cast<const ArrayExpr *>(&expr);
            array && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
            if (array->elements.size() != *targetType.arrayLength) {
                return false;
            }
            for (const auto &element : array->elements) {
                const TypeRef elementType = CheckExpr(*element);
                if (!CanAssignExprTo(*element, elementType, targetType.inner[0])) {
                    return false;
                }
            }
            return true;
        }

        if (const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(&expr);
            repeat && targetType.kind == TypeRef::Kind::Array && targetType.arrayLength && !targetType.inner.empty()) {
            if (exprType.kind != TypeRef::Kind::Array || exprType.arrayLength != targetType.arrayLength) {
                return false;
            }
            const TypeRef elementType = CheckExpr(*repeat->value);
            return CanAssignExprTo(*repeat->value, elementType, targetType.inner[0]);
        }

        // Tuple literals are contextually typed element-by-element. This lets
        // each element use the same assignment rules as a scalar expression
        // (notably range-checked unsuffixed integer literals), and naturally
        // handles nested tuple literals as well.
        if (const auto *tuple = dynamic_cast<const TupleExpr *>(&expr);
            tuple && exprType.kind == TypeRef::Kind::Tuple && targetType.kind == TypeRef::Kind::Tuple) {
            if (tuple->elements.size() != targetType.inner.size() || exprType.inner.size() != targetType.inner.size()) {
                return false;
            }
            for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
                if (!CanAssignExprTo(*tuple->elements[i], exprType.inner[i], targetType.inner[i])) {
                    return false;
                }
            }
            return true;
        }

        if (targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr)) {
            return UnsuffixedIntegerLiteralFits(expr, targetType);
        }

        // A constant integer expression (e.g. 10 + 2 * (5 - 3)) coerces to
        // any integer type it fits in, the same way a bare literal does.
        if (targetType.IsInteger()) {
            if (const auto folded = EvalConstInt(expr); folded && ConstantFitsTarget(*folded, targetType)) {
                return true;
            }
        }

        if (const auto *ternary = dynamic_cast<const TernaryExpr *>(&expr)) {
            const TypeRef thenType = CheckExpr(*ternary->thenExpr);
            const TypeRef elseType = CheckExpr(*ternary->elseExpr);
            if (CanAssignExprTo(*ternary->thenExpr, thenType, targetType) &&
                CanAssignExprTo(*ternary->elseExpr, elseType, targetType)) {
                return true;
            }
        }

        return exprType.IsAssignableTo(targetType) ||
               (IsNullLiteral(expr) && targetType.kind == TypeRef::Kind::Pointer) ||
               UnsuffixedIntegerLiteralFits(expr, targetType) || TypeImplementsInterface(exprType, targetType);
    }

    std::optional<std::uint64_t> EvalArrayLength(const Expr &expr) const override {
        const auto value = EvalConstInt(expr);
        if (!value || *value < 0) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(*value);
    }

    // Validate array extents and reject flexible arrays everywhere except the
    // final, top-level field of a struct. A nested T[] is never a tail field.
    void ValidateArrayType(const TypeExpr &type, bool allowFlexibleTail = false) override {
        if (const auto *array = dynamic_cast<const ArrayTypeExpr *>(&type)) {
            if (!array->size) {
                if (!allowFlexibleTail) {
                    EmitError(array->location, "flexible array type is only allowed as the final field of a struct");
                }
            }
            else if (!EvalArrayLength(*array->size)) {
                EmitError(array->size->location, "array length must be a non-negative compile-time integer");
            }
            ValidateArrayType(*array->element);
            return;
        }
        if (const auto *pointer = dynamic_cast<const PointerTypeExpr *>(&type)) {
            ValidateArrayType(*pointer->pointee);
            return;
        }
        if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
            ValidateArrayType(*reference->pointee);
            return;
        }
        if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
            for (const auto &element : tuple->elements) {
                ValidateArrayType(*element);
            }
            return;
        }
        if (const auto *function = dynamic_cast<const FunctionTypeExpr *>(&type)) {
            for (const auto &param : function->params) {
                ValidateArrayType(*param);
            }
            if (function->returnType) {
                ValidateArrayType(**function->returnType);
            }
            return;
        }
        if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
            for (const auto &arg : named->typeArgs) {
                ValidateArrayType(*arg);
            }
        }
    }

    Symbol *FindUniquePackageType(const std::string &name) const {
        auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
            return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
                   lhs.location.column == rhs.location.column;
        };

        Symbol *matched = nullptr;
        for (const auto &[_, moduleScopes] : packageModuleScopes) {
            for (const auto &[__, scope] : moduleScopes) {
                auto *sym = const_cast<Scope *>(scope)->LookupLocal(name);
                if (!sym || (sym->kind != Symbol::Kind::Type && sym->kind != Symbol::Kind::Interface)) {
                    continue;
                }
                if (matched && !sameSymbol(*matched, *sym)) {
                    return nullptr;
                }
                matched = sym;
            }
        }
        return matched;
    }

    TypeRef ResolveType(const TypeExpr &expr) override {
        TypeRef type = ResolveTypeImpl(expr);
        if (!type.IsUnknown()) {
            typeNodeTypes.insert_or_assign(&expr, type);
        }
        return type;
    }

    TypeRef ResolveTypeImpl(const TypeExpr &expr) {
        if (const auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (IsUnimplementedPrimitiveType(t->name)) {
                EmitError(expr.location, std::format("primitive type '{}' is reserved but is not implemented in this "
                                                     "compiler version",
                                                     t->name));
                return TypeRef::MakeUnknown();
            }

            for (const auto &tp : currentTypeParams) {
                if (tp == t->name) {
                    if (!t->typeArgs.empty()) {
                        EmitError(expr.location, std::format("Type parameter '{}' cannot "
                                                             "take type arguments",
                                                             t->name));
                        return TypeRef::MakeUnknown();
                    }
                    return TypeRef::MakeTypeParam(t->name);
                }
            }

            std::vector<TypeRef> resolvedArgs;
            bool hasUnknownArgs = false;
            for (const auto &argExpr : t->typeArgs) {
                TypeRef argType = ResolveType(*argExpr);
                if (argType.IsUnknown()) {
                    hasUnknownArgs = true;
                }
                resolvedArgs.push_back(argType);
            }

            if (hasUnknownArgs) {
                return TypeRef::MakeUnknown();
            }

            if (t->name == "RangeFull" && resolvedArgs.empty()) {
                return TypeRef::MakeRangeFull();
            }
            if (resolvedArgs.size() == 1) {
                if (t->name == "Range") {
                    return TypeRef::MakeRange(resolvedArgs[0]);
                }
                if (t->name == "RangeInclusive") {
                    return TypeRef::MakeRange(resolvedArgs[0], true, true, true);
                }
                if (t->name == "RangeFrom") {
                    return TypeRef::MakeRange(resolvedArgs[0], true, false);
                }
                if (t->name == "RangeTo") {
                    return TypeRef::MakeRange(resolvedArgs[0], false, true);
                }
                if (t->name == "RangeToInclusive") {
                    return TypeRef::MakeRange(resolvedArgs[0], false, true, true);
                }
            }

            if (const EnumDecl *enumeration = EnumNamed(t->name)) {
                const auto &decl = *enumeration;
                if (resolvedArgs.size() != decl.typeParams.size()) {
                    EmitGenericArityError(expr, std::format("variant type '{}'", t->name), decl.typeParams.size(),
                                          resolvedArgs.size());
                    return TypeRef::MakeUnknown();
                }
                CheckTypeReferenceConstraints(expr, decl.typeParams, resolvedArgs, std::format("enum '{}'", t->name));
                return EnumType(decl, resolvedArgs);
            }

            if (auto structType = ResolveStructTypeReference(expr, t->name, resolvedArgs)) {
                return *structType;
            }

            Symbol *sym = currentScope ? currentScope->Lookup(t->name) : nullptr;
            if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
                // Return base type if no generic arguments are provided
                if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                    return sym->type;
                }

                return TypeRef::MakeNamed(GenericTypeName(*t));
            }
            if (!sym) {
                sym = FindUniquePackageType(t->name);
                if (sym && (sym->kind == Symbol::Kind::Type || sym->kind == Symbol::Kind::Interface)) {
                    if (t->typeArgs.empty() && !sym->type.IsUnknown()) {
                        return sym->type;
                    }
                    return TypeRef::MakeNamed(GenericTypeName(*t));
                }
            }

            if (sym) {
                EmitError(expr.location,
                          std::format("name '{}' is a {}, not a type", t->name, SymbolKindName(sym->kind)),
                          {DeclarationNote(*sym)});
            }
            else {
                std::optional<std::string> help;
                if (currentScope) {
                    if (const Symbol *suggestion = currentScope->Suggest(t->name)) {
                        help = std::format("did you mean '{}'?", suggestion->name);
                    }
                }
                EmitError(expr.location, std::format("type '{}' is not defined in this scope", t->name), {},
                          std::move(help));
            }
            return TypeRef::MakeUnknown();
        }

        if (const auto *t = dynamic_cast<const PathTypeExpr *>(&expr)) {
            if (t->segments.empty()) {
                EmitError(expr.location, "empty type path");
                return TypeRef::MakeUnknown();
            }

            std::string fullPath = t->segments.front();
            for (size_t i = 1; i < t->segments.size(); ++i) {
                fullPath += "::" + t->segments[i];
            }
            return TypeRef::MakeNamed(fullPath);
        }

        if (const auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
            TypeRef pointeeType = ResolveType(*t->pointee);
            if (pointeeType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakePointer(std::move(pointeeType));
        }

        if (const auto *t = dynamic_cast<const ReferenceTypeExpr *>(&expr)) {
            TypeRef pointeeType = ResolveType(*t->pointee);
            if (pointeeType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakeReference(std::move(pointeeType));
        }

        if (const auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
            TypeRef elemType = ResolveType(*t->element);
            if (elemType.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeArray(std::move(elemType), t->size ? EvalArrayLength(*t->size) : std::nullopt);
        }

        if (const auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
            std::vector<TypeRef> elems;
            elems.reserve(t->elements.size());

            for (const auto &e : t->elements) {
                TypeRef elem = ResolveType(*e);
                if (elem.IsUnknown()) {
                    return TypeRef::MakeUnknown();
                }
                elems.push_back(elem);
            }

            return TypeRef::MakeTuple(std::move(elems));
        }

        if (dynamic_cast<const SelfTypeExpr *>(&expr)) {
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        if (const auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
            std::vector<TypeRef> paramTypes;
            paramTypes.reserve(t->params.size());
            for (const auto &p : t->params) {
                TypeRef pt = ResolveType(*p);
                if (pt.IsUnknown()) {
                    return TypeRef::MakeUnknown();
                }
                paramTypes.push_back(std::move(pt));
            }
            TypeRef ret = t->returnType ? ResolveType(*t->returnType->get()) : TypeRef::MakeOpaque();
            if (ret.IsUnknown()) {
                return TypeRef::MakeUnknown();
            }
            TypeRef funcType = TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
            funcType.isVariadic = t->isVariadic;
            return funcType;
        }

        return TypeRef::MakeUnknown();
    }

    TypeRef ResolveTypeWithSubstitution(const TypeExpr &expr,
                                        const std::unordered_map<std::string, TypeRef> &substitutions) override {
        if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
            if (t->typeArgs.empty()) {
                if (auto it = substitutions.find(t->name); it != substitutions.end()) {
                    return it->second;
                }
                return ResolveType(expr);
            }

            if (t->typeArgs.size() == 1) {
                TypeRef elemType = ResolveTypeWithSubstitution(*t->typeArgs[0], substitutions);
                if (t->name == "Range") {
                    return TypeRef::MakeRange(std::move(elemType));
                }
                if (t->name == "RangeInclusive") {
                    return TypeRef::MakeRange(std::move(elemType), true, true, true);
                }
                if (t->name == "RangeFrom") {
                    return TypeRef::MakeRange(std::move(elemType), true, false);
                }
                if (t->name == "RangeTo") {
                    return TypeRef::MakeRange(std::move(elemType), false, true);
                }
                if (t->name == "RangeToInclusive") {
                    return TypeRef::MakeRange(std::move(elemType), false, true, true);
                }
            }

            std::vector<TypeRef> resolvedArgs;
            resolvedArgs.reserve(t->typeArgs.size());
            for (const auto &typeArg : t->typeArgs) {
                resolvedArgs.push_back(ResolveTypeWithSubstitution(*typeArg, substitutions));
            }
            // An enum instantiation is composed in one place, so that a type reached through a substitution -- a
            // return type resolved while a signature is built -- carries the layout marker one resolved directly has.
            const EnumDecl *enumeration = EnumNamed(t->name);
            return enumeration ? EnumType(*enumeration, resolvedArgs)
                               : TypeRef::MakeNamed(TypeRef::InstantiationName(t->name, resolvedArgs));
        }
        if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr)) {
            TypeRef pointeeType = ResolveTypeWithSubstitution(*t->pointee, substitutions);
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakePointer(std::move(pointeeType));
        }
        if (auto *t = dynamic_cast<const ReferenceTypeExpr *>(&expr)) {
            TypeRef pointeeType = ResolveTypeWithSubstitution(*t->pointee, substitutions);
            pointeeType.isMut = pointeeType.isMut || t->pointeeMut;
            return TypeRef::MakeReference(std::move(pointeeType));
        }
        if (auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr)) {
            return TypeRef::MakeArray(ResolveTypeWithSubstitution(*t->element, substitutions),
                                      t->size ? EvalArrayLength(*t->size) : std::nullopt);
        }
        if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
            std::vector<TypeRef> elems;
            for (auto &elem : t->elements) {
                elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
            }
            return TypeRef::MakeTuple(std::move(elems));
        }
        // A callback parameter names its type parameters like any other type does, so `func(T, T) -> bool` has to be
        // substituted through as well. Without this the parameter kept an unsubstituted `T` and no argument could ever
        // match it. Lowering has resolved function types this way all along; only the analyzer was missing the case.
        if (auto *t = dynamic_cast<const FunctionTypeExpr *>(&expr)) {
            std::vector<TypeRef> paramTypes;
            paramTypes.reserve(t->params.size());
            for (const auto &param : t->params) {
                paramTypes.push_back(ResolveTypeWithSubstitution(*param, substitutions));
            }
            TypeRef returnType = t->returnType ? ResolveTypeWithSubstitution(*t->returnType->get(), substitutions)
                                               : TypeRef::MakeOpaque();
            TypeRef functionType = TypeRef::MakeFunc(std::move(paramTypes), std::move(returnType));
            functionType.isVariadic = t->isVariadic;
            return functionType;
        }
        return ResolveType(expr);
    }

    [[nodiscard]] const FuncDecl *LookupMethod(const TypeRef &receiverType, const std::string &methodName,
                                               const std::vector<TypeRef> &argTypes = {},
                                               const bool requireAccessible = true) override {
        const std::string typeName = NamedBaseTypeName(receiverType);
        if (typeName.empty()) {
            return nullptr;
        }
        const auto typeIt = methodsByType.find(typeName);
        if (typeIt == methodsByType.end()) {
            return nullptr;
        }
        const auto methodIt = typeIt->second.find(methodName);
        if (methodIt == typeIt->second.end()) {
            return nullptr;
        }
        const std::vector<const FuncDecl *> accessible =
            requireAccessible ? AccessibleMethodCandidates(receiverType, methodName) : methodIt->second;
        const auto &overloads = accessible;
        if (overloads.empty()) {
            return nullptr;
        }
        // Best-effort scrape for property access (missing args).
        if (argTypes.empty()) {
            return overloads[0];
        }
        if (overloads.size() == 1) {
            // Single overload: validate arity and assignability before
            // returning.
            const auto *decl = overloads[0];
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
            if (paramTypes.size() != argTypes.size()) {
                return nullptr;
            }
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                if (argTypes[i].IsUnknown() || paramTypes[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(paramTypes[i]) && !argTypes[i].CanImplicitlyBorrowTo(paramTypes[i]) &&
                    !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (const auto *decl : overloads) {
            std::vector<TypeRef> paramTypes = ResolveMethodParamTypes(receiverType, *decl);
            if (paramTypes.size() != argTypes.size()) {
                continue;
            }
            bool match = true;
            for (std::size_t i = 0; i < argTypes.size(); ++i) {
                if (!argTypes[i].IsUnknown() && !paramTypes[i].IsUnknown() &&
                    !argTypes[i].IsAssignableTo(paramTypes[i]) && !argTypes[i].CanImplicitlyBorrowTo(paramTypes[i]) &&
                    !(argTypes[i].IsInteger() && paramTypes[i].IsInteger())) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return decl;
            }
        }
        return nullptr;
    }

    std::unordered_map<std::string, TypeRef> MethodTypeSubstitutions(const TypeRef &receiverType) const override {
        const TypeRef *receiver = &receiverType;
        if ((receiver->kind == TypeRef::Kind::Pointer || receiver->kind == TypeRef::Kind::Reference) &&
            !receiver->inner.empty()) {
            receiver = &receiver->inner[0];
        }
        if (receiver->kind != TypeRef::Kind::Named) {
            return {};
        }

        const std::vector<TypeParameter> *typeParams = AggregateTypeParams(BaseTypeName(receiver->name));
        if (!typeParams) {
            return {};
        }
        const std::vector<TypeRef> args = ParseTypeArgsFromTypeName(receiver->name);
        std::unordered_map<std::string, TypeRef> substitutions;
        const std::size_t count = std::min(typeParams->size(), args.size());
        for (std::size_t i = 0; i < count; ++i) {
            substitutions.emplace((*typeParams)[i].name, args[i]);
        }
        return substitutions;
    }

    TypeRef InstantiateAssociatedReceiver(TypeRef receiverType, const std::vector<TypeExprPtr> &typeArgs) override {
        const std::string typeName = NamedBaseTypeName(receiverType);
        const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName);
        if (!typeParams || typeParams->empty() || typeArgs.empty()) {
            return receiverType;
        }

        std::string name = typeName + "<";
        for (std::size_t i = 0; i < typeArgs.size(); ++i) {
            if (i) {
                name += ", ";
            }
            name += ResolveType(*typeArgs[i]).ToString();
        }
        name += ">";
        return TypeRef::MakeNamed(std::move(name));
    }

    TypeRef ResolveMethodReturnType(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        const auto substitutions = MethodTypeSubstitutions(receiverType);
        TypeRef ret = method.returnType ? ResolveTypeWithSubstitution(*method.returnType->get(), substitutions)
                                        : TypeRef::MakeOpaque();
        currentSelfType = savedSelfType;
        return ret;
    }

    std::vector<TypeRef> ResolveMethodParamTypes(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            if (param.isVariadic || param.name == "self") {
                continue;
            }
            params.push_back(ResolveTypeWithSubstitution(*param.type, MethodTypeSubstitutions(receiverType)));
        }
        currentSelfType = savedSelfType;
        return params;
    }

    const FuncDecl *LookupOperatorMethod(const TypeRef &receiverType, const std::string &operatorName,
                                         const std::vector<TypeRef> &argumentTypes) override {
        return LookupMethod(receiverType, operatorName, argumentTypes);
    }

    std::vector<TypeRef> ResolveOperatorParameterTypes(const TypeRef &receiverType, const FuncDecl &method) override {
        return ResolveMethodParamTypes(receiverType, method);
    }

    TypeRef ResolveOperatorReturnType(const TypeRef &receiverType, const FuncDecl &method) override {
        return ResolveMethodReturnType(receiverType, method);
    }

    TypeRef AssociatedFunctionType(const TypeRef &receiverType, const FuncDecl &method) override {
        TypeRef savedSelfType = currentSelfType;
        currentSelfType = receiverType.kind == TypeRef::Kind::Pointer || receiverType.kind == TypeRef::Kind::Reference
                            ? receiverType
                            : TypeRef::MakePointer(receiverType);
        TypeRef type =
            MakeFuncTypeWithSubstitution(method.params, method.returnType, MethodTypeSubstitutions(receiverType),
                                         TypeParameterNames(method.typeParams));
        currentSelfType = savedSelfType;
        return type;
    }

    [[nodiscard]] const FuncDecl *LookupInterfaceMethod(const TypeRef &receiverType,
                                                        const std::string &methodName) const override {
        const std::string ifaceName = NamedBaseTypeName(receiverType);
        if (ifaceName.empty()) {
            return nullptr;
        }
        const auto ifaceIt = interfaceDecls.find(ifaceName);
        if (ifaceIt == interfaceDecls.end()) {
            return nullptr;
        }
        for (const auto &method : ifaceIt->second->methods) {
            if (method->name == methodName) {
                return method.get();
            }
        }
        return nullptr;
    }

    /// `Self` bound to the type the interface is being read for.
    static std::unordered_map<std::string, TypeRef> SelfSubstitution(const TypeRef &selfType) {
        return {{std::string(SemanticDetail::SelfTypeName), selfType}};
    }

    TypeRef ResolveInterfaceMethodReturnType(const FuncDecl &method, const TypeRef &selfType) override {
        if (!method.returnType) {
            return TypeRef::MakeOpaque();
        }
        return ResolveTypeWithSubstitution(*method.returnType->get(), SelfSubstitution(selfType));
    }

    std::vector<TypeRef> ResolveInterfaceMethodParamTypes(const FuncDecl &method, const TypeRef &selfType) override {
        const auto substitution = SelfSubstitution(selfType);
        std::vector<TypeRef> params;
        for (const auto &param : method.params) {
            // The receiver arrives as the data half of the interface value, so it is not one of the written arguments.
            if (param.isVariadic || param.IsReceiver()) {
                continue;
            }
            params.push_back(ResolveTypeWithSubstitution(*param.type, substitution));
        }
        return params;
    }

    const FuncDecl *LookupFunctionOverload(const Symbol &sym, const std::vector<TypeRef> &argTypes,
                                           const std::vector<TypeExprPtr> &typeArgs = {}) override {
        if (sym.kind != Symbol::Kind::Func || sym.funcOverloads.empty()) {
            return nullptr;
        }
        const auto borrowsAsInterface = [&](const TypeRef &argument, const TypeRef &parameter) {
            if (parameter.kind != TypeRef::Kind::Reference || parameter.inner.empty()) {
                return false;
            }
            TypeRef source = argument.kind == TypeRef::Kind::Reference && !argument.inner.empty()
                               ? argument.inner.front()
                               : argument;
            TypeRef target = parameter.inner.front();
            source.isMut = false;
            target.isMut = false;
            return TypeImplementsInterface(source, target);
        };
        if (sym.funcOverloads.size() == 1) {
            const auto *decl = sym.funcOverloads[0];
            if (!typeArgs.empty() && typeArgs.size() != decl->typeParams.size()) {
                return decl;
            }
            std::unordered_map<std::string, TypeRef> substitutions;
            const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
            for (std::size_t i = 0; i < count; ++i) {
                substitutions.emplace(decl->typeParams[i].name, ResolveType(*typeArgs[i]));
            }
            if (substitutions.size() < decl->typeParams.size()) {
                DeduceTypeArguments(*decl, argTypes, substitutions);
            }
            TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                            TypeParameterNames(decl->typeParams));
            if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                return decl;
            }
            const std::size_t paramCount = funcType.inner.size() - 1;
            const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
            std::size_t requiredCount = 0;
            for (const auto &p : decl->params) {
                if (!p.isVariadic && !p.defaultValue) {
                    ++requiredCount;
                }
            }
            const bool arityOk = isVariadic ? argTypes.size() >= requiredCount
                                            : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
            if (!arityOk) {
                return nullptr;
            }
            for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                if (argTypes[i].IsUnknown() || funcType.inner[i].IsUnknown()) {
                    continue;
                }
                if (!argTypes[i].IsAssignableTo(funcType.inner[i]) &&
                    !argTypes[i].CanImplicitlyBorrowTo(funcType.inner[i]) &&
                    !borrowsAsInterface(argTypes[i], funcType.inner[i]) &&
                    !(argTypes[i].IsInteger() && funcType.inner[i].IsInteger())) {
                    return nullptr;
                }
            }
            return decl;
        }
        for (const bool allowVariadic : {false, true}) {
            for (const bool exactOnly : {true, false}) {
                for (const auto *decl : sym.funcOverloads) {
                    if (!typeArgs.empty() && typeArgs.size() != decl->typeParams.size()) {
                        continue;
                    }
                    std::unordered_map<std::string, TypeRef> substitutions;
                    const std::size_t count = std::min(decl->typeParams.size(), typeArgs.size());
                    for (std::size_t i = 0; i < count; ++i) {
                        substitutions.emplace(decl->typeParams[i].name, ResolveType(*typeArgs[i]));
                    }
                    if (substitutions.size() < decl->typeParams.size()) {
                        DeduceTypeArguments(*decl, argTypes, substitutions);
                    }
                    if (!TypeArgumentsSatisfyBounds(decl->typeParams, substitutions)) {
                        continue;
                    }
                    TypeRef funcType = MakeFuncTypeWithSubstitution(decl->params, decl->returnType, substitutions,
                                                                    TypeParameterNames(decl->typeParams));
                    if (funcType.kind != TypeRef::Kind::Func || funcType.inner.empty()) {
                        continue;
                    }
                    const std::size_t paramCount = funcType.inner.size() - 1;
                    const bool isVariadic = !decl->params.empty() && decl->params.back().isVariadic;
                    if (isVariadic != allowVariadic) {
                        continue;
                    }
                    std::size_t requiredCount = 0;
                    for (const auto &p : decl->params) {
                        if (!p.isVariadic && !p.defaultValue) {
                            ++requiredCount;
                        }
                    }
                    const bool arityOk = isVariadic
                                           ? argTypes.size() >= requiredCount
                                           : (argTypes.size() >= requiredCount && argTypes.size() <= paramCount);
                    if (!arityOk) {
                        continue;
                    }
                    bool match = true;
                    for (std::size_t i = 0; i < std::min(argTypes.size(), paramCount); ++i) {
                        const TypeRef &paramType = funcType.inner[i];
                        if (argTypes[i].IsUnknown() || paramType.IsUnknown()) {
                            continue;
                        }
                        // An unsuffixed integer literal carries the default `int`, which is assignable to no other
                        // integer width. The single-overload path above lets one reach any integer parameter anyway;
                        // without the same allowance here, giving a name a second overload stopped every call that
                        // passed a bare literal from resolving at all. Only the coercing pass grants it, so an exact
                        // match still wins, and only `int` widens, so an explicitly typed argument is never silently
                        // narrowed to a different width.
                        const bool literalToInteger = argTypes[i].kind == TypeRef::Kind::Int && paramType.IsInteger();
                        const bool assignable = argTypes[i].IsAssignableTo(paramType) ||
                                                argTypes[i].CanImplicitlyBorrowTo(paramType) ||
                                                borrowsAsInterface(argTypes[i], paramType) || literalToInteger;
                        if (exactOnly ? !(argTypes[i] == paramType) : !assignable) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        return decl;
                    }
                }
            }
        }
        return nullptr;
    }

    TypeRef FunctionType(const FuncDecl &decl) {
        return MakeFuncType(decl.params, decl.returnType, TypeParameterNames(decl.typeParams));
    }

    static std::optional<std::uint64_t> CheckedAlignUp(const std::uint64_t value, const std::uint64_t alignment) {
        if (alignment == 0 || value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
            return std::nullopt;
        }
        return AlignUp(value, alignment);
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeRef(const TypeRef &inputType, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        if ((inputType.kind == TypeRef::Kind::Named || inputType.kind == TypeRef::Kind::TypeParam) &&
            substitutions.contains(inputType.name) &&
            substitutions.at(inputType.name).ToString() != inputType.ToString()) {
            return LayoutOfTypeRef(substitutions.at(inputType.name), substitutions);
        }

        const std::string key = inputType.ToString();
        if (const auto known = typeLayouts.find(key); known != typeLayouts.end()) {
            return known->second;
        }
        if (!activeLayoutTypes.insert(key).second) {
            return std::nullopt;
        }

        const auto finish = [&](std::optional<ResolvedTypeLayout> result) {
            activeLayoutTypes.erase(key);
            if (result) {
                typeLayouts.insert_or_assign(key, *result);
            }
            return result;
        };
        const auto checkedAdd = [](const std::uint64_t left,
                                   const std::uint64_t right) -> std::optional<std::uint64_t> {
            if (right > std::numeric_limits<std::uint64_t>::max() - left) {
                return std::nullopt;
            }
            return left + right;
        };
        const auto checkedMultiply = [](const std::uint64_t left,
                                        const std::uint64_t right) -> std::optional<std::uint64_t> {
            if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
                return std::nullopt;
            }
            return left * right;
        };

        if (inputType.kind == TypeRef::Kind::Named) {
            if (inputType.name.starts_with("Slice<") || inputType.name == "Slice") {
                return finish(ResolvedTypeLayout{16, 8});
            }

            const std::string baseName = BaseTypeName(inputType.name);
            std::unordered_map<std::string, TypeRef> localSubs = substitutions;
            const std::vector<TypeRef> typeArgs = ParseTypeArgsFromTypeName(inputType.name);
            if (const auto structure = structDecls.find(baseName); structure != structDecls.end()) {
                const std::size_t count = std::min(structure->second->typeParams.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[structure->second->typeParams[i].name] = typeArgs[i];
                }
                return finish(LayoutOfStruct(*structure->second, localSubs));
            }
            if (const auto enumeration = enumDecls.find(baseName); enumeration != enumDecls.end()) {
                const std::size_t count = std::min(enumeration->second->typeParams.size(), typeArgs.size());
                for (std::size_t i = 0; i < count; ++i) {
                    localSubs[enumeration->second->typeParams[i].name] = typeArgs[i];
                }
                return finish(LayoutOfEnum(*enumeration->second, localSubs));
            }
            if (const auto unionType = unionDecls.find(baseName); unionType != unionDecls.end()) {
                return finish(LayoutOfUnion(*unionType->second, localSubs));
            }

            // Interface values are fat pointers: {data, vtable}.
            if (Symbol *sym = currentScope->Lookup(baseName); sym) {
                if (sym->kind == Symbol::Kind::Interface) {
                    return finish(ResolvedTypeLayout{16, 8});
                }
                if (sym->kind == Symbol::Kind::Type && !sym->type.IsUnknown() && !(sym->type == inputType)) {
                    return finish(LayoutOfTypeRef(sym->type, localSubs));
                }
            }
            if (!inputType.inner.empty()) {
                return finish(LayoutOfTypeRef(inputType.inner[0], localSubs));
            }
            return finish(std::nullopt);
        }

        if (inputType.kind == TypeRef::Kind::Reference && !inputType.inner.empty()) {
            const TypeRef referent = inputType.inner.front();
            if (referent.kind == TypeRef::Kind::Named) {
                const auto interface = interfaceDecls.find(BaseTypeName(referent.name));
                if (interface != interfaceDecls.end()) {
                    return finish(ResolvedTypeLayout{16, 8});
                }
            }
        }

        if (inputType.kind == TypeRef::Kind::Tuple) {
            std::uint64_t offset = 0;
            std::uint64_t alignment = 1;
            for (const TypeRef &element : inputType.inner) {
                const auto elementLayout = LayoutOfTypeRef(element, substitutions);
                if (!elementLayout) {
                    return finish(std::nullopt);
                }
                const auto alignedOffset = CheckedAlignUp(offset, elementLayout->alignment);
                if (!alignedOffset) {
                    return finish(std::nullopt);
                }
                offset = *alignedOffset;
                const auto end = checkedAdd(offset, elementLayout->size > 0 ? elementLayout->size : 8);
                if (!end) {
                    return finish(std::nullopt);
                }
                offset = *end;
                alignment = std::max(alignment, elementLayout->alignment);
            }
            const auto size = CheckedAlignUp(offset, alignment);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt);
        }

        if (inputType.kind == TypeRef::Kind::Array) {
            if (inputType.inner.empty() || !inputType.arrayLength) {
                return finish(std::nullopt);
            }
            const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
            if (!elementLayout) {
                return finish(std::nullopt);
            }
            const auto size = checkedMultiply(elementLayout->size, *inputType.arrayLength);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
        }

        if (inputType.IsRange()) {
            if (inputType.kind == TypeRef::Kind::RangeFull) {
                return finish(ResolvedTypeLayout{0, 1});
            }
            if (inputType.inner.empty()) {
                return finish(std::nullopt);
            }
            const auto elementLayout = LayoutOfTypeRef(inputType.inner[0], substitutions);
            if (!elementLayout || elementLayout->size == 0) {
                return finish(std::nullopt);
            }
            const std::uint64_t count =
                inputType.kind == TypeRef::Kind::Range || inputType.kind == TypeRef::Kind::RangeInclusive ? 2 : 1;
            const auto size = checkedMultiply(elementLayout->size, count);
            return finish(size ? std::optional(ResolvedTypeLayout{*size, elementLayout->alignment}) : std::nullopt);
        }

        const auto size = inputType.SizeInBytes();
        return finish(size ? std::optional(ResolvedTypeLayout{*size, Layout::FieldAlign(*size)}) : std::nullopt);
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfFields(const std::vector<TypeRef> &fields,
                   const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t offset = 0;
        std::uint64_t alignment = 1;
        for (const TypeRef &field : fields) {
            const auto fieldLayout = LayoutOfTypeRef(field, substitutions);
            if (!fieldLayout) {
                return std::nullopt;
            }
            const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
            if (!alignedOffset) {
                return std::nullopt;
            }
            offset = *alignedOffset;
            if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
                return std::nullopt;
            }
            offset += fieldLayout->size > 0 ? fieldLayout->size : 8;
            alignment = std::max(alignment, fieldLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, alignment);
        return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout> LayoutOfEnum(const EnumDecl &decl,
                                                   const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        const auto tagLayout = LayoutOfTypeRef(EnumBaseType(decl), substitutions);
        if (!tagLayout) {
            return std::nullopt;
        }
        if (decl.form == EnumDecl::Form::Enumeration) {
            return tagLayout;
        }

        bool hasPayload = false;
        ResolvedTypeLayout maximumPayload;
        for (const auto &variant : decl.variants) {
            std::vector<TypeRef> fields;
            fields.reserve(variant.fields.size() + variant.namedFields.size());
            for (const auto &field : variant.fields) {
                fields.push_back(ResolveTypeWithSubstitution(*field, substitutions));
            }
            for (const auto &field : variant.namedFields) {
                fields.push_back(ResolveTypeWithSubstitution(*field.type, substitutions));
            }
            if (fields.empty()) {
                continue;
            }
            hasPayload = true;
            const auto payload = LayoutOfFields(fields, substitutions);
            if (!payload) {
                return std::nullopt;
            }
            maximumPayload.size = std::max(maximumPayload.size, payload->size);
            maximumPayload.alignment = std::max(maximumPayload.alignment, payload->alignment);
        }

        if (!hasPayload) {
            return tagLayout;
        }
        const std::uint64_t alignment = std::max(tagLayout->alignment, maximumPayload.alignment);
        const auto payloadOffset = CheckedAlignUp(tagLayout->size, maximumPayload.alignment);
        if (!payloadOffset || maximumPayload.size > std::numeric_limits<std::uint64_t>::max() - *payloadOffset) {
            return std::nullopt;
        }
        const auto size = CheckedAlignUp(*payloadOffset + maximumPayload.size, alignment);
        return size ? std::optional(ResolvedTypeLayout{*size, alignment}) : std::nullopt;
    }

    TypeRef EnumBaseType(const EnumDecl &decl) {
        return decl.baseType ? ResolveType(*decl.baseType) : TypeRef::MakeInt();
    }

    TypeRef EnumType(const EnumDecl &decl, const std::vector<TypeRef> &typeArgs = {}) override {
        TypeRef type = TypeRef::MakeNamed(TypeRef::InstantiationName(decl.name, typeArgs));
        if (decl.typeParams.empty()) {
            // `inner` carries how large the value is, and nothing reads it as the tag -- the tag's own type is kept
            // beside the declaration. An enum that is only a discriminant is the size of that discriminant, so its
            // base type says it. One carrying a payload is wider than its tag, and only the layout knows by how much;
            // recording the tag there instead sized the whole value as the tag, so a call took the tag alone and left
            // the payload behind.
            // Record the layout under the name whatever the enum's shape: lowering builds this type a second time
            // and reads the size back from here, having no layout machinery of its own, and it needs the size of a
            // plain discriminant enum just as much when substitution has dropped what `inner` said.
            const auto layout = LayoutOfTypeRef(type);
            if (layout) {
                typeLayouts.insert_or_assign(type.name, *layout);
            }
            if (decl.form == EnumDecl::Form::Variant && layout) {
                type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
                return type;
            }
            type.inner.push_back(EnumBaseType(decl));
            return type;
        }
        if (typeArgs.size() == decl.typeParams.size()) {
            // An instantiation built out of the enclosing generic's parameters cannot be laid out here and is not
            // spelled anywhere else, so it is noted against the function writing it and composed again at each
            // instantiation of that function -- which is where the arguments are finally types.
            if (currentFunctionDecl && std::ranges::any_of(typeArgs, [this](const TypeRef &argument) {
                    return MentionsTypeParameter(argument);
                })) {
                deferredEnumInstantiations[currentFunctionDecl].push_back({&decl, typeArgs});
            }
            std::unordered_map<std::string, TypeRef> substitutions;
            for (std::size_t i = 0; i < typeArgs.size(); ++i) {
                substitutions.emplace(decl.typeParams[i].name, typeArgs[i]);
            }
            if (const auto layout = LayoutOfTypeRef(type, substitutions)) {
                // Under the instantiation's own name, for the reason the branch above records it: lowering builds
                // this type a second time and reads its size back from here, having no layout machinery of its own.
                // An instantiation composed only inside generic bodies -- where nothing concrete ever spells it --
                // otherwise reached lowering with no marker at all.
                typeLayouts.insert_or_assign(type.name, *layout);
                type.inner.push_back(TypeRef::MakeArray(TypeRef::MakeChar8(), layout->size));
            }
        }
        return type;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfStruct(const StructDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t offset = 0;
        std::uint64_t maxAlign = 1;
        for (const auto &field : decl.fields) {
            const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            std::optional<ResolvedTypeLayout> fieldLayout;
            if (fieldType.kind == TypeRef::Kind::Array && !fieldType.arrayLength && !fieldType.inner.empty()) {
                const auto elementLayout = LayoutOfTypeRef(fieldType.inner[0], substitutions);
                if (elementLayout) {
                    fieldLayout = ResolvedTypeLayout{0, elementLayout->alignment};
                }
            }
            else {
                fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
            }
            if (!fieldLayout) {
                return std::nullopt;
            }
            const auto alignedOffset = CheckedAlignUp(offset, fieldLayout->alignment);
            if (!alignedOffset) {
                return std::nullopt;
            }
            offset = *alignedOffset;
            if (fieldLayout->size > std::numeric_limits<std::uint64_t>::max() - offset) {
                return std::nullopt;
            }
            offset += fieldLayout->size;
            maxAlign = std::max(maxAlign, fieldLayout->alignment);
        }
        const auto size = CheckedAlignUp(offset, maxAlign);
        return size ? std::optional(ResolvedTypeLayout{*size, maxAlign}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfUnion(const UnionDecl &decl, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        std::uint64_t size = 0;
        std::uint64_t alignment = 1;
        for (const auto &field : decl.fields) {
            const TypeRef fieldType = ResolveTypeWithSubstitution(*field.type, substitutions);
            const auto fieldLayout = LayoutOfTypeRef(fieldType, substitutions);
            if (!fieldLayout) {
                return std::nullopt;
            }
            size = std::max(size, fieldLayout->size);
            alignment = std::max(alignment, fieldLayout->alignment);
        }
        const auto alignedSize = CheckedAlignUp(size, alignment);
        return alignedSize ? std::optional(ResolvedTypeLayout{*alignedSize, alignment}) : std::nullopt;
    }

    std::optional<ResolvedTypeLayout>
    LayoutOfTypeExpr(const TypeExpr &expr, const std::unordered_map<std::string, TypeRef> &substitutions = {}) {
        return LayoutOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
    }

    std::optional<ResolvedTypeLayout> LayoutOfTypeExpression(const TypeExpr &expr) override {
        return LayoutOfTypeExpr(expr);
    }

    void RecordResolvedTypeLayouts() override {
        std::vector<TypeRef> resolvedTypes;
        resolvedTypes.reserve(typeNodeTypes.size() + expressionTypes.size() + patternTypes.size());
        for (const auto &[_, type] : typeNodeTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, type] : expressionTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, type] : patternTypes) {
            resolvedTypes.push_back(type);
        }
        for (const auto &[_, binding] : callableBindings) {
            for (const auto &[__, type] : binding.substitutions) {
                resolvedTypes.push_back(type);
            }
            if (binding.receiverType) {
                resolvedTypes.push_back(*binding.receiverType);
            }
        }
        for (const TypeRef &type : resolvedTypes) {
            LayoutOfTypeRef(type);
        }
    }

    void CheckDecl(const Decl &decl) override {
        if (auto *fn = dynamic_cast<const FuncDecl *>(&decl)) {
            if (const Symbol *symbol = currentScope->LookupLocal(fn->name);
                symbol && symbol->kind == Symbol::Kind::Func) {
                ValidateFunctionSignature(*fn, symbol->funcOverloads);
            }
            CheckFuncDecl(*fn);
        }
        else if (auto *structDecl = dynamic_cast<const StructDecl *>(&decl)) {
            CheckStructDecl(*structDecl);
        }
        else if (auto *enumDecl = dynamic_cast<const EnumDecl *>(&decl)) {
            CheckEnumDecl(*enumDecl);
        }
        else if (auto *unionDecl = dynamic_cast<const UnionDecl *>(&decl)) {
            CheckUnionDecl(*unionDecl);
        }
        else if (auto *ifaceDecl = dynamic_cast<const InterfaceDecl *>(&decl)) {
            CheckInterfaceDecl(*ifaceDecl);
        }
        else if (auto *implDecl = dynamic_cast<const ImplDecl *>(&decl)) {
            CheckImplDecl(*implDecl);
        }
        else if (auto *modDecl = dynamic_cast<const ModuleDecl *>(&decl)) {
            CheckModuleDecl(*modDecl);
        }
        else if (auto *constDecl = dynamic_cast<const ConstDecl *>(&decl)) {
            CheckConstDecl(*constDecl);
        }
        else if (auto *aliasDecl = dynamic_cast<const TypeAliasDecl *>(&decl)) {
            ValidateArrayType(*aliasDecl->type);
            ResolveType(*aliasDecl->type); // triggers unknown-type errors
        }
        else if (auto *externFn = dynamic_cast<const ExternFuncDecl *>(&decl)) {
            if (externFn->dll.empty()) {
                EmitError(externFn->location, std::format("extern function '{}' must specify a "
                                                          "source DLL via "
                                                          "#Link(\"dll.dll\")",
                                                          externFn->name));
            }
            if (externFn->returnType) {
                ValidateArrayType(*externFn->returnType->get());
                const TypeRef returnType = ResolveType(*externFn->returnType->get());
                ValidateStoredType(returnType, externFn->returnType->get()->location, "extern return type");
            }
            for (auto &p : externFn->params) {
                if (!p.isVariadic) {
                    ValidateArrayType(*p.type);
                    const TypeRef parameterType = ResolveType(*p.type);
                    if (parameterType.kind != TypeRef::Kind::Reference) {
                        ValidateStoredType(parameterType, p.location, "extern parameter");
                    }
                }
            }
        }
        else if (auto *externVar = dynamic_cast<const ExternVarDecl *>(&decl)) {
            ValidateArrayType(*externVar->type);
            ValidateStoredType(ResolveType(*externVar->type), externVar->location, "extern variable");
        }
        else if (auto *externBlock = dynamic_cast<const ExternBlockDecl *>(&decl)) {
            for (auto &item : externBlock->items) {
                CheckDecl(*item);
            }
        }
        else if (auto *useDecl = dynamic_cast<const UseDecl *>(&decl)) {
            CheckUseDecl(*useDecl);
        }
        ValidatePublicDeclaration(decl);
    }

    void CheckFuncDecl(const FuncDecl &d, bool isMethod = false) {
        auto savedTypeParams = currentTypeParams;
        const FuncDecl *savedFunctionDecl = BeginTrackedFunction(d);
        if (!isMethod) {
            currentTypeParams.clear();
            if (IsSpecialOperationName(d.name)) {
                EmitError(d.location,
                          std::format("special operation '{}' may only be declared in an extend block", d.name));
            }
            else if (IsDestructorName(d.name)) {
                EmitError(d.location, std::format("destructor '{}' may only be declared in an extend block", d.name));
            }
        }
        AppendTypeParameterNames(currentTypeParams, d.typeParams);
        const ScopedTypeParameterBounds boundScope(*this, &d.typeParams, !isMethod);

        if (d.returnType) {
            ValidateArrayType(*d.returnType->get());
        }
        TypeRef retType = d.returnType ? ResolveType(*d.returnType->get()) : TypeRef::MakeOpaque();
        if (!retType.IsOpaque() && !retType.IsUnknown()) {
            ValidateStoredType(retType, d.returnType ? d.returnType->get()->location : d.location,
                               "function return type");
        }

        auto savedRet = currentReturnType;
        currentReturnType = retType;
        const bool savedNoReturn = currentFunctionNoReturn;
        currentFunctionNoReturn = d.isNoReturn;

        PushScope();

        for (const auto &tp : d.typeParams) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = tp.name;
            sym.type = TypeRef::MakeTypeParam(tp.name);
            Define(std::move(sym));
        }

        const TypeRef savedSelfType = DeclareReceiver(d, isMethod);

        bool seenDefault = false;
        for (const auto &param : d.params) {
            if (param.IsReceiver()) {
                continue;
            }
            ValidateArrayType(*param.type);
            if (param.isVariadic) {
                seenDefault = false; // variadic ends fixed params; reset
            }
            else if (param.defaultValue) {
                seenDefault = true;
            }
            else if (seenDefault) {
                EmitError(param.location, std::format("parameter '{}' without a default "
                                                      "value cannot follow a "
                                                      "parameter with a default value",
                                                      param.name));
            }
            Symbol sym;
            sym.kind = Symbol::Kind::Var;
            sym.name = param.name;
            sym.location = param.location;
            sym.type = param.isVariadic ? TypeRef::MakeNamed(SliceTypeName(ResolveType(*param.type)))
                                        : ResolveType(*param.type);
            if (sym.type.kind != TypeRef::Kind::Reference) {
                ValidateStoredType(sym.type, param.location, "function parameter");
            }
            sym.isMut = false;
            DefineTrackedLocal(std::move(sym), true);
            if (param.defaultValue) {
                TypeRef paramType = ResolveType(*param.type);
                TypeRef defaultType = CheckExpr(**param.defaultValue);
                if (!defaultType.IsUnknown() && !paramType.IsUnknown() &&
                    !CanAssignExprTo(**param.defaultValue, defaultType, paramType)) {
                    EmitError(param.location,
                              AssignmentErrorMessage(**param.defaultValue, paramType,
                                                     std::format("default value type '{}' does not "
                                                                 "match parameter type '{}'",
                                                                 defaultType.ToString(), paramType.ToString())));
                }
            }
        }

        if (d.isAsm) {
            // An asm function's body is raw machine instructions, not Rux
            // statements, so it is validated when the assembler encodes it —
            // except for the one thing the assembler for the target cannot
            // say, which is that the body was written for the other one.
            CheckAsmBodyArchitecture(d);
        }
        else if (!d.body) {
            if (d.intrinsicName.empty() &&
                !(isMethod && (IsSpecialOperationName(d.name) || IsDestructorName(d.name)))) {
                EmitError(d.location, std::format("function '{}' has no body", d.name));
            }
        }
        else {
            CheckFunctionBody(*d.body, d, retType);
        }

        PopScope();
        currentSelfType = savedSelfType;
        currentReturnType = savedRet;
        currentFunctionNoReturn = savedNoReturn;
        currentTypeParams = savedTypeParams;
        EndTrackedFunction(savedFunctionDecl);
    }

    // An `asm func` body is written for one architecture, and nothing in the
    // syntax says which: the mnemonics do. Report the first instruction that
    // names an instruction of the architecture the compilation is not for,
    // which is the whole body's mistake rather than that one line's.
    //
    // This runs after `when` folding, so a body a build never reaches is never
    // reported, and it stops at the first offender so a body written for the
    // wrong machine costs one diagnostic rather than one per line. A body that
    // reaches this far is one the build needs and no assembler can encode, so
    // it is an error: `when #target.arch` is how a function written twice
    // reaches both machines, and every first-party body uses it.
    void CheckAsmBodyArchitecture(const FuncDecl &d) const {
        const Target::Arch target = context.target.arch;
        for (const auto &instr : d.asmBody) {
            if (instr.mnemonic.empty()) {
                continue; // a label definition
            }
            const Target::Arch mnemonicArch = AsmMnemonicArch(instr.mnemonic);
            if (mnemonicArch == Target::Arch::Unknown || mnemonicArch == target) {
                continue;
            }
            EmitError(instr.location,
                      std::format("'{}' is an {} instruction, but asm func '{}' is compiled for {}", instr.mnemonic,
                                  Target::ToDisplayString(mnemonicArch), d.name, Target::ToDisplayString(target)));
            return;
        }
    }

    void CheckStructDecl(const StructDecl &d) {
        auto savedTypeParams = currentTypeParams;
        currentTypeParams = TypeParameterNames(d.typeParams);
        const ScopedTypeParameterBounds boundScope(*this, &d.typeParams);

        PushScope();
        for (const auto &tp : d.typeParams) {
            Symbol sym;
            sym.kind = Symbol::Kind::Type;
            sym.name = tp.name;
            sym.type = TypeRef::MakeTypeParam(tp.name);
            Define(sym);
        }

        std::unordered_set<std::string> seen;
        for (std::size_t i = 0; i < d.fields.size(); ++i) {
            const auto &field = d.fields[i];
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
            }
            const auto *array = dynamic_cast<const ArrayTypeExpr *>(field.type.get());
            const bool isFlexibleTail = array && !array->size && i + 1 == d.fields.size();
            ValidateArrayType(*field.type, isFlexibleTail);
            ValidateStoredType(ResolveType(*field.type), field.location,
                               std::format("field '{}' in struct '{}'", field.name, d.name));
        }

        PopScope();
        currentTypeParams = savedTypeParams;
    }

    void CheckEnumDecl(const EnumDecl &d) {
        const auto savedTypeParams = currentTypeParams;
        AppendTypeParameterNames(currentTypeParams, d.typeParams);
        const ScopedTypeParameterBounds boundScope(*this, &d.typeParams, /*replaceEnclosing=*/false);
        const TypeRef baseType = EnumBaseType(d);
        if (!d.IsVariant() && !baseType.IsUnknown() && !baseType.IsInteger()) {
            EmitError(d.location, std::format("enum '{}' base type must be an integer type", d.name));
        }
        const std::string_view declarationName = d.IsVariant() ? "variant" : "enum";
        const std::string_view caseName = d.IsVariant() ? "case" : "enumerator";
        std::unordered_set<std::string> seen;
        for (const auto &variant : d.variants) {
            if (!seen.insert(variant.name).second) {
                EmitError(variant.location,
                          std::format("duplicate {} '{}' in {} '{}'", caseName, variant.name, declarationName, d.name));
            }
            if (variant.discriminant && (!variant.fields.empty() || !variant.namedFields.empty())) {
                EmitError(variant.location, std::format("{} '{}::{}' cannot have both fields and a discriminant",
                                                        caseName, d.name, variant.name));
            }
            for (const auto &f : variant.fields) {
                ValidateArrayType(*f);
                ValidateStoredType(
                    ResolveType(*f), f->location,
                    std::format("payload in {} {} '{}::{}'", declarationName, caseName, d.name, variant.name));
            }
            std::unordered_set<std::string> namedFields;
            for (const auto &f : variant.namedFields) {
                if (!namedFields.insert(f.name).second) {
                    EmitError(f.location, std::format("duplicate field '{}' in {} {} '{}::{}'", f.name, declarationName,
                                                      caseName, d.name, variant.name));
                }
                ValidateArrayType(*f.type);
                ValidateStoredType(ResolveType(*f.type), f.location,
                                   std::format("field '{}' in {} {} '{}::{}'", f.name, declarationName, caseName,
                                               d.name, variant.name));
            }
        }
        currentTypeParams = savedTypeParams;
    }

    void CheckUnionDecl(const UnionDecl &d) {
        std::unordered_set<std::string> seen;
        for (const auto &field : d.fields) {
            if (!seen.insert(field.name).second) {
                EmitError(field.location, std::format("duplicate field '{}' in union '{}'", field.name, d.name));
            }
            ValidateArrayType(*field.type);
            ValidateStoredType(ResolveType(*field.type), field.location,
                               std::format("field '{}' in union '{}'", field.name, d.name));
        }
    }

    void CheckInterfaceDecl(const InterfaceDecl &d) {
        // `Self` stands for whichever type implements this interface, which is not known here, so it is checked as a
        // type parameter and bound to the implementing type wherever the interface is actually read.
        const auto savedTypeParams = currentTypeParams;
        currentTypeParams.emplace_back(SemanticDetail::SelfTypeName);
        const auto restore = [&] { currentTypeParams = savedTypeParams; };

        std::unordered_set<std::string> seen;
        for (const auto &method : d.methods) {
            if (!seen.insert(method->name).second) {
                EmitError(method->location,
                          std::format("duplicate method '{}' in interface '{}'", method->name, d.name));
            }
            if (method->returnType) {
                ValidateStoredType(ResolveType(**method->returnType), method->returnType->get()->location,
                                   "interface method return type");
            }
            for (const auto &p : method->params) {
                if (!p.isVariadic) {
                    const TypeRef parameterType = ResolveType(*p.type);
                    if (parameterType.kind != TypeRef::Kind::Reference) {
                        ValidateStoredType(parameterType, p.location, "interface method parameter");
                    }
                }
            }
        }
        restore();
    }

    void CheckImplDecl(const ImplDecl &d) {
        const auto savedTypeParams = currentTypeParams;
        currentTypeParams = ImplTypeParams(d);

        // A compound receiver (e.g. `int[]`) resolves through the type
        // expression rather than a named symbol.
        const std::string typeName = d.typeName.starts_with("Slice<") ? d.typeName : BaseTypeName(d.typeName);
        // An extend block borrows the extended type's parameters, so it borrows their bounds too: a method body passing
        // `T` on to a constrained generic is checked against what the struct declared rather than left unconstrained.
        const ScopedTypeParameterBounds boundScope(*this, AggregateTypeParams(typeName));
        const Symbol *extendedSymbol = currentScope->Lookup(typeName);
        if (d.extendedType) {
            ValidateArrayType(*d.extendedType);
        }
        const bool receiverMayResolve = extendedSymbol != nullptr || d.typeName.starts_with("Slice<");
        TypeRef extendedType =
            d.extendedType && receiverMayResolve ? ResolveType(*d.extendedType) : TypeRef::MakeUnknown();
        const bool isSliceReceiver =
            extendedType.kind == TypeRef::Kind::Array ||
            (extendedType.kind == TypeRef::Kind::Named && extendedType.name.starts_with("Slice<"));
        if (!isSliceReceiver && !extendedSymbol) {
            std::optional<std::string> help;
            if (const Symbol *suggestion = currentScope->Suggest(typeName)) {
                help = std::format("did you mean '{}'?", suggestion->name);
            }
            EmitError(d.location, std::format("cannot extend type '{}' because it is not defined", d.typeName), {},
                      std::move(help));
        }
        else if (extendedSymbol && extendedSymbol->kind != Symbol::Kind::Type) {
            EmitError(d.location,
                      std::format("cannot extend '{}' because it is a {}, not a type", d.typeName,
                                  SymbolKindName(extendedSymbol->kind)),
                      {DeclarationNote(*extendedSymbol)});
        }

        if (d.interfaceName) {
            Symbol *ifaceSym = currentScope->Lookup(*d.interfaceName);
            if (!ifaceSym) {
                std::optional<std::string> help;
                if (const Symbol *suggestion = currentScope->Suggest(*d.interfaceName)) {
                    help = std::format("did you mean '{}'?", suggestion->name);
                }
                EmitError(d.location, std::format("interface '{}' is not defined", *d.interfaceName), {},
                          std::move(help));
            }
            else if (ifaceSym->kind != Symbol::Kind::Interface) {
                EmitError(d.location,
                          std::format("name '{}' is a {}, not an interface", *d.interfaceName,
                                      SymbolKindName(ifaceSym->kind)),
                          {DeclarationNote(*ifaceSym)});
            }
            else {
                std::unordered_set<std::string> implNames;
                for (const auto &m : d.methods) {
                    implNames.insert(m->name);
                }
                for (const auto &required : ifaceSym->interfaceMethods) {
                    if (!implNames.count(required)) {
                        EmitError(d.location,
                                  std::format("implementation of interface '{}' for type '{}' is missing method '{}'",
                                              *d.interfaceName, d.typeName, required),
                                  {std::format("interface '{}' requires method '{}'", *d.interfaceName, required)});
                    }
                }
            }
        }

        bool savedInImpl = inImpl;
        TypeRef savedSelfType = currentSelfType;
        const ImplDecl *savedImpl = currentImpl;
        TypeRef savedExtendedType = currentExtendedType;
        inImpl = true;
        currentImpl = &d;
        currentExtendedType = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
        // Each method replaces this with what its own receiver declares. It stands for the block as a whole: what
        // `self` written as a type resolves to, and what a method that declares no receiver at all would see.
        if (isSliceReceiver) {
            // A slice is a fat pointer already; `self` is the slice value, so
            // `for x in self` and `self[i]` work directly.
            currentSelfType = extendedType;
        }
        else {
            TypeRef selfBase = extendedType.IsUnknown() ? TypeRef::MakeNamed(d.typeName) : extendedType;
            currentSelfType = TypeRef::MakePointer(selfBase);
        }
        for (const auto &m : d.methods) {
            if (const auto typeIt = methodsByType.find(typeName); typeIt != methodsByType.end()) {
                if (const auto methodIt = typeIt->second.find(m->name); methodIt != typeIt->second.end()) {
                    ValidateFunctionSignature(*m, methodIt->second, /*isMethod=*/true);
                }
            }
            ValidateSpecialOperation(*m, currentExtendedType);
            ValidateIndexOperator(*m, currentExtendedType);
            ValidateDestructor(*m, currentExtendedType);
            ValidateConstructor(*m, currentExtendedType);
            CheckFuncDecl(*m, /*isMethod=*/true);
            ValidatePublicFunction(*m, std::format("public method '{}.{}'", typeName, m->name), d.extendedType.get());
        }
        currentSelfType = savedSelfType;
        currentExtendedType = savedExtendedType;
        currentImpl = savedImpl;
        inImpl = savedInImpl;
        currentTypeParams = savedTypeParams;
    }

    void CheckModuleDecl(const ModuleDecl &d) {
        Scope *savedScope = currentScope;
        currentScope = &ModuleScopeFor(d.name, *currentScope);
        for (const auto &item : d.items) {
            CheckDecl(*item);
        }
        currentScope = savedScope;
    }

    static bool IsSliceTypeRef(const TypeRef &type) {
        return type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<");
    }

    // An element of a constant array must reduce to a literal, since the array
    // is laid out in read-only data rather than evaluated at each use.
    bool IsConstArrayElement(const Expr &e) const {
        if (dynamic_cast<const LiteralExpr *>(&e)) {
            return true;
        }
        if (const auto *u = dynamic_cast<const UnaryExpr *>(&e)) {
            return u->op == TokenKind::Minus && IsConstArrayElement(*u->operand);
        }
        if (const auto *ident = dynamic_cast<const IdentExpr *>(&e)) {
            const Symbol *sym = currentScope->Lookup(ident->name);
            return sym && sym->kind == Symbol::Kind::Const;
        }
        return false;
    }

    void CheckConstDecl(const ConstDecl &d) {
        if (!d.intrinsicName.empty()) {
            if (!d.type) {
                EmitError(d.location, std::format("'intrinsic' constant '{}' requires a type", d.name));
                return;
            }
            const TypeRef constType = ResolveType(**d.type);
            ValidateStoredType(constType, d.location, "intrinsic constant");
            if (Symbol *sym = currentScope->Lookup(d.name)) {
                sym->type = constType;
                sym->intrinsicName = d.intrinsicName;
            }
            return;
        }
        if (!d.value) {
            EmitError(d.location, std::format("constant '{}' requires an initializer", d.name));
            return;
        }
        if (d.type) {
            ValidateArrayType(**d.type);
        }
        TypeRef valueType = CheckExpr(*d.value);
        TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
        ValidateStoredType(constType, d.location, "constant");
        if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
            !CanAssignExprTo(*d.value, valueType, constType)) {
            EmitError(d.value->location,
                      AssignmentErrorMessage(*d.value, constType,
                                             std::format("cannot assign '{}' to constant of type '{}'",
                                                         valueType.ToString(), constType.ToString())));
        }
        if (IsSliceTypeRef(constType) || constType.kind == TypeRef::Kind::Array) {
            const auto *array = dynamic_cast<const ArrayExpr *>(d.value.get());
            const auto *repeat = dynamic_cast<const ArrayRepeatExpr *>(d.value.get());
            const bool isText = dynamic_cast<const LiteralExpr *>(d.value.get()) != nullptr;
            if (!isText && !array && !repeat) {
                EmitError(d.value->location,
                          "a constant sequence must be initialized with an array literal or a string literal");
            }
            else if (array) {
                for (const auto &element : array->elements) {
                    if (!IsConstArrayElement(*element)) {
                        EmitError(element->location, "element of a constant array must be a literal or a "
                                                     "named constant");
                        break;
                    }
                }
            }
            else if (repeat && !IsConstArrayElement(*repeat->value)) {
                EmitError(repeat->value->location, "element of a constant array must be a literal or a named constant");
            }
        }
        if (Symbol *sym = currentScope->Lookup(d.name)) {
            sym->type = constType;
        }
    }

    static std::string JoinPathSegments(const std::vector<std::string> &path, std::size_t first,
                                        std::size_t lastExclusive) {
        std::string result;
        for (std::size_t i = first; i < lastExclusive; ++i) {
            if (!result.empty()) {
                result += "::";
            }
            result += path[i];
        }
        return result;
    }

    static std::string ModulePathForImport(const UseDecl &d) {
        if (d.path.size() <= 1) {
            return "";
        }
        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() <= 2) {
                return "";
            }
            return JoinPathSegments(d.path, 1, d.path.size() - 1);
        }
        return JoinPathSegments(d.path, 1, d.path.size());
    }

    static std::string LogicalModulePathForImport(const UseDecl &d) {
        if (d.kind == UseDecl::Kind::Single) {
            if (d.path.size() <= 1) {
                return "";
            }
            return JoinPathSegments(d.path, 0, d.path.size() - 1);
        }
        return JoinPathSegments(d.path, 0, d.path.size());
    }

    struct ImportScope {
        const std::unordered_map<std::string, Symbol> *table = nullptr;
        std::string displayName;
        std::string ownerPackage;
        std::string modulePath;
    };

    static std::string ImportScopeDisplayName(const std::string &pkgName, const std::string &modulePath) {
        if (modulePath.empty()) {
            return std::format("package '{}'", pkgName);
        }
        return std::format("module '{}'", modulePath);
    }

    ImportScope ResolveImportScope(const UseDecl &d, const std::string &pkgName, const std::string &modulePath) {
        const std::string logicalModulePath = LogicalModulePathForImport(d);
        if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
            if (auto modIt = pkgIt->second.find(modulePath); modIt != pkgIt->second.end()) {
                return {&modIt->second->Table(), ImportScopeDisplayName(pkgName, modulePath), pkgName, modulePath};
            }
        }

        std::vector<std::pair<std::string, Scope *>> matches;
        for (const auto &[candidatePackage, moduleScopes] : packageModuleScopes) {
            auto modIt = moduleScopes.find(logicalModulePath);
            if (modIt == moduleScopes.end()) {
                continue;
            }
            if (std::ranges::none_of(matches, [&](const auto &match) { return match.second == modIt->second; })) {
                matches.emplace_back(candidatePackage, modIt->second);
            }
        }

        std::ranges::sort(matches, {}, &std::pair<std::string, Scope *>::first);
        if (matches.size() > 1) {
            std::vector<std::string> notes;
            for (const auto &[candidatePackage, _] : matches) {
                notes.push_back(
                    std::format("module '{}' is available from package '{}'", logicalModulePath, candidatePackage));
            }
            EmitError(d.location, std::format("module '{}' is ambiguous", logicalModulePath), std::move(notes),
                      std::format("qualify the import with one of the listed package names"));
            return {};
        }
        if (!matches.empty()) {
            return {&matches[0].second->Table(), ImportScopeDisplayName(matches[0].first, logicalModulePath),
                    matches[0].first, logicalModulePath};
        }

        if (!packageModuleScopes.contains(pkgName)) {
            EmitError(d.location, std::format("package or module '{}' is not defined", pkgName));
        }
        else {
            EmitError(d.location, std::format("module '{}' was not found in package '{}'", modulePath, pkgName));
        }
        return {};
    }

    [[nodiscard]] const Symbol *InaccessibleModule(const std::string &package, const std::string &path) const {
        if (package == currentPackage || path.empty()) {
            return nullptr;
        }
        const auto packageIt = packageModuleScopes.find(package);
        if (packageIt == packageModuleScopes.end()) {
            return nullptr;
        }
        const auto rootIt = packageIt->second.find("");
        if (rootIt == packageIt->second.end()) {
            return nullptr;
        }
        const Scope *scope = rootIt->second;
        std::size_t begin = 0;
        while (begin < path.size()) {
            const std::size_t separator = path.find("::", begin);
            const std::string segment =
                path.substr(begin, separator == std::string::npos ? std::string::npos : separator - begin);
            const auto found = scope->Table().find(segment);
            if (found == scope->Table().end() || found->second.kind != Symbol::Kind::Module) {
                return nullptr;
            }
            if (!IsAccessible(found->second)) {
                return &found->second;
            }
            scope = found->second.moduleScope;
            if (!scope || separator == std::string::npos) {
                break;
            }
            begin = separator + 2;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<Symbol> AccessibleImport(const Symbol &symbol) const {
        if (symbol.kind != Symbol::Kind::Func || symbol.funcOverloads.empty()) {
            return IsAccessible(symbol) ? std::optional<Symbol>(symbol) : std::nullopt;
        }
        Symbol accessible = symbol;
        std::erase_if(accessible.funcOverloads, [this](const FuncDecl *overload) { return !IsAccessible(*overload); });
        if (accessible.funcOverloads.empty()) {
            return std::nullopt;
        }
        accessible.isPublic = true;
        accessible.isEffectivelyPublic = true;
        return accessible;
    }

    void PromoteFromPackage(const UseDecl &d, const std::string &pkgName, const std::string &name) {
        const std::string modulePath = ModulePathForImport(d);
        ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
        if (!scope.table) {
            return;
        }
        if (const Symbol *module = InaccessibleModule(scope.ownerPackage, scope.modulePath)) {
            EmitPrivacyError(d.location, *module);
            return;
        }
        auto sym_it = scope.table->find(name);
        if (sym_it == scope.table->end()) {
            std::string message = std::format("name '{}' was not found in {}", name, scope.displayName);
            std::optional<std::string> help;
            // The item is not at this path, but if one of the package's modules
            // holds it, point at the fully-qualified import.
            if (auto pkgIt = packageModuleScopes.find(pkgName); pkgIt != packageModuleScopes.end()) {
                for (const auto &[candidateModule, candidateScope] : pkgIt->second) {
                    if (!candidateModule.empty() && candidateScope->Table().contains(name)) {
                        help = std::format("did you mean 'import {}::{}::{}'?", pkgName, candidateModule, name);
                        break;
                    }
                }
            }
            EmitError(d.location, std::move(message), {}, std::move(help));
            return;
        }
        const std::optional<Symbol> accessible = AccessibleImport(sym_it->second);
        if (!accessible) {
            EmitPrivacyError(d.location, sym_it->second);
            return;
        }
        DefineImportedSymbol(*accessible);
        ImportSignatureDependencies(*accessible, *scope.table);
    }

    void DefineImportedSymbol(const Symbol &sym) {
        if (Symbol *existing = currentScope->LookupLocal(sym.name)) {
            if (existing->kind == sym.kind && existing->location.line == sym.location.line &&
                existing->location.column == sym.location.column) {
                *existing = sym;
                return;
            }
        }
        currentScope->Define(sym, diags, currentFile);
    }

    void ImportSignatureDependencies(const Symbol &sym, const std::unordered_map<std::string, Symbol> &sourceTable) {
        if (sym.kind != Symbol::Kind::Func) {
            return;
        }

        auto findPackageType = [&](const std::string &name) -> const Symbol * {
            auto sameSymbol = [](const Symbol &lhs, const Symbol &rhs) {
                return lhs.kind == rhs.kind && lhs.name == rhs.name && lhs.location.line == rhs.location.line &&
                       lhs.location.column == rhs.location.column;
            };

            const Symbol *matched = nullptr;
            for (const auto &[_, moduleScopes] : packageModuleScopes) {
                for (const auto &[__, scope] : moduleScopes) {
                    const auto &table = scope->Table();
                    auto it = table.find(name);
                    if (it == table.end()) {
                        continue;
                    }
                    if (it->second.kind != Symbol::Kind::Type && it->second.kind != Symbol::Kind::Interface) {
                        continue;
                    }
                    if (matched && !sameSymbol(*matched, it->second)) {
                        return nullptr;
                    }
                    matched = &it->second;
                }
            }
            return matched;
        };

        auto importNamedType = [&](const std::string &name) {
            if (currentScope->Lookup(name)) {
                return;
            }
            auto depIt = sourceTable.find(name);
            const Symbol *dep = depIt == sourceTable.end() ? findPackageType(name) : &depIt->second;
            if (!dep) {
                return;
            }
            if (dep->kind == Symbol::Kind::Type || dep->kind == Symbol::Kind::Interface) {
                if (const std::optional<Symbol> accessible = AccessibleImport(*dep)) {
                    DefineImportedSymbol(*accessible);
                }
            }
        };

        auto visitType = [&](this auto &&self, const TypeExpr &type) -> void {
            if (const auto *named = dynamic_cast<const NamedTypeExpr *>(&type)) {
                importNamedType(named->name);
                for (const auto &arg : named->typeArgs) {
                    self(*arg);
                }
            }
            else if (const auto *ptr = dynamic_cast<const PointerTypeExpr *>(&type)) {
                self(*ptr->pointee);
            }
            else if (const auto *reference = dynamic_cast<const ReferenceTypeExpr *>(&type)) {
                self(*reference->pointee);
            }
            else if (const auto *slice = dynamic_cast<const ArrayTypeExpr *>(&type)) {
                self(*slice->element);
            }
            else if (const auto *tuple = dynamic_cast<const TupleTypeExpr *>(&type)) {
                for (const auto &elem : tuple->elements) {
                    self(*elem);
                }
            }
            else if (const auto *fn = dynamic_cast<const FunctionTypeExpr *>(&type)) {
                for (const auto &param : fn->params) {
                    self(*param);
                }
                if (fn->returnType) {
                    self(**fn->returnType);
                }
            }
        };

        for (const auto *overload : sym.funcOverloads) {
            for (const auto &param : overload->params) {
                visitType(*param.type);
            }
            if (overload->returnType) {
                visitType(**overload->returnType);
            }
        }
    }

    void CheckUseDecl(const UseDecl &d) {
        if (d.path.empty()) {
            EmitError(d.location, "empty import path");
            return;
        }
        const std::string &pkgName = d.path[0];

        if (d.kind == UseDecl::Kind::Single) {
            // Bind `packageModuleScopes[pkgName][moduleName]` as a module alias
            // usable through `::`. Returns true when the module exists.
            auto bindModuleAlias = [&](const std::string &moduleName) -> bool {
                auto pkgIt = packageModuleScopes.find(pkgName);
                if (pkgIt == packageModuleScopes.end()) {
                    return false;
                }
                auto modIt = pkgIt->second.find(moduleName);
                if (modIt == pkgIt->second.end()) {
                    return false;
                }
                const auto root = pkgIt->second.find("");
                if (root == pkgIt->second.end()) {
                    return false;
                }
                const auto module = root->second->Table().find(moduleName);
                if (module == root->second->Table().end() || module->second.kind != Symbol::Kind::Module) {
                    return false;
                }
                if (!IsAccessible(module->second)) {
                    EmitPrivacyError(d.location, module->second);
                    return true;
                }
                DefineImportedSymbol(module->second);
                return true;
            };

            // Bare `import Pkg;` binds the package's eponymous module as a
            // namespace, so its members are reached through `Pkg::Name`.
            if (d.path.size() < 2) {
                if (bindModuleAlias(pkgName)) {
                    return;
                }
                EmitError(d.location, std::format("import '{}' does not name a module", pkgName), {},
                          std::format("import an item instead, for example 'import {}::Name'", pkgName));
                return;
            }
            const std::string &name = d.path.back();
            // If path.size()==2 and name matches a logical module, create a
            // module alias.
            if (d.path.size() == 2 && bindModuleAlias(name)) {
                return;
            }
            PromoteFromPackage(d, pkgName, name);
        }
        else if (d.kind == UseDecl::Kind::Multi) {
            for (const auto &name : d.names) {
                PromoteFromPackage(d, pkgName, name);
            }
        }
        else // Glob: promote all from the specific module (or all modules
        // if Pkg::*)
        {
            const std::string modulePath = ModulePathForImport(d);
            ImportScope scope = ResolveImportScope(d, pkgName, modulePath);
            if (!scope.table) {
                return;
            }
            if (const Symbol *module = InaccessibleModule(scope.ownerPackage, scope.modulePath)) {
                EmitPrivacyError(d.location, *module);
                return;
            }
            for (const auto &[name, sym] : *scope.table) {
                if (const std::optional<Symbol> accessible = AccessibleImport(sym)) {
                    DefineImportedSymbol(*accessible);
                }
            }
        }
    }

    static std::string MangleTypeName(const TypeRef &type) {
        std::string out;
        for (const char c : type.ToString()) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                out += c;
            }
            else {
                out += '_';
            }
        }
        return out.empty() ? "_" : out;
    }

    TypeRef SubstituteIdentityType(TypeRef type, const std::unordered_map<std::string, TypeRef> &substitutions) const {
        if (type.kind == TypeRef::Kind::TypeParam) {
            if (const auto substitution = substitutions.find(type.name); substitution != substitutions.end()) {
                return substitution->second;
            }
        }
        for (auto &inner : type.inner) {
            inner = SubstituteIdentityType(std::move(inner), substitutions);
        }
        return type;
    }

    TypeRef IdentityParameterType(const Param &parameter,
                                  const std::unordered_map<std::string, TypeRef> &substitutions = {}) const {
        TypeRef type = TypeRef::MakeUnknown();
        if (const auto resolved = typeNodeTypes.find(parameter.type.get()); resolved != typeNodeTypes.end()) {
            type = SubstituteIdentityType(resolved->second, substitutions);
        }
        if (parameter.isVariadic) {
            type = TypeRef::MakeNamed(SliceTypeName(type));
        }
        return type;
    }

    std::string MangleFunctionWithParams(const FuncDecl &declaration) const {
        std::string name = declaration.name + "__";
        for (std::size_t i = 0; i < declaration.params.size(); ++i) {
            if (i != 0) {
                name += "_";
            }
            name += MangleTypeName(IdentityParameterType(declaration.params[i]));
        }
        return name;
    }

    bool FunctionIsOverloadedInModule(const FuncDecl &declaration) const {
        const auto functions = functionsByName.find(declaration.name);
        if (functions == functionsByName.end()) {
            return false;
        }
        const std::string &modulePath = functionModulePaths.at(&declaration);
        std::size_t count = 0;
        for (const auto *candidate : functions->second) {
            if (functionModulePaths.at(candidate) == modulePath && ++count > 1) {
                return true;
            }
        }
        return false;
    }

    bool MethodIsOverloadedForIdentity(const std::string &typeName, const std::string &methodName) const {
        const auto type = methodsByType.find(typeName);
        if (type == methodsByType.end()) {
            return false;
        }
        const auto method = type->second.find(methodName);
        return method != type->second.end() && method->second.size() > 1;
    }

    std::string MethodLinkerName(const FuncDecl &method, const TypeRef &receiverType,
                                 const std::unordered_map<std::string, TypeRef> &substitutions) const {
        const std::string typeName = NamedBaseTypeName(receiverType);
        std::string name = typeName + "::" + method.name;
        if (MethodIsOverloadedForIdentity(typeName, method.name)) {
            name += "__";
            bool first = true;
            for (const auto &parameter : method.params) {
                if (parameter.name == "self" || parameter.isVariadic) {
                    continue;
                }
                if (!first) {
                    name += "_";
                }
                name += MangleTypeName(IdentityParameterType(parameter, substitutions));
                first = false;
            }
        }

        const auto implementation = methodImpls.find(&method);
        if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
            return name;
        }
        if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
            for (const auto &parameter : *typeParams) {
                if (const auto substitution = substitutions.find(parameter.name); substitution != substitutions.end()) {
                    name += "_" + MangleTypeName(substitution->second);
                }
            }
        }
        return name;
    }

    void RecordMethodIdentityRecipe(ResolvedCallableBinding &binding, const FuncDecl &method) const {
        assert(binding.receiverType && "method binding is missing its receiver type");
        const std::string typeName = NamedBaseTypeName(*binding.receiverType);
        binding.linkerNameBase = typeName + "::" + method.name;
        binding.linkerNameHasOverloadSignature = MethodIsOverloadedForIdentity(typeName, method.name);
        if (binding.linkerNameHasOverloadSignature) {
            for (const auto &parameter : method.params) {
                if (parameter.name != "self" && !parameter.isVariadic) {
                    binding.linkerOverloadTypes.push_back(IdentityParameterType(parameter, binding.substitutions));
                }
            }
        }

        const auto implementation = methodImpls.find(&method);
        if (implementation == methodImpls.end() || ImplTypeParams(*implementation->second).empty()) {
            return;
        }
        if (const std::vector<TypeParameter> *typeParams = AggregateTypeParams(typeName)) {
            binding.linkerSpecializationParameters = TypeParameterNames(*typeParams);
        }
    }

    void BuildFinalSymbolIdentities() override {
        std::unordered_map<std::string, std::unordered_set<std::string>> owners;
        for (const auto &[name, declarations] : functionsByName) {
            (void)name;
            for (const auto *declaration : declarations) {
                const std::string local = FunctionIsOverloadedInModule(*declaration)
                                            ? MangleFunctionWithParams(*declaration)
                                            : declaration->name;
                owners[local].insert(functionModulePaths.at(declaration));
            }
        }

        const auto localFunctionName = [&](const FuncDecl &declaration) {
            return FunctionIsOverloadedInModule(declaration) ? MangleFunctionWithParams(declaration) : declaration.name;
        };
        const auto qualifyFunctionName = [&](const FuncDecl &declaration, std::string name) {
            const std::string local = localFunctionName(declaration);
            const std::string &modulePath = functionModulePaths.at(&declaration);
            if (owners[local].size() > 1 && !modulePath.empty()) {
                return modulePath + "::" + name;
            }
            return name;
        };

        for (const auto &[name, declarations] : functionsByName) {
            (void)name;
            for (const auto *declaration : declarations) {
                std::string local = qualifyFunctionName(*declaration, localFunctionName(*declaration));
                symbolIdentities.insert_or_assign(declaration, ResolvedSymbolIdentity{std::move(local)});
            }
        }

        for (const auto *declaration : externFuncDecls) {
            symbolIdentities.insert_or_assign(
                declaration,
                ResolvedSymbolIdentity{declaration->symbolName.empty() ? declaration->name : declaration->symbolName});
        }

        for (const auto *implementation : implDecls) {
            const std::string typeName = implementation->typeName.starts_with("Slice<")
                                           ? implementation->typeName
                                           : BaseTypeName(implementation->typeName);
            if (ImplTypeParams(*implementation).empty()) {
                const TypeRef receiverType = TypeRef::MakeNamed(typeName);
                for (const auto &method : implementation->methods) {
                    symbolIdentities.insert_or_assign(
                        method.get(), ResolvedSymbolIdentity{MethodLinkerName(*method, receiverType, {})});
                }
            }
            if (!implementation->interfaceName || !ImplTypeParams(*implementation).empty()) {
                continue;
            }
            const auto interface = interfaceDecls.find(*implementation->interfaceName);
            if (interface == interfaceDecls.end() || interface->second->methods.empty()) {
                continue;
            }
            ResolvedVtableIdentity identity;
            identity.linkerName = "__vtable__" + typeName + "__" + *implementation->interfaceName;
            for (const auto &method : interface->second->methods) {
                identity.entries.push_back(typeName + "::" + method->name);
            }
            vtableIdentities.insert_or_assign(implementation, std::move(identity));
        }

        for (auto &[call, binding] : callableBindings) {
            (void)call;
            if (!binding.selectedDeclaration || binding.dispatch == ResolvedCallableBinding::DispatchKind::Interface ||
                binding.dispatch == ResolvedCallableBinding::DispatchKind::Indirect ||
                binding.dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant ||
                // A constrained call has one target per instantiation; lowering names it from the witness instead.
                binding.dispatch == ResolvedCallableBinding::DispatchKind::Constrained) {
                continue;
            }
            const auto *function = dynamic_cast<const FuncDecl *>(binding.selectedDeclaration);
            if (function &&
                (binding.dispatch == ResolvedCallableBinding::DispatchKind::Method ||
                 binding.dispatch == ResolvedCallableBinding::DispatchKind::Constructor) &&
                binding.receiverType) {
                binding.linkerName = MethodLinkerName(*function, *binding.receiverType, binding.substitutions);
                RecordMethodIdentityRecipe(binding, *function);
                continue;
            }
            if (function && !function->typeParams.empty()) {
                // A generic function's monomorphized name is its own name plus its type arguments, which two
                // overloads instantiated at the same argument share. Only the first was ever emitted, and every call
                // bound to it — so a four-argument call reached a three-parameter function and quietly dropped an
                // argument. Overloaded methods already carry their parameter types in the name; free functions now
                // do too, built through the same recipe so a later re-instantiation spells it identically.
                // The source declaration and every concrete instantiation must share the same package-qualified base.
                // Otherwise two dependencies exporting the same generic signature both publish (for example)
                // `Clamp__T_T_T`, and calls would still name an unqualified `Clamp_int_int_int` after fixing only the
                // declaration identity.
                binding.linkerNameBase = qualifyFunctionName(*function, function->name);
                binding.linkerNameHasOverloadSignature = FunctionIsOverloadedInModule(*function);
                if (binding.linkerNameHasOverloadSignature) {
                    for (const auto &parameter : function->params) {
                        if (!parameter.isVariadic) {
                            binding.linkerOverloadTypes.push_back(
                                IdentityParameterType(parameter, binding.substitutions));
                        }
                    }
                }
                binding.linkerSpecializationParameters = TypeParameterNames(function->typeParams);
                binding.linkerName = binding.LinkerNameFor(binding.substitutions);
                continue;
            }
            if (const auto identity = symbolIdentities.find(binding.selectedDeclaration);
                identity != symbolIdentities.end()) {
                binding.linkerName = identity->second.linkerName;
            }
        }
    }

    // Expressions
    TypeRef CheckExpr(const Expr &expr) override {
        const std::size_t diagnosticStart = diags.size();
        TypeRef type = CheckExprImpl(expr);
        if (const auto *call = dynamic_cast<const CallExpr *>(&expr)) {
            const bool hasNewError =
                std::ranges::any_of(diags.begin() + static_cast<std::ptrdiff_t>(diagnosticStart), diags.end(),
                                    [](const SemanticDiagnostic &diagnostic) {
                                        return diagnostic.severity == SemanticDiagnostic::Severity::Error;
                                    });
            if (type.IsUnknown() || hasNewError) {
                callableBindings.erase(call);
            }
        }
        RecordCheckedExpression(expr, type);
        return type;
    }

    TypeRef CheckExprImpl(const Expr &expr) {
        if (const std::optional<TypeRef> basicType = CheckBasicExpression(expr)) {
            return *basicType;
        }

        if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
            Symbol *sym = currentScope->Lookup(e->name);
            if (sym) {
                return ReadTrackedSymbol(*sym, e->location);
            }
            EmitUndefinedName(e->location, e->name);
            return TypeRef::MakeUnknown();
        }

        if (dynamic_cast<const SelfExpr *>(&expr)) {
            if (!inImpl) {
                EmitError(expr.location, "'self' used outside of an extend block");
            }
            return currentSelfType.IsUnknown() ? TypeRef::MakeNamed("self") : currentSelfType;
        }

        if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
            if (e->segments.empty()) {
                return TypeRef::MakeUnknown();
            }
            Symbol *first = currentScope->Lookup(e->segments[0]);
            if (!first) {
                EmitUndefinedName(e->location, e->segments[0]);
                return TypeRef::MakeUnknown();
            }
            if (e->segments.size() >= 2 &&
                (first->kind == Symbol::Kind::Type || first->kind == Symbol::Kind::Interface)) {
                if (first->kind == Symbol::Kind::Type) {
                    if (e->segments.size() == 2) {
                        if (const auto constant = LookupPrimitiveConstant(first->type, e->segments[1], context)) {
                            return constant->type;
                        }
                    }
                    const std::string &variantName = e->segments[1];
                    if (const auto resolved = LookupCase(first->name, variantName)) {
                        const EnumDecl::Variant *variant = resolved->selectedCase;
                        if (e->segments.size() > 2) {
                            EmitError(e->location,
                                      std::format("'{}' is a {} {}, not a module", variantName,
                                                  resolved->form == EnumDecl::Form::Variant ? "variant" : "enum",
                                                  resolved->form == EnumDecl::Form::Variant ? "case" : "enumerator"));
                            return TypeRef::MakeUnknown();
                        }
                        if (!variant->fields.empty() || !variant->namedFields.empty()) {
                            return EnumVariantConstructorType(*resolved->declaration, *variant);
                        }
                        return EnumType(*resolved->declaration);
                    }
                }
                TypeRef receiverType = first->type.IsUnknown() ? TypeRef::MakeNamed(first->name) : first->type;
                const std::string &methodName = e->segments[1];
                const FuncDecl *method = LookupMethod(receiverType, methodName);
                if (!method) {
                    const std::vector<const FuncDecl *> accessible =
                        AccessibleMethodCandidates(receiverType, methodName);
                    if (accessible.empty()) {
                        const auto methods = methodsByType.find(NamedBaseTypeName(receiverType));
                        if (methods != methodsByType.end()) {
                            const auto named = methods->second.find(methodName);
                            if (named != methods->second.end() && !named->second.empty()) {
                                EmitPrivacyError(e->location, *named->second.front(), "associated function",
                                                 methodName);
                                return TypeRef::MakeUnknown();
                            }
                        }
                    }
                    EmitError(e->location,
                              std::format("'{}' not found in extend for type '{}'", methodName, first->name));
                    return TypeRef::MakeUnknown();
                }
                if (e->segments.size() > 2) {
                    EmitError(e->location, std::format("'{}' is a function, not a module", methodName));
                    return TypeRef::MakeUnknown();
                }
                return AssociatedFunctionType(receiverType, *method);
            }
            Symbol *current = first;
            Scope *moduleScope = nullptr;
            for (std::size_t i = 1; i < e->segments.size(); ++i) {
                if (current->kind != Symbol::Kind::Module || !current->moduleScope) {
                    EmitError(
                        e->location,
                        std::format("name '{}' is a {}, not a module", current->name, SymbolKindName(current->kind)),
                        {DeclarationNote(*current)});
                    return TypeRef::MakeUnknown();
                }
                moduleScope = current->moduleScope;
                Symbol *item = moduleScope->LookupLocal(e->segments[i]);
                if (!item) {
                    EmitError(e->location,
                              std::format("'{}' not found in module '{}'", e->segments[i], e->segments[i - 1]));
                    return TypeRef::MakeUnknown();
                }
                if (!IsAccessible(*item)) {
                    EmitPrivacyError(e->location, *item);
                    return TypeRef::MakeUnknown();
                }
                current = item;
            }
            return current->type;
        }

        if (auto *e = dynamic_cast<const TypeQueryExpr *>(&expr)) {
            return CheckTypeQueryExpression(*e);
        }

        if (dynamic_cast<const IntrinsicExpr *>(&expr)) {
            const auto *e = static_cast<const IntrinsicExpr *>(&expr);
            using K = IntrinsicExpr::Kind;
            const bool takesArgument = e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature ||
                                       e->kind == K::Config || e->kind == K::HasConfig;
            if (takesArgument) {
                if (e->args.size() != 1 || !e->args[0]) {
                    EmitError(e->location, "compile-time intrinsic expects exactly one argument");
                }
                else if (e->kind == K::TargetFeature && dynamic_cast<const EnumShorthandExpr *>(e->args[0].get())) {
                    // `.AVX2` is given its meaning by #target.HasFeature.
                }
                else {
                    const TypeRef argType = CheckExpr(*e->args[0]);
                    const std::string stringType = SliceTypeName(TypeRef::MakeChar8());
                    if (!argType.IsUnknown() && !(argType.kind == TypeRef::Kind::Named && argType.name == stringType)) {
                        EmitError(e->args[0]->location, "compile-time intrinsic argument must be a string");
                    }
                }
            }

            if (e->kind == K::Line || e->kind == K::Column || e->kind == K::PointerBits) {
                return TypeRef::MakeUInt();
            }
            if (e->kind == K::BuildTimestamp) {
                return TypeRef::MakeUInt64();
            }
            if (e->kind == K::TargetFeature || e->kind == K::CompilerHasFeature || e->kind == K::HasConfig ||
                e->kind == K::DebugAssertions || e->kind == K::DebugInfo || e->kind == K::IsTest) {
                return TypeRef::MakeBool();
            }
            if (e->kind == K::Os || e->kind == K::Arch || e->kind == K::Abi || e->kind == K::Endian ||
                e->kind == K::DataModel || e->kind == K::ObjectFormat || e->kind == K::BuildMode ||
                e->kind == K::Optimization || e->kind == K::OutputKind) {
                const char *name = e->kind == K::Os           ? "#target.os"
                                 : e->kind == K::Arch         ? "#target.arch"
                                 : e->kind == K::Abi          ? "#target.abi"
                                 : e->kind == K::Endian       ? "#target.endian"
                                 : e->kind == K::DataModel    ? "#target.dataModel"
                                 : e->kind == K::ObjectFormat ? "#target.objectFormat"
                                 : e->kind == K::BuildMode    ? "#build.mode"
                                 : e->kind == K::Optimization ? "#build.optimization"
                                                              : "#build.outputKind";
                EmitError(e->location, std::string("'") + name + "' can only be used in a 'when' condition");
                return TypeRef::MakeUnknown();
            }
            return TypeRef::MakeNamed(SliceTypeName(TypeRef::MakeChar8()));
        }

        if (const auto *e = dynamic_cast<const EnumShorthandExpr *>(&expr)) {
            // The bare `.Variant` shorthand is not part of the language; the
            // variant must always be written out in full.
            EmitError(e->location,
                      std::format("'.{}' must be written in full, as in 'Enum::{}'", e->variant, e->variant));
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
            return CheckTernaryExpression(*e);
        }

        if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
            TypeRef loType = e->lo ? CheckExpr(*e->lo) : TypeRef::MakeUnknown();
            TypeRef hiType = e->hi ? CheckExpr(*e->hi) : TypeRef::MakeUnknown();
            if ((!loType.IsUnknown() && !loType.IsNumeric()) || (!hiType.IsUnknown() && !hiType.IsNumeric())) {
                EmitError(e->location, "range bounds must be numeric");
            }
            if (e->lo && e->hi) {
                const auto start = EvalConstInt(*e->lo);
                const auto end = EvalConstInt(*e->hi);
                if (start && end && *start > *end) {
                    EmitError(e->location, "range start cannot be greater than its end");
                }
            }
            TypeRef elemType = loType.IsUnknown() ? hiType : loType;
            if (e->lo && e->hi && hiType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->lo, hiType)) {
                elemType = hiType;
            }
            else if (e->lo && e->hi && loType.IsInteger() && UnsuffixedIntegerLiteralFits(*e->hi, loType)) {
                elemType = loType;
            }
            if (elemType.IsUnknown()) {
                return TypeRef::MakeRangeFull();
            }
            return TypeRef::MakeRange(elemType, e->lo != nullptr, e->hi != nullptr, e->inclusive);
        }

        if (const auto *e = dynamic_cast<const CallExpr *>(&expr)) {
            return CheckCallExpression(*e);
        }

        if (const std::optional<TypeRef> aggregateType = CheckAggregateExpression(expr)) {
            return *aggregateType;
        }

        if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
            TypeRef operandType = CheckExpr(*e->operand);
            const std::string ifaceName = NamedBaseTypeName(ResolveType(*e->type));
            if (!ifaceName.empty()) {
                Symbol *sym = currentScope->Lookup(ifaceName);
                if (sym && sym->kind == Symbol::Kind::Interface) {
                    EmitError(e->location, std::format("type test 'is {}' is unavailable: interface checks are not "
                                                       "implemented",
                                                       ifaceName));
                }
            }
            return TypeRef::MakeBool();
        }
        if (auto *e = dynamic_cast<const MatchExpr *>(&expr)) {
            return CheckMatchExpression(*e);
        }

        if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
            CheckBlock(*e->block);
            return TypeRef::MakeUnknown();
        }

        if (auto *e = dynamic_cast<const SpreadExpr *>(&expr)) {
            return CheckExpr(*e->operand);
        }

        return TypeRef::MakeUnknown();
    }

    TypeRef LiteralType(const Token &tok) const override {
        switch (tok.kind) {
        case TokenKind::IntLiteral:
        case TokenKind::FloatLiteral:
            return SuffixedLiteralType(tok);
        case TokenKind::StringLiteral:
            return StringLiteralType(tok);
        case TokenKind::CharLiteral:
            return CharLiteralType(tok);
        case TokenKind::BoolLiteral:
            return TypeRef::MakeBool();
        default:
            return TypeRef::MakeUnknown();
        }
    }

    /// Reject a constant that the target character width cannot hold.
    ///
    /// The width's own rule decides: a code unit accepts everything that fits in it, a scalar value stops at U+10FFFF
    /// and refuses the surrogates. Every character width is covered, so a width gains this check by joining the
    /// catalog rather than by being listed here.
    void ValidateCastConstant(const CastExpr &expression, const TypeRef &operandType,
                              const TypeRef &targetType) const override {
        const auto maximum = MaxCharacterValue(targetType.kind);
        if (!maximum || !(operandType.IsInteger() || operandType.IsChar())) {
            return;
        }
        const auto outOfRange = [&] {
            EmitError(expression.location,
                      std::format("constant cast from '{}' to '{}' is outside the target type's range",
                                  operandType.ToString(), targetType.ToString()));
        };
        if (const auto value = EvalConstInt(*expression.operand); value && *value < 0) {
            outOfRange();
            return;
        }
        const auto charValue = EvalConstCharCastValue(*expression.operand);
        if (!charValue) {
            return;
        }
        if (*charValue > *maximum) {
            outOfRange();
        }
        else if (!IsValidCharacterValue(targetType.kind, *charValue)) {
            EmitError(expression.location,
                      std::format("cast from '{}' to '{}' uses invalid surrogate code point U+{:04X}",
                                  operandType.ToString(), targetType.ToString(), *charValue));
        }
    }

    /// Counts a type's structural nodes, stopping once the limit is passed so a runaway type costs no more to
    /// measure than a well-behaved one.
    static std::size_t TypeNodeCount(const TypeRef &type, const std::size_t limit) {
        std::size_t count = 1;
        for (const TypeRef &inner : type.inner) {
            if (count > limit) {
                return count;
            }
            count += TypeNodeCount(inner, limit - std::min(count, limit));
        }
        return count;
    }

    /// A generic that instantiates itself at a strictly larger type argument -- `Grow<T>` calling `Grow<*T>` -- never
    /// closes its set of instantiations. Deduplication does not help, because every instantiation is genuinely new.
    /// The compiler used to follow it until the recursive walks over an ever-deeper type exhausted the stack and it
    /// died with no diagnostic at all.
    ///
    /// Bounding the type argument's size catches it: any infinite set of instantiations must produce ever-larger type
    /// arguments, since there are only finitely many small types. Size rather than nesting depth, so a set that grows
    /// in breadth is caught too. The limit is far above anything written by hand -- `Slice<char8>` is two nodes.
    static constexpr std::size_t kMaxInstantiationTypeNodes = 128;

    void QueueGenericInstantiation(const FuncDecl &decl,
                                   const std::unordered_map<std::string, TypeRef> &substitutions) override {
        if (substitutions.empty()) {
            return;
        }
        // A generic function carries its own parameters; a method of a generic type carries none of its own and is
        // instantiated by what the receiver's type arguments say. Both are instantiations with a body to re-check,
        // so the concreteness test reads the substitution map rather than the declaration.
        if (!decl.typeParams.empty() && substitutions.size() != decl.typeParams.size()) {
            return;
        }

        const bool isConcrete = std::ranges::all_of(substitutions, [this](const auto &substitution) {
            return !substitution.second.IsUnknown() && !MentionsTypeParameter(substitution.second);
        });
        if (!isConcrete) {
            if (currentFunctionDecl) {
                deferredGenericCalls[currentFunctionDecl].push_back({&decl, substitutions});
            }
            return;
        }
        pendingGenericInstantiations.push_back({&decl, substitutions});
    }

    void QueueDropMethodInstantiations() override {
        const auto queue = [&](const TypeRef &type) {
            if (type.IsUnknown() || MentionsTypeParameter(type) || !ClassifyTypeProperties(type).IsDroppable()) {
                return;
            }
            const std::string destructorName = "~" + NamedBaseTypeName(type);
            const FuncDecl *destructor = LookupMethod(type, destructorName, {}, false);
            if (destructor == nullptr) {
                return;
            }
            QueueGenericInstantiation(*destructor, MethodTypeSubstitutions(type));
        };
        for (const auto &[expression, type] : expressionTypes) {
            queue(type);
        }
        for (const auto &[node, type] : typeNodeTypes) {
            queue(type);
        }
        for (const auto &[pattern, type] : patternTypes) {
            queue(type);
        }
    }

    void ValidatePendingGenericInstantiations() override {
        std::size_t processed = 0;
        while (processed < pendingGenericInstantiations.size()) {
            PendingGenericInstantiation instantiation = std::move(pendingGenericInstantiations[processed++]);

            // Keyed by what the parameters stand for, taken from the substitution map in a fixed order so a method
            // of a generic type -- which has no parameters of its own to walk -- is deduplicated too.
            std::vector<std::string> names;
            names.reserve(instantiation.substitutions.size());
            for (const auto &[name, type] : instantiation.substitutions) {
                names.push_back(name);
            }
            std::ranges::sort(names);
            std::string key;
            for (const std::string &name : names) {
                if (!key.empty()) {
                    key += ";";
                }
                key += name + "=" + instantiation.substitutions.at(name).ToString();
            }
            if (!validatedGenericInstantiations[instantiation.decl].insert(std::move(key)).second) {
                continue;
            }

            // Refusing to process this instantiation is what stops the runaway: its body is never walked, so it never
            // queues the next, larger one. Reported once per function, since every instantiation past the limit has
            // the same cause and listing them would bury it.
            const auto oversized = std::ranges::find_if(instantiation.substitutions, [](const auto &substitution) {
                return TypeNodeCount(substitution.second, kMaxInstantiationTypeNodes) > kMaxInstantiationTypeNodes;
            });
            if (oversized != instantiation.substitutions.end()) {
                if (reportedRunawayInstantiations.insert(instantiation.decl).second) {
                    // The offending type is by definition enormous, and printing all of it would bury the message
                    // it is meant to illustrate.
                    constexpr std::size_t kShownTypeCharacters = 40;
                    std::string shown = oversized->second.ToString();
                    if (shown.size() > kShownTypeCharacters) {
                        shown = shown.substr(0, kShownTypeCharacters) + "...";
                    }
                    EmitError(
                        instantiation.decl->location,
                        std::format("generic function '{}' instantiates itself without end", instantiation.decl->name),
                        {std::format("type argument '{}' for '{}' grew past the limit of {} type nodes", shown,
                                     oversized->first, kMaxInstantiationTypeNodes)},
                        "give the recursion a case that stops, or one that reuses a type argument it has "
                        "already been given");
                }
                continue;
            }

            Scope *savedScope = currentScope;
            const std::string savedFile = currentFile;
            const std::string savedPackage = currentPackage;
            const FuncDecl *savedFunctionDecl = currentFunctionDecl;
            if (const auto it = functionDeclScopes.find(instantiation.decl); it != functionDeclScopes.end()) {
                currentScope = it->second;
            }
            if (const auto it = functionDeclFiles.find(instantiation.decl); it != functionDeclFiles.end()) {
                currentFile = it->second;
            }
            if (const auto it = declarationInfos.find(instantiation.decl); it != declarationInfos.end()) {
                currentPackage = it->second.ownerPackage;
            }
            currentFunctionDecl = nullptr;

            // A type argument that is itself an instantiation -- `AllocateBlock<Node<int32>>` -- is named for the
            // first time right here. Nothing in the source spells it, so nothing computed its layout, and the
            // `sizeof` this instantiation folds would have had nothing to read. Laying it out now is what puts it
            // in the model that lowering asks.
            for (const auto &substitution : instantiation.substitutions) {
                LayoutOfTypeRef(substitution.second);
            }

            if (const auto it = deferredEnumInstantiations.find(instantiation.decl);
                it != deferredEnumInstantiations.end()) {
                for (const DeferredEnumInstantiation &deferred : it->second) {
                    std::vector<TypeRef> arguments;
                    arguments.reserve(deferred.typeArgs.size());
                    for (const TypeRef &argument : deferred.typeArgs) {
                        arguments.push_back(SubstituteTypeParameters(argument, instantiation.substitutions));
                    }
                    instantiatedTypes.push_back(EnumType(*deferred.decl, arguments));
                }
            }

            ValidateDeferredBasicExpressionChecks(*instantiation.decl, instantiation.substitutions);
            if (const auto it = deferredGenericCalls.find(instantiation.decl); it != deferredGenericCalls.end()) {
                for (const DeferredGenericCall &call : it->second) {
                    std::unordered_map<std::string, TypeRef> substitutions;
                    for (const auto &[param, type] : call.substitutions) {
                        substitutions.emplace(param, SubstituteTypeParameters(type, instantiation.substitutions));
                    }
                    QueueGenericInstantiation(*call.callee, substitutions);
                }
            }

            currentFunctionDecl = savedFunctionDecl;
            currentPackage = savedPackage;
            currentFile = savedFile;
            currentScope = savedScope;
        }
    }
};

// Sema public API
SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, CompileTimeContext inputContext)
    : modules(std::move(userModules))
    , deps(std::move(inputDeps))
    , packageName(std::move(inputPackageName))
    , compileTimeContext(std::move(inputContext)) {
}

SemanticAnalyzer::SemanticAnalyzer(std::vector<Module *> userModules, std::vector<DepPackage> inputDeps,
                                   std::string inputPackageName, std::string inputTargetSystem)
    : SemanticAnalyzer(std::move(userModules), std::move(inputDeps), std::move(inputPackageName),
                       CompileTimeContext{}) {
    if (inputTargetSystem == "FreeBSD")
        compileTimeContext.target.os = Target::OS::FreeBSD;
    else if (inputTargetSystem == "Linux")
        compileTimeContext.target.os = Target::OS::Linux;
    else if (inputTargetSystem == "macOS" || inputTargetSystem == "MacOS")
        compileTimeContext.target.os = Target::OS::MacOS;
    else if (inputTargetSystem == "Windows")
        compileTimeContext.target.os = Target::OS::Windows;
    compileTimeContext.target.object_format = Target::GetObjectFormat(compileTimeContext.target.os);
}

SemanticModel SemanticAnalyzer::Analyze() {
    // Fold `when` first: the branches that were not taken are dropped here, so
    // nothing below ever sees — or type-checks — them. Each package resolves its
    // own conditionals against its own constants.
    for (auto &dep : deps) {
        std::vector<Module *> depModules;
        depModules.reserve(dep.modules.size());
        for (const auto &entry : dep.modules) {
            depModules.push_back(entry.module);
        }
        ResolveConditionalCompilation(depModules, compileTimeContext, diags);
    }
    ResolveConditionalCompilation(modules, compileTimeContext, diags);

    std::vector<const Module *> constModules(modules.begin(), modules.end());
    std::unordered_map<const Expr *, TypeRef> expressionTypes;
    std::unordered_map<const TypeExpr *, TypeRef> typeNodeTypes;
    std::unordered_map<const Pattern *, TypeRef> patternTypes;
    std::unordered_map<const EnumPattern *, ResolvedCasePattern> casePatterns;
    std::unordered_map<const BinaryExpr *, ResolvedVariantEquality> variantEqualities;
    std::unordered_map<std::string, VariantEqualityPlan> variantEqualityPlans;
    std::unordered_map<const Expr *, ValueConsumption> valueConsumptions;
    std::unordered_map<const Expr *, ValueCopy> valueCopies;
    std::unordered_map<const CallExpr *, ResolvedCallableBinding> callableBindings;
    std::unordered_map<const LetStmt *, ResolvedDefaultConstructor> defaultConstructors;
    std::unordered_map<const Decl *, ResolvedSymbolIdentity> symbolIdentities;
    std::unordered_map<const ImplDecl *, ResolvedVtableIdentity> vtableIdentities;
    std::unordered_map<std::string, ResolvedTypeLayout> typeLayouts;
    std::unordered_map<const TypeQueryExpr *, std::uint64_t> typeQueryValues;
    SemanticAnalyzerImplementation analyzer(
        constModules, deps, packageName, diags, symbols, compileTimeContext, expressionTypes, typeNodeTypes,
        patternTypes, casePatterns, variantEqualities, variantEqualityPlans, valueConsumptions, valueCopies,
        callableBindings, defaultConstructors, symbolIdentities, vtableIdentities, typeLayouts, typeQueryValues);
    analyzer.Run();
    std::vector<const Module *> orderedModules;
    for (const auto &dep : deps) {
        for (const auto &entry : dep.modules) {
            orderedModules.push_back(entry.module);
        }
    }
    orderedModules.insert(orderedModules.end(), modules.begin(), modules.end());
    return SemanticModel{std::move(diags),
                         std::move(symbols),
                         std::move(orderedModules),
                         std::move(compileTimeContext),
                         std::move(expressionTypes),
                         std::move(typeNodeTypes),
                         std::move(patternTypes),
                         std::move(casePatterns),
                         std::move(variantEqualities),
                         std::move(variantEqualityPlans),
                         std::move(valueConsumptions),
                         std::move(valueCopies),
                         std::move(callableBindings),
                         std::move(defaultConstructors),
                         analyzer.EffectiveVisibilities(),
                         std::move(symbolIdentities),
                         std::move(vtableIdentities),
                         analyzer.TakeConstraintWitnesses(),
                         analyzer.TakePropagations(),
                         analyzer.TakeCoalescings(),
                         analyzer.TakeIndexOperators(),
                         analyzer.TakeIndexAssignments(),
                         analyzer.TakeIterations(),
                         std::move(typeLayouts),
                         analyzer.TakeTypeProperties(),
                         analyzer.TakeDropGluePlans(),
                         std::move(typeQueryValues)};
}
} // namespace Rux
