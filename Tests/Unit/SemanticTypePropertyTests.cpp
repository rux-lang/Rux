#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

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
