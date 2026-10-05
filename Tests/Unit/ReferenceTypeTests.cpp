// Reference-type representation from syntax through the target ABI boundary.

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
#include <iterator>
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

TEST_CASE("borrowed scalar reads retain reference types in semantic facts and load in HIR") {
    Lexer lexer(R"(
        func Read(value: &var uint128) -> uint128 {
            let alias: &uint128 = value;
            let copied: uint128 = alias;
            return copied;
        }
    )",
                "scalar-read.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "scalar-read.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *alias = dynamic_cast<const LetStmt *>(function->body->stmts[0].get());
    const auto *copy = dynamic_cast<const LetStmt *>(function->body->stmts[1].get());
    REQUIRE(alias != nullptr);
    REQUIRE(copy != nullptr);
    REQUIRE(alias->init != nullptr);
    REQUIRE(copy->init != nullptr);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    CHECK_FALSE(model.HasBorrowedScalarRead(*alias->init));
    CHECK(model.HasBorrowedScalarRead(*copy->init));
    REQUIRE(model.TryGetType(*copy->init) != nullptr);
    CHECK_EQ(model.TryGetType(*copy->init)->kind, TypeRef::Kind::Reference);
    CHECK(model.TryGetConsumption(*copy->init) == nullptr);
    CHECK(model.TryGetCopy(*copy->init) == nullptr);

    HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_EQ(hir.modules.size(), 1);
    REQUIRE_EQ(hir.modules.front().funcs.size(), 1);
    const HirFunc &read = hir.modules.front().funcs.front();
    REQUIRE(read.body.has_value());
    const auto *loweredAlias = dynamic_cast<const HirLetStmt *>(read.body->stmts[0].get());
    const auto *loweredCopy = dynamic_cast<const HirLetStmt *>(read.body->stmts[1].get());
    REQUIRE(loweredAlias != nullptr);
    REQUIRE(loweredCopy != nullptr);
    CHECK(dynamic_cast<const HirVarExpr *>(loweredAlias->init.get()) != nullptr);
    const auto *load = dynamic_cast<const HirUnaryExpr *>(loweredCopy->init.get());
    REQUIRE(load != nullptr);
    CHECK_EQ(load->op, TokenKind::Star);
    CHECK_EQ(load->type.ToString(), "uint128");
    CHECK_EQ(load->operand->type.kind, TypeRef::Kind::Reference);
    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const auto lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    CHECK_FALSE(lir.modules.empty());
}

TEST_CASE("borrowed scalar writes store through the reference in HIR") {
    Lexer lexer(R"(
        func Bump(count: &var int64) {
            count += 2i64;
        }
    )",
                "scalar-write.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "scalar-write.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *statement = dynamic_cast<const ExprStmt *>(function->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    CHECK(model.HasReferenceWrite(*assignment->target));
    CHECK_FALSE(model.HasBorrowedScalarRead(*assignment->target));
    REQUIRE(model.TryGetType(*assignment->target) != nullptr);
    CHECK_EQ(model.TryGetType(*assignment->target)->kind, TypeRef::Kind::Reference);

    HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_EQ(hir.modules.size(), 1);
    REQUIRE_EQ(hir.modules.front().funcs.size(), 1);
    const HirFunc &bump = hir.modules.front().funcs.front();
    REQUIRE(bump.body.has_value());
    const auto *loweredStatement = dynamic_cast<const HirExprStmt *>(bump.body->stmts[0].get());
    REQUIRE(loweredStatement != nullptr);
    const auto *store = dynamic_cast<const HirAssignExpr *>(loweredStatement->expr.get());
    REQUIRE(store != nullptr);
    CHECK_EQ(store->type.ToString(), "int64");
    const auto *place = dynamic_cast<const HirUnaryExpr *>(store->target.get());
    REQUIRE(place != nullptr);
    CHECK_EQ(place->op, TokenKind::Star);
    CHECK_EQ(place->operand->type.kind, TypeRef::Kind::Reference);
    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const auto lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    CHECK_FALSE(lir.modules.empty());
}

TEST_CASE("a write through a reference names the reference and its writable spelling") {
    const auto diagnostics = AnalyzeReferences(R"(
        func Observe(count: &int) { count += 1; }
        func Pointerlike(count: &var int) { *count += 1; }
        func Rebound() {
            var total = 1;
            var alias: &var int = total;
            alias = 2;
        }
    )");
    std::vector<SemanticDiagnostic> errors;
    std::ranges::copy_if(diagnostics, std::back_inserter(errors), [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error;
    });
    REQUIRE_EQ(errors.size(), 3);
    CHECK_EQ(errors[0].message, "cannot modify 'count' through immutable reference '&int'");
    CHECK_EQ(errors[0].help, "declare 'count' as '&var int' to write through it");
    CHECK_EQ(errors[1].message, "operator '*' requires a pointer operand, but found '&var int'");
    CHECK_EQ(errors[1].help, "write 'count' in place of '*count', as in 'count += 1'");
    CHECK_EQ(errors[2].message, "cannot assign 'int' to '&var int'");
    CHECK_EQ(errors[2].help, "declare 'alias' with 'let' to write through it");
}

TEST_CASE("a whole value written through a reference replaces the referent and destroys the old one") {
    Lexer lexer(R"(
        struct Handle { value: int32; }
        extend Handle { func ~Handle(self: &var Handle) {} }
        func Replace(slot: &var Handle) {
            slot = Handle { value: 2i32 };
        }
    )",
                "aggregate-write.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "aggregate-write.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.back().get());
    REQUIRE(function != nullptr);
    REQUIRE(function->body != nullptr);
    const auto *statement = dynamic_cast<const ExprStmt *>(function->body->stmts[0].get());
    REQUIRE(statement != nullptr);
    const auto *assignment = dynamic_cast<const AssignExpr *>(statement->expr.get());
    REQUIRE(assignment != nullptr);
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    CHECK(model.HasReferenceWrite(*assignment->target));
    REQUIRE(model.TryGetType(*assignment->target) != nullptr);
    CHECK_EQ(model.TryGetType(*assignment->target)->kind, TypeRef::Kind::Reference);

    HirPackage hir = AstToHirLowering(model).Generate();
    REQUIRE_EQ(hir.modules.size(), 1);
    const auto replace = std::ranges::find(hir.modules.front().funcs, "Replace", &HirFunc::name);
    REQUIRE(replace != hir.modules.front().funcs.end());
    REQUIRE(replace->body.has_value());
    const auto *loweredStatement = dynamic_cast<const HirExprStmt *>(replace->body->stmts[0].get());
    REQUIRE(loweredStatement != nullptr);
    const auto *store = dynamic_cast<const HirAssignExpr *>(loweredStatement->expr.get());
    REQUIRE(store != nullptr);
    CHECK_EQ(store->type.ToString(), "Handle");
    const auto *place = dynamic_cast<const HirUnaryExpr *>(store->target.get());
    REQUIRE(place != nullptr);
    CHECK_EQ(place->op, TokenKind::Star);
    // The referent is the caller's live value, so the store destroys it where it stands rather than through a binding.
    REQUIRE(store->overwriteCleanup.has_value());
    CHECK(store->overwriteCleanup->destroysTarget);
    CHECK_EQ(store->overwriteCleanup->bindingId, 0);
    CHECK_EQ(store->overwriteCleanup->glueSymbol, "__rux_drop__Handle");
    HirToLirLowering lowering(std::move(hir), CompileTimeContext{}.target);
    const auto lir = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    CHECK_FALSE(lir.modules.empty());
}

TEST_CASE("a write to a parameter never suggests declaring it with 'var'") {
    const auto diagnostics = AnalyzeReferences(R"(
        struct Pair { a: int32; b: int32; }
        extend Pair {
            func Reset(self: &var Pair) { self = Pair { a: 0i32, b: 0i32 }; }
            func Cleared(self: Pair) -> Pair {
                self = Pair { a: 0i32, b: 0i32 };
                return self;
            }
        }
        func Observe(pair: &Pair) { pair = Pair { a: 1i32, b: 2i32 }; }
        func Local(pair: Pair) { pair = Pair { a: 1i32, b: 2i32 }; }
        func Call(pair: Pair) { pair.Reset(); }
    )");
    std::vector<SemanticDiagnostic> errors;
    std::ranges::copy_if(diagnostics, std::back_inserter(errors), [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.severity == Diagnostic::Severity::Error;
    });
    REQUIRE_EQ(errors.size(), 4);
    CHECK_EQ(errors[0].message, "cannot modify immutable receiver 'self'");
    CHECK_EQ(errors[0].help, "take the receiver as 'self: &var Pair' to change the caller's value");
    CHECK_EQ(errors[1].message, "cannot modify 'pair' through immutable reference '&Pair'");
    CHECK_EQ(errors[1].help, "declare 'pair' as '&var Pair' to write through it");
    CHECK_EQ(errors[2].message, "cannot modify parameter 'pair'");
    CHECK_EQ(errors[2].help, "take 'pair' as '&var Pair' to change the caller's value, or move it into a 'var' local");
    CHECK_EQ(errors[3].message, "cannot call 'Reset' on immutable 'pair'");
    CHECK_EQ(errors[3].help, "take 'pair' as '&var Pair' to change the caller's value, or move it into a 'var' local");
}

TEST_CASE("borrowed scalar reads do not copy aggregates or stored pointers") {
    SUBCASE("Copy struct") {
        const auto diagnostics = AnalyzeReferences(R"(
            struct Pair { first: int; second: int; }
            func Copy(value: &Pair) { let copied: Pair = value; }
        )");
        CHECK(HasErrorContaining(diagnostics, "Pair"));
        CHECK_FALSE(diagnostics.empty());
    }
    SUBCASE("move-only struct") {
        const auto diagnostics = AnalyzeReferences(R"(
            struct Owner { pointer: *var int; }
            extend Owner { func ~Owner(self: &var Owner) {} }
            func Copy(value: &Owner) -> Owner { return value; }
        )");
        CHECK(HasErrorContaining(diagnostics, "reference value '&Owner' cannot escape"));
    }
    SUBCASE("raw pointer") {
        const auto diagnostics = AnalyzeReferences(R"(
            func Copy(value: &(*int)) -> *int { return value; }
        )");
        CHECK(HasErrorContaining(diagnostics, "cannot escape through a return"));
    }
    SUBCASE("explicit reference move") {
        const auto diagnostics = AnalyzeReferences(R"(
            func Move(value: &int) -> int { return <-value; }
        )");
        CHECK(HasErrorContaining(diagnostics, "cannot move a non-owning reference"));
    }
    SUBCASE("mutable reborrow") {
        const auto diagnostics = AnalyzeReferences(R"(
            func Mutate(value: &var int) {}
            func Forward(value: &int) { Mutate(value); }
        )");
        CHECK(HasErrorContaining(diagnostics, "requires '&var int'"));
    }
}

TEST_CASE("generic borrowed scalar reads validate every instantiated referent") {
    SUBCASE("multiple primitive widths") {
        const auto diagnostics = AnalyzeReferences(R"(
            func Read<T>(value: &var T) -> T { return value; }
            func Main() {
                var narrow = 3i8;
                var wide = 18446744073709551617u128;
                let first = Read<int8>(narrow);
                let second = Read<uint128>(wide);
            }
        )");
        for (const auto &diagnostic : diagnostics) {
            INFO(diagnostic.message);
            CHECK_NE(diagnostic.severity, Diagnostic::Severity::Error);
        }
    }
    SUBCASE("Copy aggregate after scalar instantiation") {
        const auto diagnostics = AnalyzeReferences(R"(
            struct Pair { first: int; second: int; }
            func Read<T>(value: &T) -> T { return value; }
            func Main() {
                let scalar = 7;
                let accepted = Read<int>(scalar);
                let pair = Pair { first: 1, second: 2 };
                let rejected = Read<Pair>(pair);
            }
        )");
        CHECK(HasErrorContaining(diagnostics, "implicit value read through '&Pair' requires a Copy primitive scalar"));
    }
    SUBCASE("generic arithmetic cannot introduce an aggregate copy") {
        const auto diagnostics = AnalyzeReferences(R"(
            struct Pair { first: int; second: int; }
            extend Pair { func +(self: &Pair, other: Pair) -> Pair { return other; } }
            func Add<T>(left: &T, right: T) -> T { return left + right; }
            func Main() {
                let pair = Pair { first: 1, second: 2 };
                let rejected = Add<Pair>(pair, pair);
            }
        )");
        CHECK(HasErrorContaining(diagnostics, "requires a Copy primitive scalar"));
    }
}

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

    // The binding, not the type, is what the call refuses, so the error names it and points at 'var'.
    const auto refused = std::ranges::find_if(diagnostics, [](const SemanticDiagnostic &diagnostic) {
        return diagnostic.message == "argument 1 to 'Write' cannot borrow immutable 'item' as '&var Item'";
    });
    REQUIRE(refused != diagnostics.end());
    REQUIRE_EQ(refused->notes.size(), 1);
    CHECK(refused->notes.front().starts_with("parameter 'item' declared at"));
    REQUIRE(refused->help.has_value());
    CHECK_EQ(*refused->help, "declare 'item' with 'var' to make it mutable");
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
        variant Choice<T> { Some(T), None }
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
