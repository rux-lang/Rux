#include "Formatter/Formatter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Semantic/Conditional/ConditionalCompilation.h"
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

const std::string StringDeclaration = R"(
pub intrinsic struct string8 { pub data: *char8; pub length: uint; }
pub type string = string8;
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

TEST_CASE("string literals can be inferred without a provider") {
    auto parsed = ParseIntrinsicSource("func Main() { let text = \"hello\"; let wide = s16\"hello\"; }");
    const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("calling a text API does not require importing its signature types") {
    auto dependency = ParseIntrinsicSource(
        StringDeclaration + "pub func Count(text: string) -> uint { return text.length; }", "text.rux");
    auto parsed = ParseIntrinsicSource("import Text::Count; func Main() -> uint { return Count(\"hello\"); }");
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Text", {{"text.rux", &dependency.module}}}}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    (void)lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
}

TEST_CASE("string annotations and fields require visible declarations") {
    for (const std::string body : {"func Read(text: string) {}", "func Main() { let n = \"hello\".length; }"}) {
        CAPTURE(body);
        for (const bool imported : {false, true}) {
            CAPTURE(imported);
            auto dependency = ParseIntrinsicSource(StringDeclaration, "text.rux");
            auto parsed = ParseIntrinsicSource((imported ? "import Text::string;\n" : "") + body);
            const auto model =
                SemanticAnalyzer({&parsed.module}, {{"Text", {{"text.rux", &dependency.module}}}}, "App").Analyze();
            CHECK_EQ(model.HasErrors(), !imported);
        }
    }
}

TEST_CASE("an intrinsic type import in another file does not expose its members") {
    auto dependency = ParseIntrinsicSource(IntegerDeclaration + StringDeclaration, "provider.rux");
    auto imported = ParseIntrinsicSource("import Provider::{ int8, string };", "imported.rux");
    for (const std::string body :
         {"func Main() -> int8 { return int8::Max; }", "func Main() { let n = \"hello\".length; }"}) {
        auto isolated = ParseIntrinsicSource(body, "isolated.rux");
        const auto model = SemanticAnalyzer({&imported.module, &isolated.module},
                                            {{"Provider", {{"provider.rux", &dependency.module}}}}, "App")
                               .Analyze();
        CHECK(model.HasErrors());
    }
}

TEST_CASE("intrinsic slices and ordinary same-named structs have different representations") {
    for (const bool intrinsic : {false, true}) {
        CAPTURE(intrinsic);
        auto parsed = ParseIntrinsicSource(std::string(intrinsic ? "intrinsic " : "") + R"(
struct Slice<T> { pub data: *T; pub length: uint; }
func Read(values: Slice<int8>) -> int8 { return values[0]; }
)");
        const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
        CHECK_EQ(model.HasErrors(), !intrinsic);
        if (!model.HasErrors()) {
            const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items[1].get());
            REQUIRE(function);
            const TypeRef *type = model.TryGetType(*function->params[0].type);
            REQUIRE(type);
            CHECK(type->isIntrinsicSlice);
        }
    }
}

TEST_CASE("only an intrinsic range declaration binds range syntax") {
    for (const bool intrinsic : {false, true}) {
        CAPTURE(intrinsic);
        auto parsed = ParseIntrinsicSource(std::string(intrinsic ? "intrinsic " : "") + R"(
struct Range<T> { pub start: T; pub end: T; }
func Bounds() -> Range<int> { return 0..2; }
)");
        const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
        CHECK_EQ(model.HasErrors(), !intrinsic);
    }
}

TEST_CASE("intrinsic slice identity survives generic lowering") {
    auto parsed = ParseIntrinsicSource(R"(
intrinsic struct Slice<T> { pub data: *T; pub length: uint; }
func First<T>(values: Slice<T>) -> T { return values[0]; }
func Read(values: Slice<int8>) -> int8 { return First(values); }
)");
    const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    (void)lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
}

TEST_CASE("conditional compilation resolves source constants from a replacement package") {
    auto dependency = ParseIntrinsicSource(IntegerDeclaration, "provider.rux");
    auto parsed = ParseIntrinsicSource(R"(
import Replacement::int8;
when int8::Min == -128i8 {
    func Main() -> int { return 0; }
} else {
    func Main() -> Missing { return 0; }
}
)");
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Replacement", {{"provider.rux", &dependency.module}}}}, "App").Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("conditional compilation cannot obtain primitive constants without declarations") {
    auto parsed = ParseIntrinsicSource("when int8::Min == -128i8 { func Main() {} }");
    std::vector<Diagnostic> diagnostics;
    ResolveConditionalCompilation({&parsed.module}, CompileTimeContext{}, diagnostics);
    CHECK_FALSE(diagnostics.empty());
}

TEST_CASE("conditional context imports bind declarations without a privileged package identity") {
    auto dependency = ParseIntrinsicSource(R"(
pub enum OperatingSystem { FreeBSD, Linux, macOS, Windows }
pub struct Target { pub os: OperatingSystem; }
pub intrinsic #target: Target;
)",
                                           "platform.rux");
    auto parsed = ParseIntrinsicSource(R"(
import Platform::{ #target };
when #target.os == .Windows { func Main() {} }
else { func Main() -> Missing {} }
)");
    CompileTimeContext context;
    context.target.os = Target::OS::Windows;
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Platform", {{"platform.rux", &dependency.module}}}}, "App", context)
            .Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("an import in a discarded conditional branch never loads a provider") {
    auto parsed = ParseIntrinsicSource("when false { import Missing::int8; } func Main() {} ");
    bool requested = false;
    std::vector<Diagnostic> diagnostics;
    ResolveConditionalCompilation({&parsed.module}, CompileTimeContext{}, diagnostics, [&](std::string_view) {
        requested = true;
        return std::vector<Module *>{};
    });
    CHECK_FALSE(requested);
    CHECK(diagnostics.empty());
}

TEST_CASE("native source constant expressions use the compilation target") {
    auto dependency = ParseIntrinsicSource(R"(
pub intrinsic type uint;
extend uint {
    pub const Bits: uint = sizeof(uint) * 8u;
    pub const Max: uint = ~0u;
}
)",
                                           "native.rux");
    for (const std::uint32_t bytes : {4, 8}) {
        CAPTURE(bytes);
        auto parsed = ParseIntrinsicSource("import Native::uint; const Width = uint::Bits; const Maximum = uint::Max;");
        CompileTimeContext context;
        context.target.pointer_size = bytes;
        ConditionalEvaluator evaluator(context, {&parsed.module},
                                       [&](std::string_view) { return std::vector<Module *>{&dependency.module}; });
        evaluator.SetImports(parsed.module);
        const auto width = evaluator.EvaluateConstant("Width");
        REQUIRE(width.value);
        CHECK_EQ(std::get<std::uint64_t>(*width.value), bytes * 8);
        const auto maximum = evaluator.EvaluateConstant("Maximum");
        REQUIRE(maximum.value);
        CHECK_EQ(std::get<std::uint64_t>(*maximum.value), bytes == 4 ? 0xffffffffULL : 0xffffffffffffffffULL);
    }
}
