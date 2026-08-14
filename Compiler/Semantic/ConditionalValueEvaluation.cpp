#include "Semantic/ConditionalEvaluatorInternal.h"

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Rux {
namespace ConditionalEvaluation {
// "hello" / c8"hello" -> hello. Only the escapes that can plausibly appear in a
// `when` comparison are decoded; anything else is kept verbatim.
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
            return std::nullopt;
        }
        if (e.op == TokenKind::AmpAmp && !*lb) {
            return Value{false};
        }
        if (e.op == TokenKind::PipePipe && *lb) {
            return Value{true};
        }
        const auto right = Eval(*e.right);
        if (!right || !std::holds_alternative<bool>(*right)) {
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
                return std::nullopt;
            }
            const auto lu = static_cast<std::uint64_t>(*l);
            const auto ru = static_cast<std::uint64_t>(*r);
            switch (e.op) {
            case TokenKind::Plus:
                return Value{static_cast<std::int64_t>(lu + ru)};
            case TokenKind::Minus:
                return Value{static_cast<std::int64_t>(lu - ru)};
            case TokenKind::Star:
                return Value{static_cast<std::int64_t>(lu * ru)};
            case TokenKind::Slash:
            case TokenKind::Percent:
                if (*r == 0 || (*l == std::numeric_limits<std::int64_t>::min() && *r == -1))
                    return std::nullopt;
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
                if (*r < 0 || *r >= 64)
                    return std::nullopt;
                if (e.op == TokenKind::LessLess) {
                    return Value{static_cast<std::int64_t>(lu << static_cast<std::uint64_t>(*r))};
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
            return std::nullopt;
        }
        switch (e.op) {
        case TokenKind::Plus:
            return Value{l + *r};
        case TokenKind::Minus:
            return Value{l - *r};
        case TokenKind::Star:
            return Value{l * *r};
        case TokenKind::Slash:
        case TokenKind::Percent:
            if (*r == 0)
                return std::nullopt;
            return Value{e.op == TokenKind::Slash ? l / *r : l % *r};
        case TokenKind::Amp:
            return Value{l & *r};
        case TokenKind::Pipe:
            return Value{l | *r};
        case TokenKind::Caret:
            return Value{l ^ *r};
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
            if (*r >= 64)
                return std::nullopt;
            return Value{e.op == TokenKind::LessLess ? l << *r : l >> *r};
        case TokenKind::GreaterGreaterGreater:
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    if (left->index() != right->index()) {
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

    return std::nullopt;
}

// Evaluates a conditional-compilation condition, reporting why it cannot
// be used if it fails.
bool ConditionalEvaluator::Impl::EvalCondition(const Expr *condition, const SourceLocation location) {
    if (!condition) {
        return false;
    }
    reportedError = false;
    const auto value = Eval(*condition);
    if (!value) {
        if (!reportedError) {
            EmitError(location, "'when' condition must be a compile-time constant expression");
        }
        return false;
    }
    if (const auto *b = std::get_if<bool>(&*value)) {
        return *b;
    }
    EmitError(location, "'when' condition must be of type 'bool'");
    return false;
}

// Compile-time match: `when subject { pattern => ..., else => ... }`

// The compile-time value against an arm pattern, for the no-match diagnostic.
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

// Whether the subject value equals an arm pattern.
std::optional<bool> ConditionalEvaluator::Impl::ArmMatches(const Value &subject, const Expr &pattern,
                                                           const SourceLocation location) {
    const auto patternValue = Eval(pattern);
    if (!patternValue) {
        return std::nullopt;
    }
    if (const auto *lhs = std::get_if<EnumValue>(&subject)) {
        if (const auto *rhs = std::get_if<EnumValue>(&*patternValue)) {
            return EnumEquals(*lhs, *rhs, location);
        }
        return std::nullopt;
    }
    if (subject.index() != patternValue->index()) {
        return false;
    }
    return subject == *patternValue;
}

// Index of the first arm one of whose patterns matches the subject; an arm
// with no patterns is the `else`. Returns -1 (reporting an error) when
// nothing matches and there is no `else`.
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
    for (std::size_t i = 0; i < arms.size(); ++i) {
        if (arms[i].empty()) {
            elseIndex = static_cast<int>(i);
            continue;
        }
        for (const Expr *pattern : arms[i]) {
            const auto matched = ArmMatches(*subjectValue, *pattern, pattern->location);
            if (matched && *matched) {
                return static_cast<int>(i);
            }
        }
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
