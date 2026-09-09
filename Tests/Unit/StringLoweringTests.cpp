// What a string literal becomes: the address of its transcoded data, and a length counted in the code units of its
// own encoding rather than in the bytes of the source spelling.

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
#include <vector>

using namespace Rux;

namespace {
LirPackage CompileToLir(const std::string &source) {
    Lexer lexer(source, "strings.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "strings.rux");
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

/// The string-literal data addresses a function takes, paired with the pointer type each was requested at, whose
/// pointee is the encoding's character.
std::vector<std::pair<std::string, std::string>> StringAddresses(const LirFunc &function) {
    std::vector<std::pair<std::string, std::string>> found;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::StringAddr) {
                found.emplace_back(instruction.strArg, instruction.type.ToString());
            }
        }
    }
    return found;
}

/// The constants a function materializes, in order, which is where a literal's published length shows up.
std::vector<std::string> Constants(const LirFunc &function) {
    std::vector<std::string> found;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Const) {
                found.push_back(instruction.strArg);
            }
        }
    }
    return found;
}
} // namespace

TEST_CASE("a string literal publishes its length in its own code units") {
    // Four code points spelled in ten UTF-8 bytes: 'A', U+00A2, U+20AC, and U+1F680, the last of which needs a
    // surrogate pair in UTF-16. So the same text is 10, 5, and 4 units wide in the three encodings.
    const LirPackage package = CompileToLir(R"(
        func Eight() { let text = c8"A\u{A2}\u{20AC}\u{1F680}"; }
        func Sixteen() { let text = c16"A\u{A2}\u{20AC}\u{1F680}"; }
        func ThirtyTwo() { let text = c32"A\u{A2}\u{20AC}\u{1F680}"; }
    )");

    CHECK_EQ(Constants(RequireFunction(package, "Eight")), std::vector<std::string>{"10"});
    CHECK_EQ(Constants(RequireFunction(package, "Sixteen")), std::vector<std::string>{"5"});
    CHECK_EQ(Constants(RequireFunction(package, "ThirtyTwo")), std::vector<std::string>{"4"});
}

TEST_CASE("a string literal's data is requested at the encoding's own character") {
    const LirPackage package = CompileToLir(R"(
        func Eight() { let text = c8"text"; }
        func Sixteen() { let text = c16"text"; }
        func ThirtyTwo() { let text = c32"text"; }
    )");

    const auto eight = StringAddresses(RequireFunction(package, "Eight"));
    const auto sixteen = StringAddresses(RequireFunction(package, "Sixteen"));
    const auto thirtyTwo = StringAddresses(RequireFunction(package, "ThirtyTwo"));
    REQUIRE_EQ(eight.size(), 1);
    REQUIRE_EQ(sixteen.size(), 1);
    REQUIRE_EQ(thirtyTwo.size(), 1);

    // The value carried into code generation is the literal's UTF-8 text; the pointee of the address's type is what
    // says which encoding it is emitted in.
    CHECK_EQ(eight[0].first, "text");
    CHECK_EQ(eight[0].second, "*char8");
    CHECK_EQ(sixteen[0].second, "*char16");
    CHECK_EQ(thirtyTwo[0].second, "*char32");
}

TEST_CASE("a string's members read the same two fields a slice's do") {
    const LirPackage package = CompileToLir(R"(
        func Main() {
            let text = c8"text";
            let data = text.data;
            let length = text.length;
            let first = text[0];
        }
    )");

    const LirFunc &main = RequireFunction(package, "Main");
    std::vector<std::string> fields;
    for (const LirBlock &block : main.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::FieldPtr) {
                fields.push_back(instruction.strArg);
            }
        }
    }
    // The literal's own two stores, then the reads of each member, then the data pointer the index goes through.
    CHECK_EQ(fields, std::vector<std::string>{"data", "length", "data", "length", "data"});
}

TEST_CASE("weakening a returned view preserves its aggregate representation") {
    const LirPackage package = CompileToLir(R"(
        func Weaken(data: *var int, count: uint) -> int[..] {
            return data[..count];
        }
        func Forward(view: var int[..]) -> int[..] { return view; }
    )");
    for (const std::string name : {"Weaken", "Forward"}) {
        const LirFunc &function = RequireFunction(package, name);
        CHECK(function.returnType.IsSlice());
        for (const LirBlock &block : function.blocks) {
            for (const LirInstr &instruction : block.instrs) {
                CHECK_FALSE((instruction.op == LirOpcode::Cast && instruction.type.IsSlice()));
            }
        }
    }
}

TEST_CASE("char64 literals lower as full-width decoded scalar constants") {
    const LirPackage package = CompileToLir(R"(
        const Face = c64'\u{1F600}';
        const Last: char64 = c64'\u{10FFFF}';
        func Ascii() -> char64 { return c64'A'; }
        func Supplementary() -> char64 { return Face; }
        func Boundary() -> char64 { return Last; }
        func Literal() -> char64 { return c64'😀'; }
        func Escaped() -> char64 { return c64'\n'; }
    )");
    for (const auto &[name, value] : std::vector<std::pair<std::string, std::string>>{{"Ascii", "65"},
                                                                                      {"Supplementary", "128512"},
                                                                                      {"Boundary", "1114111"},
                                                                                      {"Literal", "128512"},
                                                                                      {"Escaped", "10"}}) {
        CAPTURE(name);
        const auto &function = RequireFunction(package, name);
        CHECK_EQ(function.returnType.kind, TypeRef::Kind::Char64);
        CHECK_EQ(Constants(function), std::vector<std::string>{value});
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Const) {
                    CHECK_EQ(instruction.type.kind, TypeRef::Kind::Char64);
                }
            }
        }
    }
}

TEST_CASE("character patterns lower decoded code points at every width") {
    const LirPackage package = CompileToLir(R"(
        func Byte(value: char8) -> int { return match value { c8'A' => 1, else => 2 }; }
        func Unit(value: char16) -> int { return match value { c16'λ' => 1, else => 2 }; }
        func Scalar(value: char32) -> int { return match value { c32'\u{1F600}' => 1, else => 2 }; }
        func Wide(value: char64) -> int { return match value { c64'😀' => 1, else => 2 }; }
    )");
    for (const auto &[name, expected] : std::vector<std::pair<std::string, std::string>>{
             {"Byte", "65"}, {"Unit", "955"}, {"Scalar", "128512"}, {"Wide", "128512"}}) {
        CAPTURE(name);
        const auto values = Constants(RequireFunction(package, name));
        CHECK(std::find(values.begin(), values.end(), expected) != values.end());
        for (const auto &value : values) {
            CHECK(value.find('\'') == std::string::npos);
        }
    }
}

TEST_CASE("equivalent character spellings diagnose duplicate match patterns") {
    for (const std::string prefix : {"c8", "c16", "c32", "c64"}) {
        CAPTURE(prefix);
        const std::string type = "char" + prefix.substr(1);
        const std::string source = "func Select(value: " + type + ") -> int { return match value { " + prefix +
                                   "'A' => 1, " + prefix + R"('\u{41}' => 2, else => 3 }; })";
        Lexer lexer(source, "characters.rux");
        auto lexed = lexer.Tokenize();
        REQUIRE_FALSE(lexed.HasErrors());
        Parser parser(std::move(lexed.tokens), "characters.rux");
        auto parsed = parser.Parse();
        REQUIRE_FALSE(parsed.HasErrors());
        SemanticAnalyzer analyzer({&parsed.module}, {}, "test", CompileTimeContext{});
        const SemanticModel model = analyzer.Analyze();
        REQUIRE(model.HasErrors());
        CHECK(std::any_of(model.diagnostics.begin(), model.diagnostics.end(), [](const Diagnostic &diagnostic) {
            return diagnostic.message.find("duplicate pattern in match") != std::string::npos;
        }));
    }
}
