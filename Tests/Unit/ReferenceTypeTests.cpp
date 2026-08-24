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
#include <utility>

using namespace Rux;

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
        struct View<T> { value: &T; }
        type Shared = &Item;
        #Link("test")
        extern func Inspect(shared: &Item, exclusive: &var Item, view: View<Item>) -> int32;
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
    REQUIRE_EQ(declaration->params.size(), 3);
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
    const auto instantiated = std::ranges::find_if(
        hir.modules[0].structs, [](const HirStruct &structure) { return structure.name == "View<Item>"; });
    REQUIRE(instantiated != hir.modules[0].structs.end());
    REQUIRE_EQ(instantiated->fields.size(), 1);
    CHECK_EQ(instantiated->fields[0].type.ToString(), "&Item");
    REQUIRE_EQ(hir.modules[0].externFuncs.size(), 1);
    REQUIRE_EQ(hir.modules[0].externFuncs[0].params.size(), 3);
    CHECK_EQ(hir.modules[0].externFuncs[0].params[0].type, *shared);
    CHECK_EQ(hir.modules[0].externFuncs[0].params[1].type, *exclusive);

    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const LirPackage lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    REQUIRE_EQ(lir.modules.size(), 1);
    REQUIRE_EQ(lir.modules[0].funcs.size(), 1);
    REQUIRE_EQ(lir.modules[0].funcs[0].params.size(), 3);
    CHECK_EQ(lir.modules[0].funcs[0].params[0].type, *shared);
    CHECK_EQ(lir.modules[0].funcs[0].params[1].type, *exclusive);
}
