#include "Semantic/Model/SemanticModel.h"

#include <doctest.h>
#include <utility>

using namespace Rux;

TEST_CASE("semantic facts retain node identity when a model moves") {
    LiteralExpr recorded;
    LiteralExpr unrelated;
    SemanticFacts facts;
    facts.expressionTypes.emplace(&recorded, TypeRef::MakeInt32());
    SemanticModel original({}, {}, {}, {}, std::move(facts));
    SemanticModel moved = std::move(original);

    REQUIRE(moved.TryGetType(recorded) != nullptr);
    CHECK(*moved.TryGetType(recorded) == TypeRef::MakeInt32());
    CHECK(moved.TryGetType(unrelated) == nullptr);
    CHECK_FALSE(moved.HasErrors());
}

TEST_CASE("semantic results keep independently analyzed facts separate") {
    LiteralExpr expression;
    SemanticFacts firstFacts;
    firstFacts.expressionTypes.emplace(&expression, TypeRef::MakeInt32());
    SemanticFacts secondFacts;
    secondFacts.expressionTypes.emplace(&expression, TypeRef::MakeInt64());
    SemanticModel first({}, {}, {}, {}, std::move(firstFacts));
    SemanticModel second({}, {}, {}, {}, std::move(secondFacts));

    REQUIRE(first.TryGetType(expression) != nullptr);
    REQUIRE(second.TryGetType(expression) != nullptr);
    CHECK(*first.TryGetType(expression) == TypeRef::MakeInt32());
    CHECK(*second.TryGetType(expression) == TypeRef::MakeInt64());
}
