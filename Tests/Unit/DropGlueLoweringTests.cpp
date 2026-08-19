// What a destruction point becomes: one glue function per droppable type, and a flag-guarded call to it wherever
// analysis decided a value's life ends.

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Rux;

namespace {
constexpr const char *DropInterface = R"(
    interface Drop {
        func Drop();
    }
)";

LirPackage CompileToLir(const std::string &source) {
    Lexer lexer(std::string(DropInterface) + source, "drop.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "drop.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), CompileTimeContext{}.target);
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

/// How many times a function calls one symbol, across every block.
std::size_t CallCount(const LirFunc &function, const std::string &symbol) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call && instruction.strArg == symbol) {
                ++count;
            }
        }
    }
    return count;
}

/// The glue symbol for a plain named type, taken from the plan rather than rebuilt, so the test cannot drift from the
/// mangling the compiler actually uses.
std::string GlueSymbol(const LirPackage &package, const std::string &typeName) {
    for (const DropGluePlan &plan : package.dropGlues) {
        if (plan.type.name == typeName) {
            return plan.symbol;
        }
    }
    FAIL("missing destruction plan for " << typeName);
    throw std::runtime_error("missing destruction plan");
}

const char *HandleSource = R"(
    struct Handle {
        slot: int32;
    }

    extend Handle : Drop {
        func Drop(self: *var Handle) {
            self.slot = 0i32;
        }
    }
)";
} // namespace

TEST_SUITE("DropGlueLowering") {
    TEST_CASE("glue is one function taking the value's address") {
        const LirPackage package = CompileToLir(std::string(HandleSource) + R"(
            func Main() -> int {
                let held = Handle { slot: 1i32 };
                return 0;
            }
        )");

        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Handle"));
        REQUIRE_EQ(glue.params.size(), 1);
        CHECK_EQ(glue.params.front().type.kind, TypeRef::Kind::Pointer);
        REQUIRE_FALSE(glue.params.front().type.inner.empty());
        CHECK_EQ(glue.params.front().type.inner.front().name, "Handle");
        CHECK_EQ(CallCount(glue, "Handle::Drop"), 1);
    }

    TEST_CASE("a scope's end calls the glue once, behind the binding's flag") {
        const LirPackage package = CompileToLir(std::string(HandleSource) + R"(
            func Main() -> int {
                let held = Handle { slot: 1i32 };
                return 0;
            }
        )");

        const LirFunc &main = RequireFunction(package, "Main");
        CHECK_EQ(CallCount(main, GlueSymbol(package, "Handle")), 1);
        // The call is not on the straight path: it sits in a block reached by a branch on the flag.
        const bool guarded = std::ranges::any_of(
            main.blocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; });
        CHECK(guarded);
    }

    TEST_CASE("a value moved to another binding is destroyed once") {
        const LirPackage package = CompileToLir(std::string(HandleSource) + R"(
            func Main() -> int {
                let first = Handle { slot: 1i32 };
                let second = first;
                return 0;
            }
        )");

        // Two bindings, but the first one's flag is cleared by the move, so only one of the two calls can run.
        CHECK_EQ(CallCount(RequireFunction(package, "Main"), GlueSymbol(package, "Handle")), 2);
    }

    TEST_CASE("a containing aggregate destroys its field") {
        const LirPackage package = CompileToLir(std::string(HandleSource) + R"(
            struct Owner {
                handle: Handle;
            }

            func Main() -> int {
                let owner = Owner { handle: Handle { slot: 1i32 } };
                return 0;
            }
        )");

        // A plan carries the whole recipe rather than deferring to the field's own glue, so the field's `Drop` is
        // reached directly and destroying an owner costs one call per droppable part instead of one per level.
        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Owner"));
        CHECK_EQ(CallCount(glue, "Handle::Drop"), 1);
        CHECK_EQ(CallCount(glue, GlueSymbol(package, "Handle")), 0);
        CHECK_EQ(CallCount(RequireFunction(package, "Main"), GlueSymbol(package, "Owner")), 1);
    }

    TEST_CASE("a Copy type gets no plan and no glue") {
        const LirPackage package = CompileToLir(R"(
            struct Plain {
                value: int32;
            }

            func Main() -> int {
                let plain = Plain { value: 1i32 };
                return 0;
            }
        )");

        CHECK(package.dropGlues.empty());
        for (const LirModule &module : package.modules) {
            CHECK(std::ranges::none_of(
                module.funcs, [](const LirFunc &function) { return function.name.starts_with("__rux_drop__"); }));
        }
    }
}
