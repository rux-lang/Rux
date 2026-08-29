#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>

using namespace Rux;

namespace {
LirPackage LowerVariantSource(const std::string &source, const TargetContext target = TargetContext::CreateNative()) {
    Lexer lexer(source, "variant-equality.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variant-equality.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    CompileTimeContext context;
    context.target = target;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), target);
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &Function(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing function " << name);
    throw std::runtime_error("missing lowered function");
}

std::size_t CountOpcode(const LirFunc &function, const LirOpcode opcode) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        count += std::ranges::count_if(block.instrs,
                                       [opcode](const LirInstr &instruction) { return instruction.op == opcode; });
    }
    return count;
}

std::size_t CountCall(const LirFunc &function, const std::string &callee) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        count += std::ranges::count_if(block.instrs, [&](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Call && instruction.strArg == callee;
        });
    }
    return count;
}

bool HasBlock(const LirFunc &function, const std::string &label) {
    return std::ranges::any_of(function.blocks, [&](const LirBlock &block) { return block.label.starts_with(label); });
}

TargetContext AArch64Target() {
    return {.os = Target::OS::Linux,
            .arch = Target::Arch::AArch64,
            .data_model = Target::DataModel::LP64,
            .abi = Target::ABI::AAPCS64,
            .default_cc = Target::CallingConv::AAPCS64,
            .endianness = Target::Endian::Little,
            .object_format = Target::ObjectFormat::ELF,
            .pointer_size = 8,
            .cpu_features = Target::CpuFeature::NEON};
}

const std::string kParseErrorSource = R"(
    variant ParseError {
        End,
        InvalidCharacter(int32),
        Pair(int32, int32)
    }
    func Left() -> ParseError { return ParseError::InvalidCharacter(3i32); }
    func Right() -> ParseError { return ParseError::InvalidCharacter(4i32); }
    func Equal() -> bool { return Left() == Right(); }
    func Different() -> bool { return Left() != Right(); }
)";
} // namespace

TEST_CASE("variant equality branches by tag before comparing the active payload") {
    const LirPackage package = LowerVariantSource(kParseErrorSource);
    const LirFunc &equal = Function(package, "Equal");
    CHECK(HasBlock(equal, "variant.eq.shared-tag"));
    CHECK(HasBlock(equal, "variant.eq.different-tag"));
    CHECK(HasBlock(equal, "variant.eq.case"));
    CHECK_GE(CountOpcode(equal, LirOpcode::CmpEq), 4);
    CHECK_GE(CountOpcode(equal, LirOpcode::IndexPtr), 2);
    CHECK_EQ(CountCall(equal, "Left"), 1);
    CHECK_EQ(CountCall(equal, "Right"), 1);
}

TEST_CASE("variant inequality reuses equality traversal and negates its result") {
    const LirPackage package = LowerVariantSource(kParseErrorSource);
    const LirFunc &different = Function(package, "Different");
    CHECK(HasBlock(different, "variant.eq.merge"));
    CHECK_EQ(CountOpcode(different, LirOpcode::Not), 1);
    CHECK_EQ(CountCall(different, "Left"), 1);
    CHECK_EQ(CountCall(different, "Right"), 1);
}

TEST_CASE("nested variant and custom payload equality lower recursively") {
    const LirPackage package = LowerVariantSource(R"(
        struct Label { value: int32; }
        extend Label {
            func ==(self: &Label, other: Label) -> bool { return self.value == other.value; }
        }
        variant Inner { None, Number(int32) }
        variant Outer { Empty, Nested(Inner), Named { label: Label; } }
        func Equal(left: Outer, right: Outer) -> bool { return left == right; }
    )");
    const LirFunc &equal = Function(package, "Equal");
    CHECK_GE(CountOpcode(equal, LirOpcode::Phi), 2);
    CHECK_EQ(CountCall(equal, "Label::=="), 1);
    CHECK_GE(CountOpcode(equal, LirOpcode::IndexPtr), 4);
}

TEST_CASE("variant equality control flow is target-independent through AArch64 lowering") {
    const LirPackage package = LowerVariantSource(kParseErrorSource, AArch64Target());
    const LirFunc &equal = Function(package, "Equal");
    CHECK(HasBlock(equal, "variant.eq.shared-tag"));
    CHECK_GE(CountOpcode(equal, LirOpcode::Phi), 1);
    CHECK_GE(CountOpcode(equal, LirOpcode::CmpEq), 4);
}
