#include "Ir/Hir/HirPrinter.h"
#include "Ir/Lir/LirPrinter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/Analysis/ProgramIndex.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace Rux;

namespace {
ParseResult ParseModule(const std::string &source, const std::string &sourceName) {
    Lexer lexer(source, sourceName);
    auto tokens = lexer.Tokenize();
    REQUIRE_FALSE(tokens.HasErrors());
    Parser parser(std::move(tokens.tokens), sourceName);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

const EnumDecl &RequireCaseType(const ParseResult &parsed, const std::string &name) {
    for (const auto &declaration : parsed.module.items) {
        const auto *caseType = dynamic_cast<const EnumDecl *>(declaration.get());
        if (caseType && caseType->name == name) {
            return *caseType;
        }
    }
    FAIL("missing case-bearing declaration " << name);
    throw std::runtime_error("missing case-bearing declaration");
}

const FuncDecl &RequireFunction(const ParseResult &parsed, const std::string &name) {
    for (const auto &declaration : parsed.module.items) {
        const auto *function = dynamic_cast<const FuncDecl *>(declaration.get());
        if (function && function->name == name) {
            return *function;
        }
    }
    FAIL("missing function declaration " << name);
    throw std::runtime_error("missing function declaration");
}

const CallExpr &RequireReturnedCall(const FuncDecl &function) {
    REQUIRE(function.body != nullptr);
    REQUIRE_EQ(function.body->stmts.size(), 1);
    const auto *returned = dynamic_cast<const ReturnStmt *>(function.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *call = dynamic_cast<const CallExpr *>(returned->value->get());
    REQUIRE(call != nullptr);
    return *call;
}

std::string DumpHir(const HirPackage &package, const std::string &stem) {
    const auto path = std::filesystem::temp_directory_path() / (stem + ".hir");
    REQUIRE(HirPrinter::Dump(package, path));
    std::ifstream input(path);
    REQUIRE(input.good());
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    return output;
}

std::string DumpLir(const LirPackage &package, const std::string &stem) {
    const auto path = std::filesystem::temp_directory_path() / (stem + ".lir");
    REQUIRE(LirPrinter::Dump(package, path));
    std::ifstream input(path);
    REQUIRE(input.good());
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    return output;
}
} // namespace

TEST_CASE("semantic program index isolates packages and records declaration ownership") {
    auto first = ParseModule(R"(
        module Api {
            func Make(value: int) -> Entry {}
            func Read(value: int) -> int { return value; }
        }
    )",
                             "first.rux");
    auto second = ParseModule(R"(
        module Api {
            struct Entry { value: int; }
            func Read(value: bool) -> int { return 0; }
        }
    )",
                              "second.rux");

    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    SemanticDetail::SemanticProgramIndex index(diagnostics, symbols);
    auto resolveType = [](const TypeExpr &) { return TypeRef::MakeUnknown(); };
    SemanticDetail::Scope &packageRoot = index.CreatePackageRoot("acme-core");
    const std::string packageName = "acme-core";
    for (const auto &declaration : first.module.items) {
        index.CollectDeclaration(*declaration, packageRoot, first.module.name, resolveType, &packageName);
    }
    for (const auto &declaration : second.module.items) {
        index.CollectDeclaration(*declaration, packageRoot, second.module.name, resolveType, &packageName);
    }

    REQUIRE(diagnostics.empty());
    CHECK(index.GlobalScope().Lookup("Api") == nullptr);
    const auto &packageScopes = index.Packages().at("acme-core");
    REQUIRE(packageScopes.contains("Api"));
    const SemanticDetail::Symbol *read = packageScopes.at("Api")->LookupLocal("Read");
    REQUIRE(read != nullptr);
    REQUIRE_EQ(read->funcOverloads.size(), 2);
    // Where a function lives is its package as well as the `module` blocks around it, so that two packages
    // declaring one name are two owners rather than one.
    CHECK(index.FunctionModulePaths().at(read->funcOverloads[0]) == "acme-core::Api");
    CHECK(index.FunctionSources().at(read->funcOverloads[0]) == "first.rux");
    CHECK(index.FunctionSources().at(read->funcOverloads[1]) == "second.rux");
    CHECK(packageScopes.at("Api")->LookupLocal("Entry") != nullptr);
}

TEST_CASE("semantic program index preserves enum and variant forms by source") {
    auto scalar = ParseModule(R"(
        pub enum Status: uint8 {
            Idle = 0,
            Running = 1
        }
    )",
                              "scalar.rux");
    auto tagged = ParseModule(R"(
        pub variant Status<Value> {
            Empty,
            Ready(Value)
        }
        variant Signal {
            Low,
            High
        }
    )",
                              "tagged.rux");

    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    SemanticDetail::SemanticProgramIndex index(diagnostics, symbols);
    auto resolveType = [](const TypeExpr &) { return TypeRef::MakeUnknown(); };

    SemanticDetail::Scope &scalarRoot = index.CreatePackageRoot("scalar-package");
    SemanticDetail::Scope &taggedRoot = index.CreatePackageRoot("tagged-package");
    const std::string scalarPackage = "scalar-package";
    const std::string taggedPackage = "tagged-package";
    for (const auto &declaration : scalar.module.items) {
        index.CollectDeclaration(*declaration, scalarRoot, scalar.module.name, resolveType, &scalarPackage);
    }
    for (const auto &declaration : tagged.module.items) {
        index.CollectDeclaration(*declaration, taggedRoot, tagged.module.name, resolveType, &taggedPackage);
    }

    CAPTURE(diagnostics.size());
    REQUIRE(diagnostics.empty());
    const EnumDecl *scalarStatus = index.EnumIn("scalar.rux", "Status");
    const EnumDecl *taggedStatus = index.EnumIn("tagged.rux", "Status");
    const EnumDecl *signal = index.EnumIn("tagged.rux", "Signal");
    REQUIRE(scalarStatus != nullptr);
    REQUIRE(taggedStatus != nullptr);
    REQUIRE(signal != nullptr);

    CHECK(scalarStatus->form == EnumDecl::Form::Enumeration);
    CHECK_FALSE(scalarStatus->IsVariant());
    CHECK(taggedStatus->form == EnumDecl::Form::Variant);
    CHECK(taggedStatus->IsVariant());
    CHECK(signal->form == EnumDecl::Form::Variant);
    CHECK(signal->IsVariant());

    REQUIRE(index.TypeParamsIn("scalar.rux", "Status") != nullptr);
    CHECK(index.TypeParamsIn("scalar.rux", "Status")->empty());
    REQUIRE(index.TypeParamsIn("tagged.rux", "Status") != nullptr);
    REQUIRE_EQ(index.TypeParamsIn("tagged.rux", "Status")->size(), 1);
    CHECK_EQ(index.TypeParamsIn("tagged.rux", "Status")->front().name, "Value");

    // The compatibility bare-name map still has last-declaration-wins behavior, but the pointer carries the source
    // form and the source-specific map keeps same-named declarations distinct.
    REQUIRE(index.Enums().contains("Status"));
    CHECK(index.Enums().at("Status") == taggedStatus);
    CHECK(index.Enums().at("Status")->IsVariant());
    CHECK(index.EnumIn("missing.rux", "Status") == nullptr);
    CHECK(index.EnumIn("tagged.rux", "Missing") == nullptr);
}

TEST_CASE("same-scope enum and variant names collide as one nominal type namespace") {
    auto parsed = ParseModule(R"(
        enum Choice { First }
        variant Choice<T> { Second(T) }
    )",
                              "collision.rux");

    std::vector<SemanticDiagnostic> diagnostics;
    std::vector<SemanticSymbol> symbols;
    SemanticDetail::SemanticProgramIndex index(diagnostics, symbols);
    auto resolveType = [](const TypeExpr &) { return TypeRef::MakeUnknown(); };
    for (const auto &declaration : parsed.module.items) {
        index.CollectDeclaration(*declaration, index.GlobalScope(), parsed.module.name, resolveType);
    }

    REQUIRE_EQ(diagnostics.size(), 1);
    CHECK(diagnostics.front().message.contains("Choice"));
    const auto *resolved = index.GlobalScope().LookupLocal("Choice");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->kind == SemanticDetail::Symbol::Kind::Type);

    const EnumDecl *lastIndexed = index.EnumIn("collision.rux", "Choice");
    REQUIRE(lastIndexed != nullptr);
    CHECK(lastIndexed->IsVariant());
    REQUIRE_EQ(lastIndexed->typeParams.size(), 1);
}

TEST_CASE("constructor facts and IR retain source declaration form") {
    ParseResult parsed = ParseModule(R"(
        pub enum Color: uint8 {
            Red = 1,
            Green = 2
        }

        pub variant Maybe<T> {
            None,
            Some(T)
        }

        variant Signal {
            Idle,
            Ready
        }

        func MakeSome() -> Maybe<int32> {
            return Maybe::Some<int32>(7i32);
        }

        func MakeReady() -> Signal {
            return Signal::Ready();
        }
    )",
                                     "forms.rux");

    const EnumDecl &color = RequireCaseType(parsed, "Color");
    const EnumDecl &maybe = RequireCaseType(parsed, "Maybe");
    const EnumDecl &signal = RequireCaseType(parsed, "Signal");
    CHECK_FALSE(color.IsVariant());
    CHECK(maybe.IsVariant());
    CHECK(signal.IsVariant());
    CHECK(signal.variants[0].fields.empty());
    CHECK(signal.variants[1].fields.empty());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "forms", "Windows");
    SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        INFO("unexpected semantic diagnostic: ", diagnostic.message);
        REQUIRE(diagnostic.severity != Diagnostic::Severity::Error);
    }

    const CallExpr &someCall = RequireReturnedCall(RequireFunction(parsed, "MakeSome"));
    const ResolvedCallableBinding *someBinding = model.TryGetCallableBinding(someCall);
    REQUIRE(someBinding != nullptr);
    CHECK(someBinding->dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant);
    CHECK(someBinding->selectedDeclaration == &maybe);
    CHECK(someBinding->selectedVariant == &maybe.variants[1]);
    CHECK(someBinding->caseTypeForm == EnumDecl::Form::Variant);
    REQUIRE_EQ(someBinding->substitutions.size(), 1);
    CHECK(someBinding->substitutions.at("T") == TypeRef::MakeInt32());

    const CallExpr &readyCall = RequireReturnedCall(RequireFunction(parsed, "MakeReady"));
    const ResolvedCallableBinding *readyBinding = model.TryGetCallableBinding(readyCall);
    REQUIRE(readyBinding != nullptr);
    CHECK(readyBinding->dispatch == ResolvedCallableBinding::DispatchKind::EnumVariant);
    CHECK(readyBinding->selectedDeclaration == &signal);
    CHECK(readyBinding->selectedVariant == &signal.variants[1]);
    CHECK(readyBinding->caseTypeForm == EnumDecl::Form::Variant);
    CHECK(readyBinding->substitutions.empty());

    AstToHirLowering astToHir(model);
    HirPackage hir = astToHir.Generate();
    REQUIRE(astToHir.Diagnostics().empty());
    REQUIRE_EQ(hir.modules.size(), 1);
    REQUIRE_EQ(hir.modules.front().enums.size(), 3);
    const HirEnum &hirColor = hir.modules.front().enums[0];
    const HirEnum &hirMaybe = hir.modules.front().enums[1];
    const HirEnum &hirSignal = hir.modules.front().enums[2];
    CHECK(hirColor.form == CaseTypeForm::Enumeration);
    CHECK_FALSE(hirColor.IsVariant());
    CHECK(hirMaybe.form == CaseTypeForm::Variant);
    CHECK(hirMaybe.IsVariant());
    CHECK(hirSignal.form == CaseTypeForm::Variant);
    CHECK(hirSignal.IsVariant());
    CHECK_EQ(hirMaybe.typeParams.size(), 1);
    CHECK(hirSignal.typeParams.empty());

    const std::string hirDump = DumpHir(hir, "rux-case-type-forms");
    CHECK(hirDump.contains("pub enum Color: uint8"));
    CHECK(hirDump.contains("pub variant Maybe<T>\n"));
    CHECK(hirDump.contains("variant Signal\n"));
    CHECK_FALSE(hirDump.contains("enum Maybe<T>:"));
    CHECK_FALSE(hirDump.contains("enum Signal:"));
    CHECK_FALSE(hirDump.contains("variant Maybe<T>:"));
    CHECK_FALSE(hirDump.contains("variant Signal:"));

    HirToLirLowering hirToLir(std::move(hir), TargetContext::CreateNative());
    LirPackage lir = hirToLir.Generate();
    REQUIRE(hirToLir.Diagnostics().empty());
    REQUIRE_EQ(lir.modules.size(), 1);
    REQUIRE_EQ(lir.modules.front().enums.size(), 3);
    const LirEnumDecl &lirColor = lir.modules.front().enums[0];
    const LirEnumDecl &lirMaybe = lir.modules.front().enums[1];
    const LirEnumDecl &lirSignal = lir.modules.front().enums[2];
    CHECK(lirColor.form == CaseTypeForm::Enumeration);
    CHECK_FALSE(lirColor.IsVariant());
    CHECK(lirMaybe.form == CaseTypeForm::Variant);
    CHECK(lirMaybe.IsVariant());
    CHECK(lirSignal.form == CaseTypeForm::Variant);
    CHECK(lirSignal.IsVariant());
    CHECK_EQ(lirMaybe.typeParams.size(), 1);
    CHECK(lirSignal.typeParams.empty());

    const std::string lirDump = DumpLir(lir, "rux-case-type-forms");
    CHECK(lirDump.contains("pub enum Color: uint8"));
    CHECK(lirDump.contains("pub variant Maybe<T>\n"));
    CHECK(lirDump.contains("variant Signal\n"));
    CHECK_FALSE(lirDump.contains("enum Maybe<T>:"));
    CHECK_FALSE(lirDump.contains("enum Signal:"));
    CHECK_FALSE(lirDump.contains("variant Maybe<T>:"));
    CHECK_FALSE(lirDump.contains("variant Signal:"));
}
