// Borrowed concrete-to-interface views through semantic analysis and lowering.

#include "AArch64RcuEmitterTestSupport.h"
#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/Layout.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Testing;

namespace {
std::vector<SemanticDiagnostic> AnalyzeInterfaceReferences(const std::string &source) {
    Lexer lexer(source, "interface-references.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "interface-references.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    return analyzer.Analyze().diagnostics;
}

bool HasErrorContaining(const std::vector<SemanticDiagnostic> &diagnostics, const std::string_view text) {
    return std::ranges::any_of(diagnostics, [&](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error && diagnostic.message.contains(text);
    });
}
} // namespace

TEST_CASE("interface references are borrowed fat views through HIR and LIR") {
    Lexer lexer(R"(
        interface CounterView {
            func Read() -> int32;
            func Add(amount: int32);
        }
        interface Marker {}
        struct Counter { value: int32; }
        extend Counter : CounterView {
            func Read(self: &Counter) -> int32 { return self.value; }
            func Add(self: &var Counter, amount: int32) { self.value += amount; }
        }
        func Observe(view: &CounterView) -> int32 { return view.Read(); }
        func Mutate(view: &var CounterView) { view.Add(3i32); }
        func Main() -> int32 {
            var counter = Counter { value: 4i32 };
            let view: &CounterView = counter;
            let marker: &Marker = counter;
            let observed = Observe(view);
            Mutate(counter);
            return observed;
        }
    )",
                "interface-references.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "interface-references.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        INFO("unexpected diagnostic: ", diagnostic.message);
        CHECK_NE(diagnostic.severity, Diagnostic::Severity::Error);
    }
    REQUIRE_FALSE(model.HasErrors());

    TypeRef interfaceType = TypeRef::MakeNamed("CounterView");
    const TypeRef sharedView = TypeRef::MakeReference(interfaceType);
    interfaceType.isMut = true;
    const TypeRef exclusiveView = TypeRef::MakeReference(interfaceType);
    const std::unordered_set<std::string> interfaces{"CounterView"};
    CHECK_EQ(Layout::RuntimeSizeOf(sharedView, {}, interfaces), 16);
    CHECK_EQ(Layout::RuntimeSizeOf(exclusiveView, {}, interfaces), 16);
    const ResolvedTypeLayout *resolved = model.TryGetLayout(sharedView);
    REQUIRE(resolved != nullptr);
    CHECK_EQ(resolved->size, 16);
    CHECK_EQ(resolved->alignment, 8);

    HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_EQ(hir.modules.size(), 1);
    const auto main =
        std::ranges::find_if(hir.modules[0].funcs, [](const HirFunc &function) { return function.name == "Main"; });
    REQUIRE(main != hir.modules[0].funcs.end());
    REQUIRE(main->body.has_value());
    REQUIRE_GE(main->body->stmts.size(), 4);
    const auto *viewBinding = dynamic_cast<const HirLetStmt *>(main->body->stmts[1].get());
    REQUIRE(viewBinding != nullptr);
    const auto *coercion = dynamic_cast<const HirCoerceToInterfaceExpr *>(viewBinding->init.get());
    REQUIRE(coercion != nullptr);
    CHECK(coercion->borrowed);
    CHECK_FALSE(coercion->vtableLabel.empty());
    CHECK_EQ(coercion->type, sharedView);
    const auto *markerBinding = dynamic_cast<const HirLetStmt *>(main->body->stmts[2].get());
    REQUIRE(markerBinding != nullptr);
    const auto *markerCoercion = dynamic_cast<const HirCoerceToInterfaceExpr *>(markerBinding->init.get());
    REQUIRE(markerCoercion != nullptr);
    CHECK(markerCoercion->borrowed);
    CHECK(markerCoercion->vtableLabel.empty());

    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const LirPackage lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    const auto observe =
        std::ranges::find_if(lir.modules[0].funcs, [](const LirFunc &function) { return function.name == "Observe"; });
    REQUIRE(observe != lir.modules[0].funcs.end());
    REQUIRE_EQ(observe->params.size(), 1);
    CHECK_EQ(observe->params[0].type, TypeRef::MakePointer(sharedView));

    const auto loweredMain =
        std::ranges::find_if(lir.modules[0].funcs, [](const LirFunc &function) { return function.name == "Main"; });
    REQUIRE(loweredMain != lir.modules[0].funcs.end());
    std::size_t concreteAllocas = 0;
    for (const LirBlock &block : loweredMain->blocks) {
        concreteAllocas +=
            static_cast<std::size_t>(std::ranges::count_if(block.instrs, [](const LirInstr &instruction) {
                return instruction.op == LirOpcode::Alloca && instruction.type == TypeRef::MakeNamed("Counter");
            }));
    }
    CHECK_EQ(concreteAllocas, 1);
}

TEST_CASE("exclusive interface views require mutable implementing places") {
    const auto diagnostics = AnalyzeInterfaceReferences(R"(
        interface CounterView { func Read() -> int32; }
        struct Counter { value: int32; }
        struct Plain { value: int32; }
        extend Counter : CounterView {
            func Read(self: &Counter) -> int32 { return self.value; }
        }
        func Mutate(view: &var CounterView) {}
        func Test() {
            let immutable = Counter { value: 1i32 };
            Mutate(immutable);
            var counter = Counter { value: 2i32 };
            let shared: &Counter = counter;
            Mutate(shared);
            var plain = Plain { value: 3i32 };
            Mutate(plain);
        }
    )");

    std::string messages;
    for (const SemanticDiagnostic &diagnostic : diagnostics) {
        messages += diagnostic.message + "\n";
    }
    INFO(messages);
    CHECK(HasErrorContaining(diagnostics, "requires '&var CounterView'"));
    CHECK(HasErrorContaining(diagnostics, "has type '&Counter'"));
    CHECK(HasErrorContaining(diagnostics, "has type 'Plain'"));
}

TEST_CASE("AArch64 lowers borrowed interface reference dispatch") {
    const LirPackage package = CompileToAArch64Lir(R"(
        interface CounterView { func Read() -> int32; }
        struct Counter { value: int32; }
        extend Counter : CounterView {
            func Read(self: &Counter) -> int32 { return self.value; }
        }
        func Observe(view: &CounterView) -> int32 { return view.Read(); }
        func Main() -> int32 {
            var counter = Counter { value: 7i32 };
            return Observe(counter);
        }
    )");

    AArch64RcuEmitter emitter(package, "test");
    const auto objects = emitter.Generate();
    CHECK_MESSAGE(emitter.Diagnostics().empty(), JoinMessages(emitter.Diagnostics()));
    REQUIRE_EQ(objects.size(), 1);
    CHECK(FindSymbol(objects.front(), "Main") != nullptr);
    CHECK(FindSymbol(objects.front(), "Observe") != nullptr);
}
