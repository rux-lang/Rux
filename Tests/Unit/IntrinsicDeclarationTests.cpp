#include "Formatter/Formatter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <string>

using namespace Rux;

namespace {
ParseResult ParseIntrinsicSource(const std::string &source, const std::string &file = "intrinsics.rux") {
    auto tokens = Lexer(source, file).Tokenize();
    REQUIRE_FALSE(tokens.HasErrors());
    auto parsed = Parser(std::move(tokens.tokens), file).Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    return parsed;
}

const std::string IntegerDeclaration = R"(
pub intrinsic type int8;
extend int8 {
    pub const Min: int8 = -128i8;
    pub const Max: int8 = 127i8;
    const Hidden: int8 = 3i8;
}
)";
} // namespace

TEST_CASE("intrinsic declarations preserve types fields and associated constants in the AST") {
    auto parsed = ParseIntrinsicSource(IntegerDeclaration + R"(
pub intrinsic struct string8 { pub data: *char8; pub length: uint; }
pub intrinsic type float32;
extend float32 { pub intrinsic const Infinity: float32; }
)");
    REQUIRE_EQ(parsed.module.items.size(), 5);
    const auto *scalar = dynamic_cast<const TypeAliasDecl *>(parsed.module.items[0].get());
    REQUIRE(scalar);
    CHECK_EQ(scalar->intrinsicName, "int8");
    const auto *extension = dynamic_cast<const ImplDecl *>(parsed.module.items[1].get());
    REQUIRE(extension);
    REQUIRE_EQ(extension->constants.size(), 3);
    CHECK_EQ(extension->constants[0]->name, "Min");
    CHECK(extension->constants[0]->value);
    const auto *string = dynamic_cast<const StructDecl *>(parsed.module.items[2].get());
    REQUIRE(string);
    CHECK_EQ(string->intrinsicName, "string8");
    REQUIRE_EQ(string->fields.size(), 2);
    CHECK_EQ(string->fields[1].name, "length");
    const auto *floating = dynamic_cast<const ImplDecl *>(parsed.module.items[4].get());
    REQUIRE(floating);
    REQUIRE_EQ(floating->constants.size(), 1);
    CHECK_EQ(floating->constants[0]->intrinsicName, "float32.Infinity");
    CHECK_FALSE(floating->constants[0]->value);
}

TEST_CASE("scalar representation requires no intrinsic declaration") {
    auto parsed = ParseIntrinsicSource("func Main() -> int8 { return 1i8 + 2i8; }");
    const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("associated constants require a visible declaration") {
    auto dependency = ParseIntrinsicSource(IntegerDeclaration, "replacement.rux");
    for (const bool imported : {false, true}) {
        CAPTURE(imported);
        auto parsed = ParseIntrinsicSource((imported ? "import Replacement::int8;\n" : "") +
                                           std::string("func Main() -> int8 { return int8::Min; }"));
        DepPackage replacement{"Replacement", {{"replacement.rux", &dependency.module}}};
        const auto model = SemanticAnalyzer({&parsed.module}, {replacement}, "App").Analyze();
        for (const auto &diagnostic : model.diagnostics) {
            INFO(diagnostic.message);
        }
        CHECK_EQ(model.HasErrors(), !imported);
        if (imported && !model.HasErrors()) {
            AstToHirLowering lowering(model);
            (void)lowering.Generate();
            CHECK(lowering.Diagnostics().empty());
        }
    }
}

TEST_CASE("associated constant bindings point to the selected source declaration") {
    auto parsed = ParseIntrinsicSource(IntegerDeclaration + "func Main() -> int8 { return int8::Max; }");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "Replacement").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.back().get());
    REQUIRE(function);
    const auto *statement = dynamic_cast<const ReturnStmt *>(function->body->stmts[0].get());
    REQUIRE(statement);
    const auto *constant = model.TryGetAssociatedConstant(**statement->value);
    REQUIRE(constant);
    CHECK_EQ(constant->name, "Max");
    CHECK(constant->value);
}

TEST_CASE("private associated constants stay private to their package") {
    auto dependency = ParseIntrinsicSource(IntegerDeclaration, "replacement.rux");
    auto parsed = ParseIntrinsicSource("import Replacement::int8; func Main() -> int8 { return int8::Hidden; }");
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Replacement", {{"replacement.rux", &dependency.module}}}}, "App")
            .Analyze();
    REQUIRE(model.HasErrors());
    bool privacy = false;
    for (const auto &diagnostic : model.diagnostics) {
        privacy |= diagnostic.message.find("private") != std::string::npos;
    }
    CHECK(privacy);
}

TEST_CASE("floating special values require explicit associated declarations") {
    auto parsed = ParseIntrinsicSource(R"(
pub intrinsic type float32;
extend float32 {
    pub intrinsic const Infinity: float32;
    pub intrinsic const NaN: float32;
}
func Infinite() -> float32 { return float32::Infinity; }
func Invalid() -> float32 { return float32::NaN; }
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "Replacement").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    (void)lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
}

TEST_CASE("different intrinsic providers cannot overwrite one visible declaration") {
    auto first = ParseIntrinsicSource(IntegerDeclaration, "first.rux");
    auto second = ParseIntrinsicSource(IntegerDeclaration, "second.rux");
    auto parsed = ParseIntrinsicSource("import First::int8; import Second::int8; func Main() {} ");
    const auto model =
        SemanticAnalyzer({&parsed.module},
                         {{"First", {{"first.rux", &first.module}}}, {"Second", {{"second.rux", &second.module}}}},
                         "App")
            .Analyze();
    CHECK(model.HasErrors());
}

TEST_CASE("intrinsic declarations reject unknown kinds and incompatible fields") {
    for (const std::string source : {
             "intrinsic type Widget;",
             "intrinsic type string8;",
             "intrinsic struct Widget {}",
             "intrinsic struct string8 { pub length: uint; pub data: *char8; }",
             "intrinsic struct string16 { pub data: *char8; pub length: uint; }",
             "intrinsic struct Slice<T> { data: *T; pub length: uint; }",
             "intrinsic struct Range<T, U> { pub start: T; pub end: T; }",
             "intrinsic type int8; extend int8 { intrinsic const Infinity: int8; }",
             "intrinsic type int8; extend int8 { const X = 1; const X = 2; }",
         }) {
        CAPTURE(source);
        auto parsed = ParseIntrinsicSource(source);
        const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
        CHECK(model.HasErrors());
    }
}
