// Evaluating the expression forms a compile-time condition is built from, and
// selecting the arm of a compile-time match.

#include "Semantic/ConditionalEvaluatorInternal.h"

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Rux {
namespace ConditionalEvaluation {
/// "hello" / c8"hello" -> hello. Only the escapes that can plausibly appear in a `when` comparison are decoded;
/// anything else is kept verbatim.
std::optional<std::string> ParseStringLiteral(const std::string_view text) {
    const auto open = text.find('"');
    if (open == std::string_view::npos || text.back() != '"' || text.size() < open + 2) {
        return std::nullopt;
    }
    const std::string_view body = text.substr(open + 1, text.size() - open - 2);

    std::string value;
    value.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 == body.size()) {
            value.push_back(body[i]);
            continue;
        }
        switch (body[++i]) {
        case 'n':
            value.push_back('\n');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case '0':
            value.push_back('\0');
            break;
        case '\\':
            value.push_back('\\');
            break;
        case '"':
            value.push_back('"');
            break;
        default:
            value.push_back('\\');
            value.push_back(body[i]);
            break;
        }
    }
    return value;
}
} // namespace ConditionalEvaluation

namespace {
std::string_view ValueTypeName(const CompileTimeValue &value) {
    if (std::holds_alternative<bool>(value))
        return "bool";
    if (std::holds_alternative<std::int64_t>(value))
        return "signed integer";
    if (std::holds_alternative<std::uint64_t>(value))
        return "unsigned integer";
    if (std::holds_alternative<double>(value))
        return "float";
    if (std::holds_alternative<std::string>(value))
        return "string";
    return "enum member";
}

std::string_view OperatorName(const TokenKind op) {
    switch (op) {
    case TokenKind::Plus:
        return "+";
    case TokenKind::Minus:
        return "-";
    case TokenKind::Star:
        return "*";
    case TokenKind::Slash:
        return "/";
    case TokenKind::Percent:
        return "%";
    case TokenKind::Amp:
        return "&";
    case TokenKind::Pipe:
        return "|";
    case TokenKind::Caret:
        return "^";
    case TokenKind::AmpAmp:
        return "&&";
    case TokenKind::PipePipe:
        return "||";
    case TokenKind::LessLess:
        return "<<";
    case TokenKind::GreaterGreater:
        return ">>";
    case TokenKind::GreaterGreaterGreater:
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
} // namespace

std::optional<std::uint32_t> ConditionalEvaluator::Impl::SignedIntegerWidth(const Expr &expr) const {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr)) {
        if (literal->token.kind != TokenKind::IntLiteral) {
            return std::nullopt;
        }
        const std::string_view text = literal->token.text;
        if (text.ends_with("u8") || text.ends_with("u16") || text.ends_with("u32") || text.ends_with("u64") ||
            text.ends_with('u')) {
            return std::nullopt;
        }
        if (text.ends_with("i8"))
            return 8;
        if (text.ends_with("i16"))
            return 16;
        if (text.ends_with("i32"))
            return 32;
        if (text.ends_with("i64"))
            return 64;
        return context.target.pointer_size * 8;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
        return SignedIntegerWidth(*unary->operand);
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
        return SignedIntegerWidth(*binary->left);
    }
    if (const auto *ident = dynamic_cast<const IdentExpr *>(&expr)) {
        if (const auto width = constSignedIntegerWidths.find(ident->name); width != constSignedIntegerWidths.end()) {
            return width->second;
        }
        if (const auto value = constExprs.find(ident->name); value != constExprs.end()) {
            return SignedIntegerWidth(*value->second);
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ConditionalEvaluator::Impl::UnsignedIntegerWidth(const Expr &expr) const {
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expr)) {
        if (literal->token.kind != TokenKind::IntLiteral) {
            return std::nullopt;
        }
        const std::string_view text = literal->token.text;
        if (text.ends_with("u8"))
            return 8;
        if (text.ends_with("u16"))
            return 16;
        if (text.ends_with("u32"))
            return 32;
        if (text.ends_with("u64"))
            return 64;
        if (text.ends_with('u'))
            return context.target.pointer_size * 8;
        return std::nullopt;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expr)) {
        return UnsignedIntegerWidth(*unary->operand);
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expr)) {
        return UnsignedIntegerWidth(*binary->left);
    }
    if (const auto *ident = dynamic_cast<const IdentExpr *>(&expr)) {
        if (const auto width = constUnsignedIntegerWidths.find(ident->name);
            width != constUnsignedIntegerWidths.end()) {
            return width->second;
        }
        if (const auto value = constExprs.find(ident->name); value != constExprs.end()) {
            return UnsignedIntegerWidth(*value->second);
        }
    }
    return std::nullopt;
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalConstantReference(const IdentExpr &expr) {
    const auto declaration = constExprs.find(expr.name);
    if (declaration == constExprs.end()) {
        EmitError(expr.location, std::format("'{}' is not a compile-time constant", expr.name));
        reportedError = true;
        return std::nullopt;
    }
    if (!constsInProgress.insert(expr.name).second) {
        EmitError(expr.location, std::format("compile-time constant '{}' depends on itself", expr.name));
        reportedError = true;
        return std::nullopt;
    }
    auto value = Eval(*declaration->second);
    constsInProgress.erase(expr.name);
    if (!value) {
        return std::nullopt;
    }

    if (const auto unsignedWidth = constUnsignedIntegerWidths.find(expr.name);
        unsignedWidth != constUnsignedIntegerWidths.end()) {
        if (const auto *signedValue = std::get_if<std::int64_t>(&*value); signedValue && *signedValue >= 0) {
            value = Value{static_cast<std::uint64_t>(*signedValue)};
        }
        else if (std::holds_alternative<std::int64_t>(*value)) {
            EmitError(expr.location,
                      std::format("compile-time constant '{}' has a negative value for its unsigned type", expr.name));
            reportedError = true;
            return std::nullopt;
        }
        const auto *unsignedValue = std::get_if<std::uint64_t>(&*value);
        const std::uint64_t maximum = unsignedWidth->second == 64 ? std::numeric_limits<std::uint64_t>::max()
                                                                  : (std::uint64_t{1} << unsignedWidth->second) - 1;
        if (unsignedValue && *unsignedValue > maximum) {
            EmitError(expr.location, std::format("compile-time constant '{}' overflows its unsigned {}-bit type",
                                                 expr.name, unsignedWidth->second));
            reportedError = true;
            return std::nullopt;
        }
    }
    else if (const auto signedWidth = constSignedIntegerWidths.find(expr.name);
             signedWidth != constSignedIntegerWidths.end()) {
        if (const auto *unsignedValue = std::get_if<std::uint64_t>(&*value);
            unsignedValue && *unsignedValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            value = Value{static_cast<std::int64_t>(*unsignedValue)};
        }
        else if (std::holds_alternative<std::uint64_t>(*value)) {
            EmitError(expr.location, std::format("compile-time constant '{}' overflows its signed {}-bit type",
                                                 expr.name, signedWidth->second));
            reportedError = true;
            return std::nullopt;
        }
        if (signedWidth->second < 64) {
            const auto *signedValue = std::get_if<std::int64_t>(&*value);
            const std::int64_t minimum = -(std::int64_t{1} << (signedWidth->second - 1));
            const std::int64_t maximum = (std::int64_t{1} << (signedWidth->second - 1)) - 1;
            if (signedValue && (*signedValue < minimum || *signedValue > maximum)) {
                EmitError(expr.location, std::format("compile-time constant '{}' overflows its signed {}-bit type",
                                                     expr.name, signedWidth->second));
                reportedError = true;
                return std::nullopt;
            }
        }
    }
    return value;
}

std::optional<CompileTimeValue> ConditionalEvaluator::Impl::EvalBinary(const BinaryExpr &e) {
    if (const auto leftVersion = EvalSemanticVersion(*e.left)) {
        const auto rightVersion = EvalSemanticVersion(*e.right);
        if (!rightVersion) {
            return std::nullopt;
        }
        const int comparison = CompareSemanticVersions(*leftVersion, *rightVersion);
        switch (e.op) {
        case TokenKind::Equal:
            return Value{comparison == 0};
        case TokenKind::BangEqual:
            return Value{comparison != 0};
        case TokenKind::Less:
            return Value{comparison < 0};
        case TokenKind::LessEqual:
            return Value{comparison <= 0};
        case TokenKind::Greater:
            return Value{comparison > 0};
        case TokenKind::GreaterEqual:
            return Value{comparison >= 0};
        default:
            return std::nullopt;
        }
    }

    const auto left = Eval(*e.left);
    if (!left) {
        return std::nullopt;
    }

    // Short-circuit, so `Debug && DebugLevel > 1` does not require the
    // right-hand side to be evaluable when the left is false.
    if (e.op == TokenKind::AmpAmp || e.op == TokenKind::PipePipe) {
        const auto *lb = std::get_if<bool>(&*left);
        if (!lb) {
            EmitError(
                e.left->location,
                std::format("compile-time operator '{}' requires 'bool' operands, but the left operand has type '{}'",
                            OperatorName(e.op), ValueTypeName(*left)));
            reportedError = true;
            return std::nullopt;
        }
        if (e.op == TokenKind::AmpAmp && !*lb) {
            return Value{false};
        }
        if (e.op == TokenKind::PipePipe && *lb) {
            return Value{true};
        }
        const auto right = Eval(*e.right);
        if (!right) {
            return std::nullopt;
        }
        if (!std::holds_alternative<bool>(*right)) {
            EmitError(
                e.right->location,
                std::format("compile-time operator '{}' requires 'bool' operands, but the right operand has type '{}'",
                            OperatorName(e.op), ValueTypeName(*right)));
            reportedError = true;
            return std::nullopt;
        }
        return right;
    }

    const auto right = Eval(*e.right);
    if (!right) {
        return std::nullopt;
    }

    const bool leftInteger =
        std::holds_alternative<std::int64_t>(*left) || std::holds_alternative<std::uint64_t>(*left);
    const bool rightInteger =
        std::holds_alternative<std::int64_t>(*right) || std::holds_alternative<std::uint64_t>(*right);
    if (leftInteger && rightInteger) {
        const auto equal = [&] {
            if (const auto *l = std::get_if<std::int64_t>(&*left)) {
                if (const auto *r = std::get_if<std::int64_t>(&*right))
                    return *l == *r;
                return *l >= 0 && static_cast<std::uint64_t>(*l) == std::get<std::uint64_t>(*right);
            }
            const auto l = std::get<std::uint64_t>(*left);
            if (const auto *r = std::get_if<std::uint64_t>(&*right))
                return l == *r;
            const auto r = std::get<std::int64_t>(*right);
            return r >= 0 && l == static_cast<std::uint64_t>(r);
        };
        const auto less = [&] {
            if (const auto *l = std::get_if<std::int64_t>(&*left)) {
                if (const auto *r = std::get_if<std::int64_t>(&*right))
                    return *l < *r;
                return *l < 0 || static_cast<std::uint64_t>(*l) < std::get<std::uint64_t>(*right);
            }
            const auto l = std::get<std::uint64_t>(*left);
            if (const auto *r = std::get_if<std::uint64_t>(&*right))
                return l < *r;
            const auto r = std::get<std::int64_t>(*right);
            return r >= 0 && l < static_cast<std::uint64_t>(r);
        };

        switch (e.op) {
        case TokenKind::Equal:
            return Value{equal()};
        case TokenKind::BangEqual:
            return Value{!equal()};
        case TokenKind::Less:
            return Value{less()};
        case TokenKind::LessEqual:
            return Value{less() || equal()};
        case TokenKind::Greater:
            return Value{!less() && !equal()};
        case TokenKind::GreaterEqual:
            return Value{!less()};
        default:
            break;
        }

        if (const auto *l = std::get_if<std::int64_t>(&*left)) {
            const auto *r = std::get_if<std::int64_t>(&*right);
            if (!r) {
                EmitError(e.location, std::format("compile-time operator '{}' cannot combine '{}' and '{}'",
                                                  OperatorName(e.op), ValueTypeName(*left), ValueTypeName(*right)));
                reportedError = true;
                return std::nullopt;
            }
            const auto lu = static_cast<std::uint64_t>(*l);
            auto checkedResult = [&](const std::int64_t result) -> std::optional<Value> {
                if (const auto width = SignedIntegerWidth(*e.left); width && *width < 64) {
                    const std::int64_t minimum = -(std::int64_t{1} << (*width - 1));
                    const std::int64_t maximum = (std::int64_t{1} << (*width - 1)) - 1;
                    if (result < minimum || result > maximum) {
                        EmitError(e.location,
                                  std::format("compile-time evaluation of '{}' overflows a signed {}-bit integer",
                                              OperatorName(e.op), *width));
                        reportedError = true;
                        return std::nullopt;
                    }
                }
                return Value{result};
            };
            switch (e.op) {
            case TokenKind::Plus: {
                std::int64_t result = 0;
                if (__builtin_add_overflow(*l, *r, &result)) {
                    EmitError(e.location, "compile-time evaluation of '+' overflows a signed 64-bit integer");
                    reportedError = true;
                    return std::nullopt;
                }
                return checkedResult(result);
            }
            case TokenKind::Minus: {
                std::int64_t result = 0;
                if (__builtin_sub_overflow(*l, *r, &result)) {
                    EmitError(e.location, "compile-time evaluation of '-' overflows a signed 64-bit integer");
                    reportedError = true;
                    return std::nullopt;
                }
                return checkedResult(result);
            }
            case TokenKind::Star: {
                std::int64_t result = 0;
                if (__builtin_mul_overflow(*l, *r, &result)) {
                    EmitError(e.location, "compile-time evaluation of '*' overflows a signed 64-bit integer");
                    reportedError = true;
                    return std::nullopt;
                }
                return checkedResult(result);
            }
            case TokenKind::Slash:
            case TokenKind::Percent:
                if (*r == 0) {
                    EmitError(e.right->location,
                              std::format("compile-time operator '{}' cannot divide by zero", OperatorName(e.op)));
                    reportedError = true;
                    return std::nullopt;
                }
                if (*l == std::numeric_limits<std::int64_t>::min() && *r == -1) {
                    EmitError(e.location,
                              std::format("compile-time evaluation of '{}' overflows a signed 64-bit integer",
                                          OperatorName(e.op)));
                    reportedError = true;
                    return std::nullopt;
                }
                return Value{e.op == TokenKind::Slash ? *l / *r : *l % *r};
            case TokenKind::Amp:
                return Value{*l & *r};
            case TokenKind::Pipe:
                return Value{*l | *r};
            case TokenKind::Caret:
                return Value{*l ^ *r};
            case TokenKind::LessLess:
            case TokenKind::GreaterGreater:
            case TokenKind::GreaterGreaterGreater:
                if (*r < 0 || *r >= 64) {
                    EmitError(e.right->location,
                              std::format("compile-time shift count {} is outside the valid range 0..63", *r));
                    reportedError = true;
                    return std::nullopt;
                }
                if (e.op == TokenKind::LessLess) {
                    std::int64_t result = 0;
                    const bool overflow = *l < 0 || (*r == 63 && *l != 0) ||
                                          (*r < 63 && __builtin_mul_overflow(*l, std::int64_t{1} << *r, &result));
                    if (overflow) {
                        EmitError(e.location, "compile-time evaluation of '<<' overflows a signed 64-bit integer");
                        reportedError = true;
                        return std::nullopt;
                    }
                    return checkedResult(result);
                }
                if (e.op == TokenKind::GreaterGreater) {
                    return Value{*l >> *r};
                }
                if (const auto width = SignedIntegerWidth(*e.left)) {
                    const std::uint64_t mask =
                        *width == 64 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << *width) - 1;
                    std::uint64_t shifted = (lu & mask) >> static_cast<std::uint64_t>(*r);
                    if (*width < 64 && (shifted & (std::uint64_t{1} << (*width - 1))) != 0) {
                        shifted |= ~mask;
                    }
                    return Value{static_cast<std::int64_t>(shifted)};
                }
                return std::nullopt;
            default:
                return std::nullopt;
            }
        }

        const auto l = std::get<std::uint64_t>(*left);
        const auto *r = std::get_if<std::uint64_t>(&*right);
        if (!r) {
            EmitError(e.location, std::format("compile-time operator '{}' cannot combine '{}' and '{}'",
                                              OperatorName(e.op), ValueTypeName(*left), ValueTypeName(*right)));
            reportedError = true;
            return std::nullopt;
        }
        switch (e.op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star: {
            std::uint64_t result = 0;
            const bool overflow = e.op == TokenKind::Plus  ? __builtin_add_overflow(l, *r, &result)
                                : e.op == TokenKind::Minus ? __builtin_sub_overflow(l, *r, &result)
                                                           : __builtin_mul_overflow(l, *r, &result);
            const auto width = UnsignedIntegerWidth(*e.left);
            const std::uint64_t maximum =
                width && *width < 64 ? (std::uint64_t{1} << *width) - 1 : std::numeric_limits<std::uint64_t>::max();
            if (overflow || result > maximum) {
                EmitError(e.location,
                          std::format("compile-time evaluation of '{}' overflows an unsigned {}-bit integer",
                                      OperatorName(e.op), width.value_or(64)));
                reportedError = true;
                return std::nullopt;
            }
            return Value{result};
        }
        case TokenKind::Slash:
        case TokenKind::Percent:
            if (*r == 0) {
                EmitError(e.right->location,
                          std::format("compile-time operator '{}' cannot divide by zero", OperatorName(e.op)));
                reportedError = true;
                return std::nullopt;
            }
            return Value{e.op == TokenKind::Slash ? l / *r : l % *r};
        case TokenKind::Amp:
            return Value{l & *r};
        case TokenKind::Pipe:
            return Value{l | *r};
        case TokenKind::Caret:
            return Value{l ^ *r};
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
            if (*r >= 64) {
                EmitError(e.right->location,
                          std::format("compile-time shift count {} is outside the valid range 0..63", *r));
                reportedError = true;
                return std::nullopt;
            }
            return Value{e.op == TokenKind::LessLess ? l << *r : l >> *r};
        case TokenKind::GreaterGreaterGreater:
            EmitError(e.location, "compile-time operator '>>>' requires a signed integer left operand");
            reportedError = true;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if (left->index() != right->index()) {
        EmitError(e.location, std::format("compile-time operator '{}' cannot combine '{}' and '{}'", OperatorName(e.op),
                                          ValueTypeName(*left), ValueTypeName(*right)));
        reportedError = true;
        return std::nullopt;
    }

    if (const auto *le = std::get_if<EnumValue>(&*left)) {
        return EvalEnumComparison(e, *le, std::get<EnumValue>(*right));
    }

    if (e.op == TokenKind::Equal) {
        return Value{*left == *right};
    }
    if (e.op == TokenKind::BangEqual) {
        return Value{*left != *right};
    }

    if (const auto *ls = std::get_if<std::string>(&*left)) {
        const auto &rs = std::get<std::string>(*right);
        switch (e.op) {
        case TokenKind::Less:
            return Value{*ls < rs};
        case TokenKind::LessEqual:
            return Value{*ls <= rs};
        case TokenKind::Greater:
            return Value{*ls > rs};
        case TokenKind::GreaterEqual:
            return Value{*ls >= rs};
        default:
            return std::nullopt;
        }
    }

    if (const auto *lb = std::get_if<bool>(&*left)) {
        const bool rb = std::get<bool>(*right);
        switch (e.op) {
        case TokenKind::Amp:
            return Value{*lb && rb};
        case TokenKind::Pipe:
            return Value{*lb || rb};
        case TokenKind::Caret:
            return Value{*lb != rb};
        default:
            return std::nullopt;
        }
    }

    if (const auto *lf = std::get_if<double>(&*left)) {
        const double rf = std::get<double>(*right);
        switch (e.op) {
        case TokenKind::Less:
            return Value{*lf < rf};
        case TokenKind::LessEqual:
            return Value{*lf <= rf};
        case TokenKind::Greater:
            return Value{*lf > rf};
        case TokenKind::GreaterEqual:
            return Value{*lf >= rf};
        case TokenKind::Plus:
            return Value{*lf + rf};
        case TokenKind::Minus:
            return Value{*lf - rf};
        case TokenKind::Star:
            return Value{*lf * rf};
        case TokenKind::Slash:
            return Value{*lf / rf};
        default:
            return std::nullopt;
        }
    }

    EmitError(e.location, std::format("compile-time operator '{}' is not defined for values of type '{}'",
                                      OperatorName(e.op), ValueTypeName(*left)));
    reportedError = true;
    return std::nullopt;
}

/// Evaluates a conditional-compilation condition, reporting why it cannot be used if it fails.
bool ConditionalEvaluator::Impl::EvalCondition(const Expr *condition, const SourceLocation location) {
    if (!condition) {
        return false;
    }
    reportedError = false;
    const auto value = Eval(*condition);
    if (!value) {
        if (!reportedError) {
            EmitError(location, "'when' condition is not a valid compile-time expression");
        }
        return false;
    }
    if (const auto *b = std::get_if<bool>(&*value)) {
        return *b;
    }
    EmitError(condition->location,
              std::format("'when' condition must have type 'bool', but found '{}'", ValueTypeName(*value)));
    return false;
}

// Compile-time match: `when subject { pattern => ..., else => ... }`

/// The compile-time value against an arm pattern, for the no-match diagnostic.
std::string ConditionalEvaluator::Impl::FormatValue(const Value &value) {
    if (const auto *e = std::get_if<EnumValue>(&value)) {
        return "." + e->variant;
    }
    if (const auto *s = std::get_if<std::string>(&value)) {
        return "\"" + *s + "\"";
    }
    if (const auto *b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
    }
    if (const auto *i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
    }
    if (const auto *u = std::get_if<std::uint64_t>(&value)) {
        return std::to_string(*u);
    }
    if (const auto *d = std::get_if<double>(&value)) {
        return std::to_string(*d);
    }
    return "the subject";
}

/// Whether the subject value equals an arm pattern.
std::optional<bool> ConditionalEvaluator::Impl::ArmMatches(const Value &subject, const Expr &pattern,
                                                           const SourceLocation location, Value *evaluatedPattern) {
    const auto patternValue = Eval(pattern);
    if (!patternValue) {
        return std::nullopt;
    }
    if (evaluatedPattern) {
        *evaluatedPattern = *patternValue;
    }
    if (const auto *lhs = std::get_if<EnumValue>(&subject)) {
        if (const auto *rhs = std::get_if<EnumValue>(&*patternValue)) {
            return EnumEquals(*lhs, *rhs, location);
        }
        return std::nullopt;
    }
    if (subject.index() != patternValue->index()) {
        EmitError(pattern.location, std::format("'when' pattern has type '{}', but the subject has type '{}'",
                                                ValueTypeName(*patternValue), ValueTypeName(subject)));
        reportedError = true;
        return std::nullopt;
    }
    return subject == *patternValue;
}

/// Index of the first arm one of whose patterns matches the subject; an arm with no patterns is the `else`. Returns -1
/// (reporting an error) when nothing matches and there is no `else`.
int ConditionalEvaluator::Impl::SelectMatchArmImpl(const Expr &subject,
                                                   const std::vector<std::vector<const Expr *>> &arms,
                                                   const SourceLocation location) {
    reportedError = false;
    const auto subjectValue = Eval(subject);
    if (!subjectValue) {
        if (!reportedError) {
            EmitError(location, "'when' match subject must be a compile-time constant expression");
        }
        return -1;
    }
    int elseIndex = -1;
    int selectedIndex = -1;
    std::unordered_set<std::string> seenPatterns;
    for (std::size_t i = 0; i < arms.size(); ++i) {
        if (arms[i].empty()) {
            if (elseIndex >= 0) {
                EmitError(location, "a compile-time 'when' match can have only one 'else' arm");
                reportedError = true;
            }
            elseIndex = static_cast<int>(i);
            continue;
        }
        for (const Expr *pattern : arms[i]) {
            Value patternValue{};
            const auto matched = ArmMatches(*subjectValue, *pattern, pattern->location, &patternValue);
            if (matched) {
                const std::string key = std::string(ValueTypeName(patternValue)) + ":" + FormatValue(patternValue);
                if (!seenPatterns.insert(key).second) {
                    EmitError(pattern->location,
                              std::format("duplicate compile-time 'when' pattern {}", FormatValue(patternValue)));
                    reportedError = true;
                }
            }
            if (matched && *matched && selectedIndex < 0) {
                selectedIndex = static_cast<int>(i);
            }
        }
    }
    if (selectedIndex >= 0) {
        return selectedIndex;
    }
    if (elseIndex >= 0) {
        return elseIndex;
    }
    if (!reportedError) {
        EmitError(location, std::format("no arm of this 'when' matches {}", FormatValue(*subjectValue)));
    }
    return -1;
}
} // namespace Rux
