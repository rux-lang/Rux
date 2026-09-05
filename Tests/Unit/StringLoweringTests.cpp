#include "IntrinsicTestDeclarations.h"
// What a string literal becomes: the address of its transcoded data, and a length counted in the code units of its
// own encoding rather than in the bytes of the source spelling.

#include "Ir/Lir/Lir.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
LirPackage CompileToLir(const std::string &source) {
    Lexer lexer(source + std::string(Rux::Testing::StringDeclarations), "strings.rux");
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
        func Eight() { let text = s8"A\u{A2}\u{20AC}\u{1F680}"; }
        func Sixteen() { let text = s16"A\u{A2}\u{20AC}\u{1F680}"; }
        func ThirtyTwo() { let text = s32"A\u{A2}\u{20AC}\u{1F680}"; }
    )");

    CHECK_EQ(Constants(RequireFunction(package, "Eight")), std::vector<std::string>{"10"});
    CHECK_EQ(Constants(RequireFunction(package, "Sixteen")), std::vector<std::string>{"5"});
    CHECK_EQ(Constants(RequireFunction(package, "ThirtyTwo")), std::vector<std::string>{"4"});
}

TEST_CASE("a string literal's data is requested at the encoding's own character") {
    const LirPackage package = CompileToLir(R"(
        func Eight() { let text = s8"text"; }
        func Sixteen() { let text = s16"text"; }
        func ThirtyTwo() { let text = s32"text"; }
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
            let text = s8"text";
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
