// Reference-type representation from syntax through the target ABI boundary.

#include "CodeGen/Layout.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
std::vector<SemanticDiagnostic> AnalyzeReferences(const std::string &source) {
    Lexer lexer(source, "reference-diagnostics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "reference-diagnostics.rux");
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

TEST_CASE("reference types preserve identity mutability and layout") {
    const TypeRef shared = TypeRef::MakeReference(TypeRef::MakeInt32());
    TypeRef writableReferent = TypeRef::MakeInt32();
    writableReferent.isMut = true;
    const TypeRef exclusive = TypeRef::MakeReference(std::move(writableReferent));

    CHECK_EQ(shared.ToString(), "&int32");
    CHECK_EQ(exclusive.ToString(), "&var int32");
    CHECK_NE(shared, exclusive);
    CHECK(exclusive.IsAssignableTo(shared));
    CHECK_FALSE(shared.IsAssignableTo(exclusive));
    CHECK_EQ(shared.SizeInBytes(), 8);
    CHECK_EQ(Layout::SizeOf(shared), 8);
    CHECK_EQ(Layout::AlignOf(shared), 8);

    CHECK_EQ(TypeRef::MakeArray(shared, 4).ToString(), "(&int32)[4]");
    CHECK_EQ(TypeRef::MakeReference(TypeRef::MakeArray(TypeRef::MakeInt32(), 4)).ToString(), "&(int32[4])");
}

TEST_CASE("reference parameters retain their types through HIR and LIR") {
    Lexer lexer(R"(
        struct Item { value: int32; }
        type Shared = &Item;
        #Link("test")
        extern func Inspect(shared: &Item, exclusive: &var Item) -> int32;
    )",
                "references.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "references.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    const ExternFuncDecl *declaration = nullptr;
    for (const DeclPtr &item : parsed.module.items) {
        if (const auto *external = dynamic_cast<const ExternFuncDecl *>(item.get())) {
            declaration = external;
        }
    }
    REQUIRE(declaration != nullptr);
    REQUIRE_EQ(declaration->params.size(), 2);
    CHECK(dynamic_cast<const ReferenceTypeExpr *>(declaration->params[0].type.get()) != nullptr);
    CHECK(dynamic_cast<const ReferenceTypeExpr *>(declaration->params[1].type.get()) != nullptr);

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    for (const Diagnostic &diagnostic : model.diagnostics) {
        INFO("unexpected diagnostic: ", diagnostic.message);
        CHECK_NE(diagnostic.severity, Diagnostic::Severity::Error);
    }
    REQUIRE_FALSE(model.HasErrors());

    const TypeRef *shared = model.TryGetType(*declaration->params[0].type);
    const TypeRef *exclusive = model.TryGetType(*declaration->params[1].type);
    REQUIRE(shared != nullptr);
    REQUIRE(exclusive != nullptr);
    CHECK_EQ(shared->kind, TypeRef::Kind::Reference);
    CHECK_EQ(shared->ToString(), "&Item");
    CHECK_EQ(exclusive->kind, TypeRef::Kind::Reference);
    CHECK_EQ(exclusive->ToString(), "&var Item");

    const TypeProperties *properties = model.TryGetProperties(*declaration->params[0].type);
    REQUIRE(properties != nullptr);
    CHECK(properties->IsCopy());

    HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_EQ(hir.modules.size(), 1);
    REQUIRE_EQ(hir.modules[0].typeAliases.size(), 1);
    CHECK_EQ(hir.modules[0].typeAliases[0].type.kind, TypeRef::Kind::Reference);
    REQUIRE_EQ(hir.modules[0].externFuncs.size(), 1);
    REQUIRE_EQ(hir.modules[0].externFuncs[0].params.size(), 2);
    CHECK_EQ(hir.modules[0].externFuncs[0].params[0].type, *shared);
    CHECK_EQ(hir.modules[0].externFuncs[0].params[1].type, *exclusive);

    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const LirPackage lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    REQUIRE_EQ(lir.modules.size(), 1);
    REQUIRE_EQ(lir.modules[0].funcs.size(), 1);
    REQUIRE_EQ(lir.modules[0].funcs[0].params.size(), 2);
    CHECK_EQ(lir.modules[0].funcs[0].params[0].type, *shared);
    CHECK_EQ(lir.modules[0].funcs[0].params[1].type, *exclusive);
}

TEST_CASE("exclusive borrows require writable places") {
    const auto diagnostics = AnalyzeReferences(R"(
        struct Item { value: int32; }
        func Write(item: &var Item) {}
        func Test() {
            let item = Item { value: 1i32 };
            Write(item);
        }
    )");

    CHECK(HasErrorContaining(diagnostics, "requires '&var Item'"));
}

TEST_CASE("references reject pointer-only operations") {
    const auto diagnostics = AnalyzeReferences(R"(
        struct Item { value: int32; }
        func Accept(item: &Item) {}
        func Test(item: &Item) {
            let dereferenced = *item;
            let advanced = item + 1;
            let empty: &Item = null;
            Accept(null);
        }
    )");

    CHECK(HasErrorContaining(diagnostics, "operator '*' requires a pointer operand"));
    CHECK(HasErrorContaining(diagnostics, "operator '+' cannot combine"));
    CHECK(HasErrorContaining(diagnostics, "null cannot initialize non-null reference '&Item'"));
    CHECK(HasErrorContaining(diagnostics, "requires '&Item'"));
}

TEST_CASE("borrowed enum values can be inspected by match") {
    const auto diagnostics = AnalyzeReferences(R"(
        enum Choice<T> { Some(T), None }
        extend Choice<T> {
            func IsSome(self: &Choice<T>) -> bool {
                return match self {
                    .Some(_) => true,
                    .None => false
                };
            }
        }
        func Inspect(value: &Choice<int32>) -> bool {
            return match value {
                .Some(_) => value.IsSome(),
                .None => false
            };
        }
    )");

    CHECK(diagnostics.empty());
}

TEST_CASE("references cannot transfer borrowed ownership") {
    const auto diagnostics = AnalyzeReferences(R"(
        struct Resource { value: int32; }
        extend Resource {
            func =(self: &var Resource, other: &Resource);
            func ~Resource(self: &var Resource) {}
        }
        struct Holder { resource: Resource; }
        extend Holder {
            func Take(self: Holder) {}
        }
        func Consume(value: Resource) {}
        func Test(shared: &Holder) {
            Consume(<-shared.resource);
            (<-shared).Take();
        }
    )");

    CHECK(HasErrorContaining(diagnostics, "out of borrowed reference storage"));
    CHECK(HasErrorContaining(diagnostics, "cannot move a non-owning reference"));
}
