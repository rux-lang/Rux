#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Target/Layout.h"
#include "Target/Target.h"
#include "Types/PrimitiveCatalog.h"
#include "Types/Type.h"

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

namespace Rux::SemanticDetail {
using Layout::AlignUp;

/// The type a string literal has.
///
/// A literal is text: a string of the encoding the prefix names, whose length is counted in that encoding's
/// code units. The bare form is UTF-8, which is what an unprefixed literal in a UTF-8 source file already is.
TypeRef AnalysisContext::StringLiteralType(const Token &tok) {
    if (tok.text.starts_with("c16\"")) {
        return TypeRef::MakeText(TypeRef::Kind::Char16);
    }
    if (tok.text.starts_with("c32\"")) {
        return TypeRef::MakeText(TypeRef::Kind::Char32);
    }
    return TypeRef::MakeText(TypeRef::Kind::Char8);
}

// The text of a string-literal token, with the surrounding quotes AnalysisContext::and any
// encoding prefix removed AnalysisContext::and the common escapes decoded, for use as a
// human-readable diagnostic message.
std::string AnalysisContext::DecodeStringMessage(const std::string &text) {
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

// `#Error(message)` AnalysisContext::and `#Warn(message)` are compile-time directives: at each
// live call site they emit a diagnostic with their message AnalysisContext::and produce no
// runtime code. A live call is one the `when` fold kept, so a directive in a
// branch that is not taken never fires. The message must be a string literal.
void AnalysisContext::EmitDiagnosticIntrinsic(const std::string &intrinsicName, const CallExpr &call) {
    const bool isError = intrinsicName == "#Error";
    // A call is located at its '(', so the diagnostic points at the callee's '#' instead, where the directive starts.
    const SourceLocation location = call.callee ? call.callee->location : call.location;
    if (call.args.size() != 1 || !call.args[0]) {
        EmitError(location, std::format("'{}' expects exactly one string argument", intrinsicName));
        return;
    }
    const auto *literal = dynamic_cast<const LiteralExpr *>(call.args[0].get());
    if (!literal || literal->token.kind != TokenKind::StringLiteral) {
        EmitError(call.args[0]->location, std::format("'{}' message must be a string literal", intrinsicName));
        return;
    }
    std::string message = DecodeStringMessage(literal->token.text);
    if (isError) {
        EmitError(location, std::move(message));
    }
    else {
        EmitWarning(location, std::move(message));
    }
}

TypeRef AnalysisContext::CharLiteralType(const Token &tok) {
    if (tok.text.starts_with("c8'")) {
        return TypeRef::MakeChar8();
    }
    if (tok.text.starts_with("c16'")) {
        return TypeRef::MakeChar16();
    }
    if (tok.text.starts_with("c32'")) {
        return TypeRef::MakeChar32();
    }
    if (tok.text.starts_with("c64'")) {
        return TypeRef::MakePrimitive(TypeRef::Kind::Char64);
    }
    return TypeRef::MakeChar();
}

std::string AnalysisContext::NumericLiteralSuffix(const std::string_view text) {
    return std::string(NumericLiteralSuffixOf(text));
}

/// The type a suffix names, built from the width AnalysisContext::and signedness the suffix table records rather than
/// from a second list of them here. A literal with no suffix takes the default: `int`, or `float64` when it has a
/// point.
TypeRef AnalysisContext::SuffixedLiteralType(const Token &tok) {
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
        const bool matches =
            primitive.bits == suffix->bits &&
            primitive.category == (suffix->isSigned ? PrimitiveCategory::SignedInt : PrimitiveCategory::UnsignedInt);
        if (matches) {
            return TypeRef::MakePrimitive(primitive.kind);
        }
    }
    return TypeRef::MakeInt();
}

std::optional<std::uint64_t> AnalysisContext::ParseUnsuffixedIntegerLiteral(const Token &tok) {
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

std::optional<std::uint64_t> AnalysisContext::ParseIntegerLiteralValue(const Token &tok) {
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

/// The width AnalysisContext::and signedness `type` is range-checked at, with the target's pointer width filled in for
/// `int` AnalysisContext::and `uint`.
///
/// @return nullopt when `type` is not an integer
std::optional<std::pair<std::uint32_t, bool>> AnalysisContext::IntegerRange(const TypeRef &type) const {
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
std::optional<std::uint64_t> AnalysisContext::UnsignedIntegerMax(const TypeRef &type) const {
    const auto range = IntegerRange(type);
    if (!range || range->second || range->first > 64) {
        return std::nullopt;
    }
    return WideInteger::MaxValue(range->first, false).ToUnsigned();
}

std::optional<std::pair<std::int64_t, std::int64_t>> AnalysisContext::SignedIntegerRange(const TypeRef &type) const {
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
/// The magnitude is decoded at the widest width there is AnalysisContext::and range-checked afterwards, so a literal
/// too large for its target is told apart from one that is not a literal at all, AnalysisContext::and both answers are
/// exact however wide the target is.
bool AnalysisContext::UnsuffixedIntegerLiteralFits(const Expr &expr, const TypeRef &target) const {
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

bool AnalysisContext::IsNullLiteral(const Expr &expr) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
    return literal && literal->token.kind == TokenKind::NullKeyword;
}

bool AnalysisContext::IsUnsuffixedIntegerLiteral(const Expr &expr) {
    const LiteralExpr *literal = dynamic_cast<const LiteralExpr *>(&expr);
    if (!literal) {
        const auto *unary = dynamic_cast<const UnaryExpr *>(&expr);
        if (!unary || unary->op != TokenKind::Minus) {
            return false;
        }
        literal = dynamic_cast<const LiteralExpr *>(unary->operand.get());
    }
    return literal && literal->token.kind == TokenKind::IntLiteral && NumericLiteralSuffix(literal->token.text).empty();
}

bool AnalysisContext::IsIntegerLiteralOutOfRangeFor(const Expr &expr, const TypeRef &targetType) const {
    return targetType.IsInteger() && IsUnsuffixedIntegerLiteral(expr) &&
           !UnsuffixedIntegerLiteralFits(expr, targetType);
}

// Explains why the address of an immutable place cannot initialize a
// writable pointer. The types alone do not point at the required binding
// change, so name it when possible.
std::string AnalysisContext::ImmutableAddressOfHint(const Expr &expr, const TypeRef &targetType) {
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
        return std::format(": '@{0}' yields a read-only '*T'; declare '{0}' with 'var' for a '*var T'", ident->name);
    }
    return {};
}

// An unprefixed character takes a narrow character destination only when it fits one code unit there, so a refusal
// names the character the way the prefixed form's own diagnostic does rather than reporting its default 'char32'.
std::optional<std::string> AnalysisContext::UnfitCharacterLiteralMessage(const Expr &expression,
                                                                         const TypeRef &targetType) {
    const auto *literal = dynamic_cast<const LiteralExpr *>(&expression);
    if (!literal || literal->token.kind != TokenKind::CharLiteral || !literal->token.text.starts_with('\'') ||
        !targetType.IsChar()) {
        return std::nullopt;
    }
    const std::optional<std::uint32_t> codePoint = Lexer::DecodeCharLiteralCodePoint(literal->token.text);
    if (!codePoint || IsOneCharacterOf(targetType.kind, *codePoint)) {
        return std::nullopt;
    }
    const std::string_view text = literal->token.text;
    return std::format("character '{}' (U+{:04X}) does not fit one '{}' code unit", text.substr(1, text.size() - 2),
                       *codePoint, targetType.ToString());
}

// Picks the diagnostic for a rejected assignment/conversion. An
// unsuffixed integer literal that does not fit the target gets a
// dedicated "out of range" message; taking the address of an immutable
// place gets the reason appended; everything else uses `fallback`.
// Keeps the wording consistent across let, return, assignment, const,
// AnalysisContext::and field positions.
std::string AnalysisContext::AssignmentErrorMessage(const Expr &expr, const TypeRef &targetType, std::string fallback) {
    if (IsIntegerLiteralOutOfRangeFor(expr, targetType)) {
        return std::format("integer literal is out of range for type '{}'", targetType.ToString());
    }
    if (std::optional<std::string> message = UnfitCharacterLiteralMessage(expr, targetType)) {
        return std::move(*message);
    }
    // A literal's code units live in the read-only section the linker places them in, so the one view that can name
    // them is a read-only one; the refusal says why rather than reporting a mismatch of writability.
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr);
        literal && literal->token.kind == TokenKind::StringLiteral && targetType.IsWritableSlice()) {
        return std::format("cannot assign string literal to '{}' because literal text is stored in a read-only section",
                           targetType.ToString());
    }
    if (const std::string hint = ImmutableAddressOfHint(expr, targetType); !hint.empty()) {
        return fallback + hint;
    }
    if (dynamic_cast<const NoneExpr *>(&expr)) {
        return std::format("'none' needs an expected optional type, but found '{}'", targetType.ToString());
    }
    if (const auto *construct = dynamic_cast<const NativeConstructExpr *>(&expr)) {
        const std::string_view name = construct->kind == NativeConstructExpr::Kind::Some    ? "Some"
                                    : construct->kind == NativeConstructExpr::Kind::Success ? "Success"
                                                                                            : "Failure";
        const std::string_view form = construct->kind == NativeConstructExpr::Kind::Some ? "optional" : "fallible";
        // A nominal variant with the same case name is constructed by its own name, never by the native constructor.
        if (targetType.kind == TypeRef::Kind::Named) {
            const std::string variantName = BaseTypeName(targetType.name);
            if (const auto declaration = enumDecls.find(NominalTypeName(variantName));
                declaration != enumDecls.end() && declaration->second->IsVariant() &&
                std::ranges::any_of(declaration->second->variants,
                                    [&](const EnumDecl::Variant &variant) { return variant.name == name; })) {
                return std::format("'.{}(...)' constructs a native {}; write '{}::{}(...)' for variant '{}'", name,
                                   form, variantName, name, variantName);
            }
        }
        TypeRef level = targetType;
        while (construct->kind == NativeConstructExpr::Kind::Some ? level.IsFallible() : level.IsOptional()) {
            TypeRef payload = level.inner.front();
            level = std::move(payload);
        }
        if (construct->kind == NativeConstructExpr::Kind::Some ? !level.IsOptional() : !level.IsFallible()) {
            return std::format("'.{}(...)' constructs a native {}, but the expected type is '{}'", name, form,
                               targetType.ToString());
        }
    }
    // Several native routes with different meanings are refused as ambiguous rather than as a plain mismatch, so the
    // message names the choice the source has to make.
    if (const auto sourceType = expressionTypes.find(&expr); sourceType != expressionTypes.end()) {
        if (ClassifyNativeConversion(sourceType->second, targetType).outcome == NativeConversion::Outcome::Ambiguous) {
            return std::format("conversion from '{}' to '{}' is ambiguous; write '.Success(...)', '.Some(...)', or '?' "
                               "to choose one, or annotate an intermediate type",
                               sourceType->second.ToString(), targetType.ToString());
        }
        if (IsUnsuffixedIntegerLiteral(expr) && targetType.IsSum() &&
            std::ranges::count_if(targetType.inner, [](const TypeRef &member) { return member.IsInteger(); }) > 1) {
            return std::format("integer literal is ambiguous for '{}', which has several integer members; add a "
                               "suffix or a cast",
                               targetType.ToString());
        }
    }
    return fallback;
}

namespace {
using FoldedInteger = std::optional<std::int64_t>;

/// Folds the integer operators over the leaves `leaf` accepts, with the two's-complement wrapping the generated code
/// produces at run time, so the folded value always matches what the program computes. Division or modulo by zero,
/// the INT64_MIN / -1 overflow, and out-of-range shifts are left unfolded so they keep their run-time behavior; '**'
/// is not folded, since it lowers to a run-time helper call.
template <typename Leaf>
FoldedInteger FoldIntegerExpression(const Expr &expr, const Leaf &leaf) {
    using I = std::int64_t;
    using U = std::uint64_t;

    if (const auto *un = dynamic_cast<const UnaryExpr *>(&expr)) {
        const auto v = FoldIntegerExpression(*un->operand, leaf);
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
        const auto l = FoldIntegerExpression(*bin->left, leaf);
        const auto r = FoldIntegerExpression(*bin->right, leaf);
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

    return leaf(expr);
}

FoldedInteger WordValue(const std::optional<std::uint64_t> value) {
    if (!value || *value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*value);
}
} // namespace

// Folds a compile-time-constant integer expression (unsuffixed integer literals combined with the integer operators)
// to its int64 value. Returns nullopt when the expression is not such a constant, so callers fall back to ordinary type
// checking.
std::optional<std::int64_t> AnalysisContext::EvalConstInt(const Expr &expr) {
    return FoldIntegerExpression(expr, [](const Expr &leaf) -> FoldedInteger {
        const auto *literal = dynamic_cast<const LiteralExpr *>(&leaf);
        if (!literal || literal->token.kind != TokenKind::IntLiteral ||
            !NumericLiteralSuffix(literal->token.text).empty()) {
            return std::nullopt;
        }
        return WordValue(ParseUnsuffixedIntegerLiteral(literal->token));
    });
}

std::optional<std::int64_t> AnalysisContext::EvalConstInteger(const Expr &expr) const {
    std::unordered_set<const ConstDecl *> active;
    return EvalConstInteger(expr, *currentScope, active);
}

std::optional<std::int64_t> AnalysisContext::EvalConstInteger(const Expr &expr, Scope &scope,
                                                              std::unordered_set<const ConstDecl *> &active) const {
    // A named constant folds its own initializer in the scope that declared it, and its value must fit the type it
    // was declared with. A constant reached again while it is being folded has a cyclic initializer, which checking
    // the declaration reports; here it is simply not a value.
    const auto constant = [&](const Symbol *symbol) -> FoldedInteger {
        if (!symbol || symbol->kind != Symbol::Kind::Const || !symbol->intrinsicName.empty()) {
            return std::nullopt;
        }
        const auto *declaration = dynamic_cast<const ConstDecl *>(symbol->declaration);
        if (!declaration || !declaration->value || !active.insert(declaration).second) {
            return std::nullopt;
        }
        const auto owner = declarationInfos.find(declaration);
        Scope &ownerScope = owner != declarationInfos.end() && owner->second.scope ? *owner->second.scope : scope;
        const FoldedInteger value = EvalConstInteger(*declaration->value, ownerScope, active);
        active.erase(declaration);
        if (!value ||
            (!symbol->type.IsUnknown() && !(symbol->type.IsInteger() && ConstantFitsTarget(*value, symbol->type)))) {
            return std::nullopt;
        }
        return value;
    };
    return FoldIntegerExpression(expr, [&](const Expr &leaf) -> FoldedInteger {
        if (const auto *literal = dynamic_cast<const LiteralExpr *>(&leaf)) {
            return literal->token.kind == TokenKind::IntLiteral ? WordValue(ParseIntegerLiteralValue(literal->token))
                                                                : std::nullopt;
        }
        if (const auto *identifier = dynamic_cast<const IdentExpr *>(&leaf)) {
            return constant(scope.Lookup(identifier->name));
        }
        if (const auto *path = dynamic_cast<const PathExpr *>(&leaf); path && path->segments.size() >= 2) {
            const Symbol *current = scope.Lookup(path->segments[0]);
            for (std::size_t i = 1; current && i < path->segments.size(); ++i) {
                if (current->kind != Symbol::Kind::Module || !current->moduleScope) {
                    return std::nullopt;
                }
                current = current->moduleScope->LookupLocal(path->segments[i]);
                if (current && !IsAccessible(*current)) {
                    return std::nullopt;
                }
            }
            return constant(current);
        }
        if (const auto *cast = dynamic_cast<const CastExpr *>(&leaf)) {
            const auto *named = dynamic_cast<const NamedTypeExpr *>(cast->type.get());
            const auto type = named ? PrimitiveTypeFromName(named->name) : std::nullopt;
            const FoldedInteger value = EvalConstInteger(*cast->operand, scope, active);
            if (!type || !type->IsInteger() || !value || !ConstantFitsTarget(*value, *type)) {
                return std::nullopt;
            }
            return value;
        }
        return std::nullopt;
    });
}

bool AnalysisContext::ConstantFitsTarget(std::int64_t value, const TypeRef &target) const {
    if (const auto max = UnsignedIntegerMax(target)) {
        return value >= 0 && static_cast<std::uint64_t>(value) <= *max;
    }
    if (const auto range = SignedIntegerRange(target)) {
        return value >= range->first && value <= range->second;
    }
    // A folded constant is a machine word, so every value fits a signed target wider than one, and every
    // non-negative value an unsigned one: `let c: uint128 = 7 * 3;`.
    if (const auto range = IntegerRange(target); range && range->first > 64) {
        return range->second || value >= 0;
    }
    return false;
}

std::optional<std::uint64_t> AnalysisContext::EvalConstCharCastValue(const Expr &expr) {
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
} // namespace Rux::SemanticDetail
