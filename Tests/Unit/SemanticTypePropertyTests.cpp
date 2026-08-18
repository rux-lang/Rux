#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>

using namespace Rux;

TEST_CASE("semantic model recursively classifies copy move-only and droppable types") {
    Lexer lexer(R"(
        interface Drop {}

        struct Handle {
            value: int32;
        }

        extend Handle : Drop {}

        struct Wrapper {
            handle: Handle;
        }

        struct Pair<T> {
            value: T;
            tag: int32;
        }

        enum Maybe<T> {
            None,
            Some(T)
        }

        union Bits {
            signed: int32,
            unsigned: uint32
        }

        func Identity<T>(value: T) {}

        func Observe(copy: Pair<int32>, moved: Pair<Handle>, wrapped: Wrapper, choice: Maybe<Handle>,
                     bits: Bits, pointer: *Handle, array: Handle[2], tuple: (Handle, int32)) {}
    )",
                "type_properties.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "type_properties.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *identity = nullptr;
    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        const auto *function = dynamic_cast<const FuncDecl *>(item.get());
        if (function && function->name == "Identity") {
            identity = function;
        }
        if (function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(identity != nullptr);
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto propertiesOf = [&](const Param &parameter) -> const TypeProperties & {
        const TypeProperties *properties = model.TryGetProperties(*parameter.type);
        REQUIRE(properties != nullptr);
        return *properties;
    };

    REQUIRE_EQ(identity->params.size(), 1);
    const TypeProperties &unresolved = propertiesOf(identity->params[0]);
    CHECK_FALSE(unresolved.IsResolved());
    CHECK_FALSE(unresolved.IsDroppable());

    REQUIRE_EQ(observe->params.size(), 8);
    const TypeProperties &copy = propertiesOf(observe->params[0]);
    CHECK(copy.IsCopy());
    CHECK_FALSE(copy.IsDroppable());

    for (const std::size_t index : {1U, 2U, 3U, 6U, 7U}) {
        const TypeProperties &properties = propertiesOf(observe->params[index]);
        CHECK(properties.IsMoveOnly());
        CHECK(properties.IsDroppable());
    }

    for (const std::size_t index : {4U, 5U}) {
        const TypeProperties &properties = propertiesOf(observe->params[index]);
        CHECK(properties.IsCopy());
        CHECK_FALSE(properties.IsDroppable());
    }

    CHECK(model.TryGetProperties(TypeRef::MakeNamed("NeverObserved")) == nullptr);
}

TEST_CASE("drop glue expands concrete aggregates in reverse construction order") {
    Lexer lexer(R"(
        interface Drop {}

        struct Leaf { value: int32; }
        extend Leaf : Drop {}

        struct Pair<T> {
            first: T;
            copy: int32;
            second: T;
        }

        struct Owner { leaf: Leaf; }
        extend Owner : Drop {}

        struct Unused { leaf: Leaf; }

        enum Choice<T> {
            None,
            Both(T, int32, T)
        }

        func Observe(leaf: Leaf, pair: Pair<Leaf>, owner: Owner, values: Leaf[3], choice: Choice<Leaf>,
                     tuple: (Leaf, int32, Leaf)) {}
    )",
                "drop_glue.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "drop_glue.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const FuncDecl *observe = nullptr;
    for (const auto &item : parsed.module.items) {
        const auto *function = dynamic_cast<const FuncDecl *>(item.get());
        if (function && function->name == "Observe") {
            observe = function;
        }
    }
    REQUIRE(observe != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    const auto glueFor = [&](const std::size_t parameter) -> const DropGluePlan & {
        const TypeRef *type = model.TryGetType(*observe->params[parameter].type);
        REQUIRE(type != nullptr);
        const DropGluePlan *glue = model.TryGetDropGlue(*type);
        REQUIRE(glue != nullptr);
        return *glue;
    };
    const auto requireDirectDrop = [](const DropGlueStep &step, const std::string &type) {
        CHECK_EQ(step.kind, DropGlueStep::Kind::InvokeDrop);
        CHECK_EQ(step.type, TypeRef::MakeNamed(type));
    };

    const DropGluePlan &leaf = glueFor(0);
    CHECK_EQ(leaf.symbol, "__rux_drop__Leaf");
    REQUIRE_EQ(leaf.steps.size(), 1);
    requireDirectDrop(leaf.steps[0], "Leaf");

    const DropGluePlan &pair = glueFor(1);
    REQUIRE_EQ(pair.steps.size(), 2);
    CHECK_EQ(pair.steps[0].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(pair.steps[0].name, "second");
    CHECK_EQ(pair.steps[0].ordinal, 2);
    REQUIRE_EQ(pair.steps[0].children.size(), 1);
    requireDirectDrop(pair.steps[0].children[0], "Leaf");
    CHECK_EQ(pair.steps[1].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(pair.steps[1].name, "first");
    CHECK_EQ(pair.steps[1].ordinal, 0);

    const DropGluePlan &owner = glueFor(2);
    REQUIRE_EQ(owner.steps.size(), 2);
    requireDirectDrop(owner.steps[0], "Owner");
    CHECK_EQ(owner.steps[1].kind, DropGlueStep::Kind::Field);
    CHECK_EQ(owner.steps[1].name, "leaf");

    const DropGluePlan &array = glueFor(3);
    REQUIRE_EQ(array.steps.size(), 1);
    CHECK_EQ(array.steps[0].kind, DropGlueStep::Kind::ArrayElements);
    CHECK_EQ(array.steps[0].count, 3);
    CHECK(array.steps[0].reverse);
    REQUIRE_EQ(array.steps[0].children.size(), 1);
    requireDirectDrop(array.steps[0].children[0], "Leaf");

    const DropGluePlan &choice = glueFor(4);
    REQUIRE_EQ(choice.steps.size(), 1);
    CHECK_EQ(choice.steps[0].kind, DropGlueStep::Kind::EnumVariant);
    CHECK_EQ(choice.steps[0].name, "Both");
    REQUIRE_EQ(choice.steps[0].children.size(), 2);
    CHECK_EQ(choice.steps[0].children[0].ordinal, 2);
    CHECK_EQ(choice.steps[0].children[1].ordinal, 0);

    const DropGluePlan &tuple = glueFor(5);
    REQUIRE_EQ(tuple.steps.size(), 2);
    CHECK_EQ(tuple.steps[0].kind, DropGlueStep::Kind::TupleElement);
    CHECK_EQ(tuple.steps[0].ordinal, 2);
    CHECK_EQ(tuple.steps[1].kind, DropGlueStep::Kind::TupleElement);
    CHECK_EQ(tuple.steps[1].ordinal, 0);

    CHECK(model.TryGetDropGlue(TypeRef::MakeInt32()) == nullptr);
    CHECK(model.TryGetDropGlue(TypeRef::MakeNamed("Pair<int32>")) == nullptr);
    const DropGluePlan *unused = model.TryGetDropGlue(TypeRef::MakeNamed("Unused"));
    REQUIRE(unused != nullptr);
    REQUIRE_EQ(unused->steps.size(), 1);
    CHECK_EQ(unused->steps[0].name, "leaf");

    const HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_FALSE(hir.dropGlues.empty());
    for (std::size_t index = 1; index < hir.dropGlues.size(); ++index) {
        CHECK(hir.dropGlues[index - 1].type.ToString() < hir.dropGlues[index].type.ToString());
    }
    const auto loweredPair = std::ranges::find(hir.dropGlues, TypeRef::MakeNamed("Pair<Leaf>"), &DropGluePlan::type);
    REQUIRE(loweredPair != hir.dropGlues.end());
    CHECK_EQ(loweredPair->steps.size(), pair.steps.size());
}
