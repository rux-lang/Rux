#include "Lexer/Lexer.h"
#include "Numeric/IntegerLiteral.h"
#include "Semantic/Analysis/AnalysisContext.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
#include "Semantic/Model/PrimitiveConstants.h"
#include "Target/Layout.h"
#include "Target/Target.h"
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
    if (tok.text.starts_with("s16\"")) {
        return TypeRef::MakeString16();
    }
    if (tok.text.starts_with("s32\"")) {
        return TypeRef::MakeString32();
    }
    return TypeRef::MakeString8();
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
// zero AnalysisContext::and the INT64_MIN / -1 overflow are left unfolded AnalysisContext::and keep their
// runtime behavior; '**' is not folded (it lowers to a runtime helper
// call).
std::optional<std::int64_t> AnalysisContext::EvalConstInt(const Expr &expr) {
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

bool AnalysisContext::ConstantFitsTarget(std::int64_t value, const TypeRef &target) const {
    if (const auto max = UnsignedIntegerMax(target)) {
        return value >= 0 && static_cast<std::uint64_t>(value) <= *max;
    }
    if (const auto range = SignedIntegerRange(target)) {
        return value >= range->first && value <= range->second;
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
