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

const char *DestructorHandleSource = R"(
    struct Handle {
        slot: int32;
    }

    extend Handle {
        func ~Handle(self: &var Handle) {
            self.slot = 0i32;
        }
    }
)";
} // namespace

TEST_SUITE("DropGlueLowering") {
    TEST_CASE("a type-named destructor is invoked by synthesized glue") {
        const LirPackage package = CompileToLir(std::string(DestructorHandleSource) + R"(
            func Main() -> int {
                let held = Handle { slot: 1i32 };
                let copied = held;
                return 0;
            }
        )");

        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Handle"));
        CHECK_EQ(CallCount(glue, "Handle::~Handle"), 1);
        CHECK_EQ(CallCount(glue, "Handle::Drop"), 0);
        CHECK_EQ(CallCount(RequireFunction(package, "Main"), GlueSymbol(package, "Handle")), 2);
    }

    TEST_CASE("generic type-named destructors are instantiated for concrete owners") {
        const LirPackage package = CompileToLir(R"(
            struct Owner<T> { value: T; }
            extend Owner<T> {
                func ~Owner(self: &var Owner<T>) {}
            }

            func Main() -> int {
                let owner = Owner<int32> { value: 1i32 };
                return 0;
            }
        )");

        const auto plan = std::ranges::find(package.dropGlues, TypeRef::MakeNamed("Owner<int32>"), &DropGluePlan::type);
        REQUIRE(plan != package.dropGlues.end());
        REQUIRE_EQ(plan->steps.size(), 1);
        CHECK(plan->steps.front().dropSymbol.contains("~Owner"));
        CHECK_EQ(CallCount(RequireFunction(package, plan->symbol), plan->steps.front().dropSymbol), 1);
        RequireFunction(package, plan->steps.front().dropSymbol);
    }

    TEST_CASE("type-named destruction takes precedence during Drop compatibility") {
        const LirPackage package = CompileToLir(R"(
            struct Compatible { value: int32; }
            extend Compatible : Drop {
                func Drop(self: *var Compatible) {}
                func ~Compatible(self: &var Compatible) {}
            }

            func Main() -> int {
                let value = Compatible { value: 1i32 };
                return 0;
            }
        )");

        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Compatible"));
        CHECK_EQ(CallCount(glue, "Compatible::~Compatible"), 1);
        CHECK_EQ(CallCount(glue, "Compatible::Drop"), 0);
    }

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

    TEST_CASE("drop glue recursively reaches nested fields and array elements") {
        const LirPackage package = CompileToLir(std::string(DestructorHandleSource) + R"(
            struct Inner {
                handle: Handle;
            }
            struct Outer {
                inner: Inner;
                handles: Handle[2];
            }

            func Main() -> int {
                let outer = Outer {
                    inner: Inner { handle: Handle { slot: 1i32 } },
                    handles: [Handle { slot: 2i32 }, Handle { slot: 3i32 }],
                };
                return 0;
            }
        )");

        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Outer"));
        // One call is for the nested field and one is the body of the reverse array-destruction loop.
        CHECK_EQ(CallCount(glue, "Handle::~Handle"), 2);
        CHECK_EQ(CallCount(glue, GlueSymbol(package, "Inner")), 0);
        CHECK_EQ(CallCount(glue, GlueSymbol(package, "Handle")), 0);
    }

    TEST_CASE("small payload enums retain addressable storage for destruction") {
        const LirPackage package = CompileToLir(std::string(DestructorHandleSource) + R"(
            enum Tiny: uint8 {
                Empty,
                Some(Handle)
            }

            func Main() -> int {
                let tiny = Tiny::Some(Handle { slot: 1i32 });
                return 0;
            }
        )");

        const LirFunc &glue = RequireFunction(package, GlueSymbol(package, "Tiny"));
        CHECK_EQ(CallCount(glue, "Handle::~Handle"), 1);
        CHECK(std::ranges::any_of(
            glue.blocks, [](const LirBlock &block) { return block.term && block.term->kind == LirTermKind::Branch; }));
    }

    TEST_CASE("propagation rolls back completed aggregate components") {
        const LirPackage package = CompileToLir(std::string(DestructorHandleSource) + R"(
            enum Error: uint8 { Bad }
            enum Result<T, E> { Success(T), Error(E) }
            struct Pair {
                first: Handle;
                second: int32;
            }

            func Read(ok: bool) -> Result<int32, Error> {
                return Result::Success<int32, Error>(7i32);
            }

            func Build(ok: bool) -> Result<Pair, Error> {
                return Result::Success<Pair, Error>(Pair {
                    first: Handle { slot: 1i32 },
                    second: Read(ok)?,
                });
            }
        )");

        // The call is on the propagated-failure path. The completed first field is not a named local, so this call
        // can only come from the aggregate's failure-cleanup edge.
        CHECK_EQ(CallCount(RequireFunction(package, "Build"), GlueSymbol(package, "Handle")), 1);
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
