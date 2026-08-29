#include "Semantic/Detail/MovePlace.h"

#include <format>
#include <utility>

namespace Rux::SemanticDetail {
namespace {
std::string ReadableCanonical(const std::string &value) {
    if (!value.empty() && (value.front() == '$' || value.front() == '#')) {
        return value.substr(1);
    }
    return value;
}

std::string CanonicalIndex(const Expr &expression, const IndexIsOperatorCall &isOperatorCall) {
    if (const auto *move = dynamic_cast<const MoveExpr *>(&expression)) {
        return CanonicalIndex(*move->operand, isOperatorCall);
    }
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        return "$" + identifier->name;
    }
    if (dynamic_cast<const SelfExpr *>(&expression)) {
        return "$self";
    }
    if (const auto *literal = dynamic_cast<const LiteralExpr *>(&expression)) {
        return "#" + literal->token.text;
    }
    if (const auto *path = dynamic_cast<const PathExpr *>(&expression)) {
        std::string value = "%";
        for (std::size_t index = 0; index < path->segments.size(); ++index) {
            if (index != 0) {
                value += "::";
            }
            value += path->segments[index];
        }
        return value;
    }
    if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        const std::string object = CanonicalIndex(*field->object, isOperatorCall);
        return object.empty() ? std::string{} : object + "." + field->field;
    }
    if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        // A call has no stable identity to name, so an operator index leaves the whole path unknown rather than
        // reading as the projection its spelling resembles.
        if (isOperatorCall && isOperatorCall(*index)) {
            return {};
        }
        const std::string object = CanonicalIndex(*index->object, isOperatorCall);
        const std::string subscript = CanonicalIndex(*index->index, isOperatorCall);
        return object.empty() || subscript.empty() ? std::string{} : object + "[" + subscript + "]";
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression)) {
        const std::string operand = CanonicalIndex(*unary->operand, isOperatorCall);
        return operand.empty() ? std::string{} : std::format("u{}({})", static_cast<unsigned int>(unary->op), operand);
    }
    if (const auto *binary = dynamic_cast<const BinaryExpr *>(&expression)) {
        const std::string left = CanonicalIndex(*binary->left, isOperatorCall);
        const std::string right = CanonicalIndex(*binary->right, isOperatorCall);
        return left.empty() || right.empty()
                 ? std::string{}
                 : std::format("b{}({},{})", static_cast<unsigned int>(binary->op), left, right);
    }
    return {};
}

MovePlace AnalyzeImpl(const Expr &expression, const IndexIsOperatorCall &isOperatorCall) {
    if (const auto *move = dynamic_cast<const MoveExpr *>(&expression)) {
        return AnalyzeImpl(*move->operand, isOperatorCall);
    }
    if (const auto *identifier = dynamic_cast<const IdentExpr *>(&expression)) {
        return {MovePlace::RootKind::Named, identifier->name, &expression, {}};
    }
    if (dynamic_cast<const SelfExpr *>(&expression)) {
        return {MovePlace::RootKind::Self, "self", &expression, {}};
    }
    if (const auto *field = dynamic_cast<const FieldExpr *>(&expression)) {
        MovePlace place = AnalyzeImpl(*field->object, isOperatorCall);
        if (place.rootKind == MovePlace::RootKind::Temporary) {
            place.rootExpression = &expression;
        }
        place.projections.push_back({MovePlace::Projection::Kind::Field, field->field, field->location});
        return place;
    }
    if (const auto *index = dynamic_cast<const IndexExpr *>(&expression)) {
        // The operator returns a value, so this computes fresh storage rather than reaching into the object. Reading
        // it as a projection would make `<- v[i]` a partial move of `v`, and a borrow of `v` reach the result.
        if (isOperatorCall && isOperatorCall(*index)) {
            return {MovePlace::RootKind::Temporary, {}, &expression, {}};
        }
        MovePlace place = AnalyzeImpl(*index->object, isOperatorCall);
        if (place.rootKind == MovePlace::RootKind::Temporary) {
            place.rootExpression = &expression;
        }
        place.projections.push_back(
            {MovePlace::Projection::Kind::Index, CanonicalIndex(*index->index, isOperatorCall), index->location});
        return place;
    }
    if (const auto *unary = dynamic_cast<const UnaryExpr *>(&expression); unary && unary->op == TokenKind::Star) {
        return {MovePlace::RootKind::Dereference, CanonicalIndex(*unary->operand, isOperatorCall), &expression, {}};
    }
    return {MovePlace::RootKind::Temporary, {}, &expression, {}};
}
} // namespace

std::string MovePlace::Projection::Display() const {
    if (kind == Kind::Field) {
        return "." + value;
    }
    return value.empty() ? "[<computed>]" : std::format("[{}]", ReadableCanonical(value));
}

std::string MovePlace::Projection::Description() const {
    if (kind == Kind::Field) {
        return std::format("field '{}'", value);
    }
    return value.empty() ? "computed indexed element" : std::format("indexed element [{}]", ReadableCanonical(value));
}

bool MovePlace::IsNamedStorage() const noexcept {
    return rootKind == RootKind::Named || rootKind == RootKind::Self;
}

bool MovePlace::IsBorrowedStorage() const noexcept {
    return rootKind == RootKind::Dereference;
}

bool MovePlace::IsPartial() const noexcept {
    return !projections.empty();
}

bool MovePlace::IsComplete() const noexcept {
    return projections.empty();
}

bool MovePlace::HasKnownIdentity() const noexcept {
    if (rootKind == RootKind::Temporary || (rootKind == RootKind::Dereference && rootName.empty())) {
        return false;
    }
    for (const Projection &projection : projections) {
        if (projection.kind == Projection::Kind::Index && projection.value.empty()) {
            return false;
        }
    }
    return true;
}

std::string MovePlace::RootDisplay() const {
    std::string text;
    switch (rootKind) {
    case RootKind::Named:
    case RootKind::Self:
        text = rootName;
        break;
    case RootKind::Dereference:
        text = rootName.empty() ? "*<pointer>" : "*" + rootName.substr(1);
        break;
    case RootKind::Temporary:
        text = "<temporary>";
        break;
    }
    return text;
}

std::string MovePlace::Display() const {
    std::string text = RootDisplay();
    for (const Projection &projection : projections) {
        text += projection.Display();
    }
    return text;
}

std::string MovePlace::ContainerDisplay() const {
    if (projections.empty()) {
        return RootDisplay();
    }
    MovePlace container = *this;
    container.projections.pop_back();
    return container.Display();
}

std::string MovePlace::LastProjectionDescription() const {
    if (projections.empty()) {
        return "complete value";
    }
    return projections.back().Description();
}

MovePlace AnalyzeMovePlace(const Expr &expression, const IndexIsOperatorCall &isOperatorCall) {
    return AnalyzeImpl(expression, isOperatorCall);
}

bool SameStoragePlace(const Expr &left, const Expr &right, const IndexIsOperatorCall &isOperatorCall) {
    const MovePlace leftPlace = AnalyzeMovePlace(left, isOperatorCall);
    const MovePlace rightPlace = AnalyzeMovePlace(right, isOperatorCall);
    if (!leftPlace.HasKnownIdentity() || !rightPlace.HasKnownIdentity() || leftPlace.rootKind != rightPlace.rootKind ||
        leftPlace.rootName != rightPlace.rootName || leftPlace.projections.size() != rightPlace.projections.size()) {
        return false;
    }
    for (std::size_t index = 0; index < leftPlace.projections.size(); ++index) {
        const MovePlace::Projection &leftProjection = leftPlace.projections[index];
        const MovePlace::Projection &rightProjection = rightPlace.projections[index];
        if (leftProjection.kind != rightProjection.kind || leftProjection.value.empty() ||
            leftProjection.value != rightProjection.value) {
            return false;
        }
    }
    return true;
}
} // namespace Rux::SemanticDetail
