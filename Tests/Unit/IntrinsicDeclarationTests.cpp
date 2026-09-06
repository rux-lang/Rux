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
pub type Utf8 = char8[..];
)";
} // namespace

TEST_CASE("intrinsic declarations preserve types fields and associated constants in the AST") {
    auto parsed = ParseIntrinsicSource(IntegerDeclaration + R"(
pub struct Packet { pub data: *char8; pub length: uint; }
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
    CHECK(string->intrinsicName.empty());
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
        auto tokens = Lexer(source, "removed.rux").Tokenize();
        auto parsed = Parser(std::move(tokens.tokens), "removed.rux").Parse();
        if (source.contains("intrinsic struct")) {
            CHECK(parsed.HasErrors());
        }
        else {
            REQUIRE_FALSE(parsed.HasErrors());
            CHECK(SemanticAnalyzer({&parsed.module}).Analyze().HasErrors());
        }
    }
}

TEST_CASE("string literals can be inferred without a provider") {
    auto parsed = ParseIntrinsicSource("func Main() { let text = \"hello\"; let wide = c16\"hello\"; }");
    const auto model = SemanticAnalyzer({&parsed.module}).Analyze();
    CHECK_FALSE(model.HasErrors());
}

TEST_CASE("calling a text API does not require importing its signature types") {
    auto dependency = ParseIntrinsicSource(
        StringDeclaration + "pub func Count(text: char8[..]) -> uint { return text.length; }", "text.rux");
    auto parsed = ParseIntrinsicSource("import Text::Count; func Main() -> uint { return Count(\"hello\"); }");
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Text", {{"text.rux", &dependency.module}}}}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    (void)lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
}

TEST_CASE("character slice annotations and members need no provider") {
    // Native slice annotations and fields work with or without an unrelated imported alias.
    for (const bool imported : {false, true}) {
        CAPTURE(imported);
        auto dependency = ParseIntrinsicSource(StringDeclaration, "text.rux");
        const std::string prefix = imported ? "import Text::Utf8;\n" : "";
        auto annotated = ParseIntrinsicSource(prefix + "func Read(text: char8[..]) {}");
        CHECK_EQ(SemanticAnalyzer({&annotated.module}, {{"Text", {{"text.rux", &dependency.module}}}}, "App")
                     .Analyze()
                     .HasErrors(),
                 false);
        auto members = ParseIntrinsicSource(prefix + "func Main() { let n = \"hello\".length; }");
        CHECK_FALSE(SemanticAnalyzer({&members.module}, {{"Text", {{"text.rux", &dependency.module}}}}, "App")
                        .Analyze()
                        .HasErrors());
    }
}

TEST_CASE("an intrinsic type import in another file does not expose its members") {
    auto dependency = ParseIntrinsicSource(IntegerDeclaration + StringDeclaration, "provider.rux");
    auto imported = ParseIntrinsicSource("import Provider::{ int8, Utf8 };", "imported.rux");
    auto isolated = ParseIntrinsicSource("func Main() -> int8 { return int8::Max; }", "isolated.rux");
    const auto model = SemanticAnalyzer({&imported.module, &isolated.module},
                                        {{"Provider", {{"provider.rux", &dependency.module}}}}, "App")
                           .Analyze();
    CHECK(model.HasErrors());
}

TEST_CASE("native slices and ordinary aggregates have different indexing behavior") {
    auto native = ParseIntrinsicSource("func Read(values: int8[..]) -> int8 { return values[0]; }");
    CHECK_FALSE(SemanticAnalyzer({&native.module}).Analyze().HasErrors());
    auto ordinary = ParseIntrinsicSource(R"(
struct UserView<T> { pub data: *T; pub length: uint; }
func Read(values: UserView<int8>) -> int8 { return values[0]; }
)");
    CHECK(SemanticAnalyzer({&ordinary.module}).Analyze().HasErrors());
}

TEST_CASE("range syntax does not convert to an ordinary bounds struct") {
    auto parsed = ParseIntrinsicSource(R"(
struct Bounds<T> { pub start: T; pub end: T; }
func Make() -> Bounds<int> { return 0..2; }
)");
    CHECK(SemanticAnalyzer({&parsed.module}).Analyze().HasErrors());
}

TEST_CASE("intrinsic slice identity survives generic lowering") {
    auto parsed = ParseIntrinsicSource(R"(
func First<T>(values: T[..]) -> T { return values[0]; }
func Read(values: int8[..]) -> int8 { return First(values); }
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

TEST_CASE("a forward string constant validates its annotation after imports") {
    auto provider = ParseIntrinsicSource(StringDeclaration, "provider.rux");
    auto consumer = ParseIntrinsicSource(R"(
import Provider::Utf8;
const Greeting: Utf8 = "hello";
func Main() -> uint { return Greeting.length; }
)");
    const auto model =
        SemanticAnalyzer({&consumer.module}, {{"Provider", {{"provider.rux", &provider.module}}}}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    CHECK(AstToHirLowering(model).Generate().modules.size() == 2);
}

TEST_CASE("a string constant annotation without a declaration is rejected after indexing") {
    auto parsed = ParseIntrinsicSource(R"(
const Greeting: string = "hello";
func Main() {}
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE(model.HasErrors());
    bool missing = false;
    for (const auto &diagnostic : model.diagnostics) {
        missing |= diagnostic.message.contains("'string'");
    }
    CHECK(missing);
}

TEST_CASE("extension signatures resolve string annotations in their owning file before calls") {
    auto provider = ParseIntrinsicSource(StringDeclaration, "provider.rux");
    auto caller = ParseIntrinsicSource(R"(
func Main() -> int { return Factory::Create("hello"); }
)",
                                       "caller.rux");
    auto implementation = ParseIntrinsicSource(R"(
import Provider::Utf8;
struct Factory {}
extend Factory {
    pub func Create(text: char8[..]) -> int { return text.length as int; }
}
)",
                                               "factory.rux");
    const auto model = SemanticAnalyzer({&caller.module, &implementation.module},
                                        {{"Provider", {{"provider.rux", &provider.module}}}}, "App")
                           .Analyze();
    REQUIRE_FALSE(model.HasErrors());
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("ordinary declarations cannot replace reserved primitive names") {
    for (const std::string source : {"struct float128 {}", "type int8 = int32;"}) {
        auto parsed = ParseIntrinsicSource(source);
        const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
        CHECK(model.HasErrors());
    }
}

TEST_CASE("wide extrema are evaluated from declarations and recorded for lowering") {
    for (const unsigned width : {128U, 256U, 512U}) {
        const std::string signedName = "int" + std::to_string(width);
        const std::string unsignedName = "uint" + std::to_string(width);
        const std::string minimum = "-" + WideInteger::MinMagnitude(width, true).ToDecimal();
        const std::string maximum = WideInteger::MaxValue(width, true).ToDecimal();
        const std::string unsignedMaximum = WideInteger::AllOnes(width).ToDecimal();
        auto parsed = ParseIntrinsicSource(
            "intrinsic type " + signedName + "; intrinsic type " + unsignedName + "; " + "extend " + signedName +
            " { pub const Min: " + signedName + " = " + minimum + "i" + std::to_string(width) +
            "; pub const Max: " + signedName + " = " + maximum + "i" + std::to_string(width) + "; } " + "extend " +
            unsignedName + " { pub const Max: " + unsignedName + " = " + unsignedMaximum + "u" + std::to_string(width) +
            "; } " + "when " + signedName + "::Min < 0 && " + signedName + "::Max > 0 && " + unsignedName + "::Max > " +
            signedName + "::Max && " + signedName + "::Max < " + unsignedName + "::Max && " + signedName + "::Min < " +
            unsignedName + "::Max { " + "func Value() -> " + signedName + " { return " + signedName +
            "::Min; } } else { func Bad() { Missing(); } }");
        const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
        REQUIRE_FALSE(model.HasErrors());
        const auto *extension = dynamic_cast<const ImplDecl *>(parsed.module.items[2].get());
        REQUIRE(extension);
        const auto *value = model.TryGetConstantValue(*extension->constants[0]);
        REQUIRE(value);
        CHECK_EQ(value->literal, minimum);
        (void)AstToHirLowering(model).Generate();
    }
}

TEST_CASE("inferred range and slice fields need no declaration") {
    // Both view and range fields are owned by the compiler.
    auto missing = ParseIntrinsicSource("func Main() { let range = 1..3; let start = range.start; }");
    CHECK_FALSE(SemanticAnalyzer({&missing.module}, {}, "App").Analyze().HasErrors());
    auto slice =
        ParseIntrinsicSource("func Main() { let array = [1, 2]; let slice = array[0..1]; let length = slice.length; }");
    CHECK_FALSE(SemanticAnalyzer({&slice.module}, {}, "App").Analyze().HasErrors());
    auto parsed = ParseIntrinsicSource(R"(
func Main() { let range = 1..3; let start = range.start;
    let array = [1, 2]; let slice = array[0..1]; let length = slice.length; }
)");
    CHECK_FALSE(SemanticAnalyzer({&parsed.module}, {}, "App").Analyze().HasErrors());
}

TEST_CASE("conditional constant evaluation retains earlier declarations while folding") {
    auto parsed = ParseIntrinsicSource(R"(
const Base: int8 = 126i8;
intrinsic type int8;
extend int8 { pub const Max: int8 = Base + 1i8; }
when int8::Max == 127i8 { func Main() -> int8 { return int8::Max; } }
else { func Main() { Missing(); } }
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *extension = dynamic_cast<const ImplDecl *>(parsed.module.items[2].get());
    REQUIRE(extension);
    REQUIRE(model.TryGetConstantValue(*extension->constants[0]));
    CHECK_EQ(model.TryGetConstantValue(*extension->constants[0])->literal, "127");
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("intrinsic bindings reject alias spellings and mismatched special float owners") {
    for (const std::string source :
         {"intrinsic type float;", "intrinsic struct string { pub data: *char8; pub length: uint; }",
          "intrinsic type int8; extend int8 { pub intrinsic const Infinity: float64; }"}) {
        auto tokens = Lexer(source, "removed.rux").Tokenize();
        auto parsed = Parser(std::move(tokens.tokens), "removed.rux").Parse();
        CHECK((parsed.HasErrors() || SemanticAnalyzer({&parsed.module}, {}, "App").Analyze().HasErrors()));
    }
}

TEST_CASE("local aliases retain their provider identity for literal fields") {
    auto provider = ParseIntrinsicSource(StringDeclaration, "provider.rux");
    auto parsed = ParseIntrinsicSource(R"(
import Provider::Utf8;
type Text = Utf8;
func Main() -> uint { let value: Text = "hello"; return value.length; }
)");
    const auto model =
        SemanticAnalyzer({&parsed.module}, {{"Provider", {{"provider.rux", &provider.module}}}}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("ordinary names do not inherit intrinsic layout or copy properties") {
    auto parsed = ParseIntrinsicSource(R"(
struct UserView<T> { pub value: T; }
struct SystemTime { pub value: uint8; }
struct StringArray { pub value: uint8; }
func Main() -> uint { return sizeof(UserView<uint8>) + sizeof(SystemTime) + sizeof(StringArray); }
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *layout = model.TryGetLayout(TypeRef::MakeNamed("UserView<uint8>"));
    REQUIRE(layout);
    CHECK_EQ(layout->size, 1);
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("selected extension constants become visible to later conditions") {
    auto parsed = ParseIntrinsicSource(R"(
intrinsic type int8;
when true { extend int8 { pub const Max: int8 = 127i8; } }
when false { extend int8 { pub const Max: int8 = 2i8; } }
when int8::Max == 127i8 { func Main() -> int8 { return int8::Max; } }
else { func Main() { Missing(); } }
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("aliases exported through another provider retain associated declarations") {
    auto original = ParseIntrinsicSource(IntegerDeclaration, "original.rux");
    auto aliases = ParseIntrinsicSource("import Original::int8; pub type Tiny = int8;", "aliases.rux");
    auto parsed = ParseIntrinsicSource(R"(
import Aliases::Tiny;
when Tiny::Max == 127i8 { func Main() -> int8 { return Tiny::Min; } }
else { func Main() { Missing(); } }
)");
    const auto model = SemanticAnalyzer({&parsed.module},
                                        {{"Original", {{"original.rux", &original.module}}},
                                         {"Aliases", {{"aliases.rux", &aliases.module}}}},
                                        "App")
                           .Analyze();
    REQUIRE_FALSE(model.HasErrors());
    (void)AstToHirLowering(model).Generate();
}

TEST_CASE("scalar intrinsic type facts retain the owning declaration") {
    auto parsed = ParseIntrinsicSource(IntegerDeclaration + "func End(value: int8) -> int8 { return value; }");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.back().get());
    REQUIRE(function);
    const Decl *binding = model.TryGetIntrinsicTypeBinding(*function->params[0].type);
    REQUIRE(binding);
    CHECK(binding == parsed.module.items[0].get());
}

TEST_CASE("duplicate extension constants and reserved-width APIs are rejected") {
    auto duplicate = ParseIntrinsicSource(IntegerDeclaration + "extend int8 { pub const Max: int8 = 2i8; }");
    CHECK(SemanticAnalyzer({&duplicate.module}, {}, "App").Analyze().HasErrors());
    auto reserved = ParseIntrinsicSource("func Main() { let value = float128::Max; }");
    const auto model = SemanticAnalyzer({&reserved.module}, {}, "App").Analyze();
    REQUIRE(model.HasErrors());
    CHECK(model.diagnostics[0].message.contains("reserved but is not implemented"));
}

TEST_CASE("ordinary generic Slice methods retain ordinary receiver identity") {
    auto parsed = ParseIntrinsicSource(R"(
struct UserView<T> { pub value: T; }
extend UserView<T> { pub func Value(self: &UserView<T>) -> T { return self.value; } }
func Main() -> uint8 { let value = UserView<uint8> { value: 7u8 }; return value.Value(); }
)");
    const auto model = SemanticAnalyzer({&parsed.module}, {}, "App").Analyze();
    REQUIRE_FALSE(model.HasErrors());
    (void)AstToHirLowering(model).Generate();
}
