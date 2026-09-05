#include "AArch64RcuEmitterTestSupport.h"

#include "Driver/BuildTarget.h"
#include "IntrinsicTestDeclarations.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/Model/CompileTimeContext.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <format>

namespace Rux::Testing {
LirPackage CompileToAArch64Lir(const std::string &source, const std::string_view targetTriple) {
    CAPTURE(targetTriple);

    Lexer lexer(source + std::string(Rux::Testing::StringDeclarations), "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(targetTriple));
    Parser parser(std::move(lexed.tokens), "test.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    // The whole front end is told which machine this is for, not just the
    // parser: an `asm func` body is checked against the target's own
    // instruction set, so a semantic stage left on the host would refuse every
    // AArch64 mnemonic that is not also an x86-64 one.
    CompileTimeContext context;
    context.target = target;
    context.targetTriple = targetTriple;

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", context);
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto hirPackage = hirLowering.Generate();
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(hirPackage).reachedFixedPoint);

    HirToLirLowering lirLowering(std::move(hirPackage), target);
    return lirLowering.Generate();
}

std::string HexWord(const std::uint32_t word) {
    return std::format("0x{:08X}", word);
}

std::vector<std::uint32_t> FunctionWords(const RcuFile &object, const std::string_view name) {
    const auto symbol =
        std::ranges::find_if(object.symbols, [name](const RcuSymbol &candidate) { return candidate.name == name; });
    REQUIRE_MESSAGE(symbol != object.symbols.end(), name);
    REQUIRE_MESSAGE(symbol->sectionIdx < object.sections.size(), name);

    const auto &text = object.sections[symbol->sectionIdx].data;
    REQUIRE_MESSAGE(symbol->value + symbol->size <= text.size(), name);
    REQUIRE_MESSAGE(symbol->size % 4 == 0, name);

    std::vector<std::uint32_t> words;
    for (std::uint32_t offset = symbol->value; offset < symbol->value + symbol->size; offset += 4) {
        words.push_back(static_cast<std::uint32_t>(text[offset]) | static_cast<std::uint32_t>(text[offset + 1]) << 8U |
                        static_cast<std::uint32_t>(text[offset + 2]) << 16U |
                        static_cast<std::uint32_t>(text[offset + 3]) << 24U);
    }
    return words;
}

std::optional<std::int64_t> StackPointerAdjustment(const std::uint32_t word, const bool subtract) {
    const std::uint32_t opcode = subtract ? 0xD1000000U : 0x91000000U;
    if ((word & 0xFF800000U) != opcode || (word & 0x3FFU) != 0x3FFU) {
        return std::nullopt;
    }
    const std::int64_t imm12 = (word >> 10U) & 0xFFFU;
    return (word & (1U << 22U)) != 0 ? imm12 << 12U : imm12;
}

const RcuSymbol *FindSymbol(const RcuFile &object, const std::string_view name) {
    const auto found =
        std::ranges::find_if(object.symbols, [name](const RcuSymbol &symbol) { return symbol.name == name; });
    return found == object.symbols.end() ? nullptr : &*found;
}

std::vector<std::uint8_t> RodataOf(const RcuFile &object, const std::string_view name) {
    const RcuSymbol *symbol = FindSymbol(object, name);
    REQUIRE_MESSAGE(symbol != nullptr, name);
    REQUIRE_EQ(symbol->sectionIdx, RCU_RODATA_IDX);
    const auto &rodata = object.sections[RCU_RODATA_IDX].data;
    REQUIRE(symbol->value + symbol->size <= rodata.size());
    return {rodata.begin() + symbol->value, rodata.begin() + symbol->value + symbol->size};
}

std::vector<RcuReloc> RelocsFor(const RcuFile &object, const std::uint16_t sectionIdx, const std::string_view symbol) {
    std::vector<RcuReloc> found;
    for (const auto &reloc : object.sections[sectionIdx].relocs) {
        if (reloc.symbolIndex < object.symbols.size() && object.symbols[reloc.symbolIndex].name == symbol) {
            found.push_back(reloc);
        }
    }
    return found;
}

std::uint32_t TextWordAt(const RcuFile &object, const std::uint32_t offset) {
    const auto &text = object.sections[RCU_TEXT_IDX].data;
    REQUIRE_MESSAGE(offset + 4 <= text.size(), offset);
    return static_cast<std::uint32_t>(text[offset]) | static_cast<std::uint32_t>(text[offset + 1]) << 8U |
           static_cast<std::uint32_t>(text[offset + 2]) << 16U | static_cast<std::uint32_t>(text[offset + 3]) << 24U;
}

std::optional<std::uint32_t> FramePointerAddImm(const std::uint32_t word, const unsigned reg) {
    if ((word & 0xFFC003FFU) != (0x910003A0U | reg)) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

bool HasFloatForm(const std::vector<std::uint32_t> &words, const std::uint32_t opcode, const unsigned sources) {
    const std::uint32_t mask = sources == 2 ? 0xFFE0FC00U : 0xFFFFFC00U;
    return std::ranges::any_of(words, [mask, opcode](const std::uint32_t word) { return (word & mask) == opcode; });
}

bool HasCset(const std::vector<std::uint32_t> &words, const std::uint32_t cset) {
    return std::ranges::any_of(words, [cset](const std::uint32_t word) { return (word & 0xFFFFFFE0U) == cset; });
}

bool HasRegisterForm(const std::vector<std::uint32_t> &words, const std::uint32_t opcode) {
    return std::ranges::any_of(words, [opcode](const std::uint32_t word) { return (word & 0xFFE0FC00U) == opcode; });
}

std::optional<std::uint32_t> FieldAddImm(const std::uint32_t word) {
    const std::uint32_t base = word >> 5U & 31U;
    if ((word & 0xFFC00000U) != 0x91000000U || base == 29 || base == 31) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

std::optional<std::uint32_t> StackPointerAddImm(const std::uint32_t word, const unsigned reg) {
    if ((word & 0xFFC003FFU) != (0x910003E0U | reg)) {
        return std::nullopt;
    }
    return word >> 10U & 0xFFFU;
}

bool IsPairAccess(const std::uint32_t word) {
    return (word & 0xFE000000U) == 0xA8000000U;
}

std::int32_t BranchDisplacement(const std::uint32_t word) {
    if ((word & 0xFC000000U) == 0x14000000U) {
        return static_cast<std::int32_t>((word & 0x03FFFFFFU) << 6U) >> 6;
    }
    return static_cast<std::int32_t>((word >> 5U & 0x7FFFFU) << 13U) >> 13;
}

std::optional<std::uint32_t> SlotDisplacement(const std::uint32_t word, const bool store) {
    if ((word & 0xFFC003FFU) != (store ? 0xF90003A9U : 0xF94003A9U)) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * 8U;
}

std::optional<std::size_t> BranchAndLinkIndex(const std::vector<std::uint32_t> &words) {
    for (std::size_t i = 0; i < words.size(); ++i) {
        if ((words[i] & 0xFC000000U) == 0x94000000U) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::size_t> BranchAndLinkIndices(const std::vector<std::uint32_t> &words) {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < words.size(); ++i) {
        if ((words[i] & 0xFC000000U) == 0x94000000U) {
            result.push_back(i);
        }
    }
    return result;
}

std::optional<unsigned> ArgumentFilled(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) == 0xF94003A0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFC003E0U) == 0xB94003A0U || (word & 0xFFC003E0U) == 0xB98003A0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFE0FFE0U) == 0xAA0003E0U || (word & 0xFFE0FFE0U) == 0x2A0003E0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFFFFC00U) == 0x93407C00U) {
        return word & 0x1FU;
    }
    return std::nullopt;
}

std::optional<unsigned> ArgumentDrained(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) == 0xF90003A0U) {
        return word & 0x1FU;
    }
    if ((word & 0xFFE0FFE0U) == 0xAA0003E0U) {
        return word >> 16U & 0x1FU;
    }
    return std::nullopt;
}

std::uint32_t SlotAccessDisplacement(const std::uint32_t word) {
    return (word >> 10U & 0xFFFU) * 8U;
}

std::optional<VectorSlotAccess> VectorSlotAccessOf(const std::uint32_t word, const unsigned bits, const bool store) {
    const std::uint32_t opcode = (bits == 64 ? 0xFD0003A0U : 0xBD0003A0U) | (store ? 0U : 0x00400000U);
    if ((word & 0xFFC003E0U) != opcode) {
        return std::nullopt;
    }
    return VectorSlotAccess{word & 0x1FU, (word >> 10U & 0xFFFU) * (bits / 8U)};
}

std::optional<unsigned> VectorArgumentFilled(const std::uint32_t word, const unsigned bits) {
    if (const auto access = VectorSlotAccessOf(word, bits, false)) {
        return access->reg;
    }
    if ((word & 0xFFFFFC00U) == (bits == 64 ? 0x1E604000U : 0x1E204000U)) {
        return word & 0x1FU;
    }
    return std::nullopt;
}

std::optional<unsigned> FloatBitsArgumentFilled(const std::uint32_t word, const unsigned bits) {
    const std::uint32_t opcode = bits == 64 ? 0x9E660000U : 0x1E260000U;
    return (word & 0xFFFFFC00U) == opcode ? std::optional<unsigned>(word & 0x1FU) : std::nullopt;
}

std::optional<std::uint32_t> StackArgumentStored(const std::uint32_t word, const unsigned width) {
    const std::uint32_t opcode = width == 1 ? 0x390003E9U
                               : width == 2 ? 0x790003E9U
                               : width == 4 ? 0xB90003E9U
                                            : 0xF90003E9U;
    if ((word & 0xFFC003FFU) != opcode) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * width;
}

std::optional<std::uint32_t> StackArgumentStoredBy(const std::uint32_t word, const unsigned width, const unsigned reg) {
    const std::uint32_t opcode = width == 1 ? 0x390003E0U
                               : width == 2 ? 0x790003E0U
                               : width == 4 ? 0xB90003E0U
                                            : 0xF90003E0U;
    if ((word & 0xFFC003E0U) != opcode || (word & 0x1FU) != reg) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * width;
}

std::optional<std::uint32_t> VectorStackArgumentStored(const std::uint32_t word, const unsigned bits) {
    const std::uint32_t opcode = bits == 32 ? 0xBD0003E0U : 0xFD0003E0U;
    if ((word & 0xFFC003E0U) != opcode) {
        return std::nullopt;
    }
    return (word >> 10U & 0xFFFU) * (bits / 8U);
}

std::optional<std::uint32_t> StackPairArgumentStored(const std::uint32_t word) {
    if ((word & 0xFFC003E0U) != 0xA90003E0U) {
        return std::nullopt;
    }
    return (word >> 15U & 0x7FU) * 8U;
}

std::optional<unsigned> VectorArgumentDrained(const std::uint32_t word, const unsigned bits) {
    if (const auto access = VectorSlotAccessOf(word, bits, true)) {
        return access->reg;
    }
    if ((word & 0xFFFFFC00U) == (bits == 64 ? 0x1E604000U : 0x1E204000U)) {
        return word >> 5U & 0x1FU;
    }
    return std::nullopt;
}

std::size_t DrainStart(const std::vector<std::uint32_t> &words) {
    const auto found = std::ranges::find_if(
        words, [](const std::uint32_t word) { return ArgumentDrained(word) == std::optional<unsigned>(0); });
    return static_cast<std::size_t>(found - words.begin());
}

std::size_t VectorDrainStart(const std::vector<std::uint32_t> &words, const unsigned bits) {
    const auto found = std::ranges::find_if(words, [bits](const std::uint32_t word) {
        return VectorArgumentDrained(word, bits) == std::optional<unsigned>(0);
    });
    return static_cast<std::size_t>(found - words.begin());
}

std::optional<std::int32_t> PreIndexedFrameSize(const std::uint32_t word) {
    constexpr std::uint32_t registers = 30U << 10U | 31U << 5U | 29U;
    if ((word & 0xFFC00000U) != 0xA9800000U || (word & 0x7FFFU) != registers) {
        return std::nullopt;
    }
    const auto imm7 = static_cast<std::int32_t>((word >> 15U & 0x7FU) << 25U) >> 25;
    return -imm7 * 8;
}

std::optional<std::int32_t> IncomingDisplacement(const std::uint32_t word, const unsigned width, const bool sign) {
    const std::uint32_t opcode = width == 1 ? (sign ? 0x398003A9U : 0x394003A9U)
                               : width == 2 ? (sign ? 0x798003A9U : 0x794003A9U)
                               : width == 4 ? (sign ? 0xB98003A9U : 0xB94003A9U)
                                            : 0xF94003A9U;
    if ((word & 0xFFC003FFU) != opcode) {
        return std::nullopt;
    }
    return static_cast<std::int32_t>((word >> 10U & 0xFFFU) * width);
}

PreservedRegisters SavedRegisters(const std::vector<std::uint32_t> &words) {
    PreservedRegisters saved;
    for (std::size_t i = 2; i < words.size(); ++i) {
        const std::uint32_t word = words[i];
        if ((word & 0xFFC003E0U) == 0xF90003A0U) {
            saved.general.push_back(word & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0xA90003A0U) {
            saved.general.push_back(word & 31U);
            saved.general.push_back(word >> 10U & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0xFD0003A0U) {
            saved.vector.push_back(word & 31U);
        }
        else if ((word & 0xFFC003E0U) == 0x6D0003A0U) {
            saved.vector.push_back(word & 31U);
            saved.vector.push_back(word >> 10U & 31U);
        }
        else {
            break;
        }
    }
    return saved;
}

bool RestoresRegister(const std::vector<std::uint32_t> &words, const unsigned reg, const bool isVector) {
    const std::uint32_t single = isVector ? 0xFD4003A0U : 0xF94003A0U;
    const std::uint32_t pair = isVector ? 0x6D4003A0U : 0xA94003A0U;
    return std::ranges::any_of(words, [reg, single, pair](const std::uint32_t word) {
        if ((word & 0xFFC003E0U) == single) {
            return (word & 31U) == reg;
        }
        if ((word & 0xFFC003E0U) == pair) {
            return (word & 31U) == reg || (word >> 10U & 31U) == reg;
        }
        return false;
    });
}

LirPackage PackageOf(LirFunc func) {
    LirModule module;
    module.name = "test.rux";
    module.funcs.push_back(std::move(func));
    LirPackage package;
    package.modules.push_back(std::move(module));
    return package;
}

LirInstr ConstInstr(const LirReg dst, const std::string_view value, TypeRef type) {
    LirInstr instr;
    instr.op = LirOpcode::Const;
    instr.dst = dst;
    instr.type = std::move(type);
    instr.strArg = value;
    return instr;
}

LirInstr PhiInstr(const LirReg dst, const std::vector<std::pair<LirReg, std::uint32_t>> &preds) {
    LirInstr instr;
    instr.op = LirOpcode::Phi;
    instr.dst = dst;
    instr.type = TypeRef::MakeInt64();
    instr.phiPreds = preds;
    return instr;
}

LirTerminator ReturnTerm(const LirReg value) {
    LirTerminator term;
    term.kind = LirTermKind::Return;
    term.retVal = value;
    term.retType = TypeRef::MakeInt64();
    return term;
}

void CheckFunctionImage(const RcuFile &object, const std::string_view name,
                        const std::vector<std::uint32_t> &expected) {
    const auto words = FunctionWords(object, name);
    INFO("function: ", name);
    CHECK_EQ(words.size(), expected.size());
    for (std::size_t i = 0; i < std::min(words.size(), expected.size()); ++i) {
        INFO("word index: ", i);
        CHECK_EQ(HexWord(words[i]), HexWord(expected[i]));
    }
}

std::vector<std::pair<std::uint32_t, std::uint16_t>>
FunctionRelocs(const RcuFile &object, const std::string_view function, const std::string_view symbol) {
    const RcuSymbol *owner = FindSymbol(object, function);
    REQUIRE_MESSAGE(owner != nullptr, function);
    std::vector<std::pair<std::uint32_t, std::uint16_t>> found;
    for (const auto &reloc : RelocsFor(object, RCU_TEXT_IDX, symbol)) {
        if (reloc.sectionOffset >= owner->value && reloc.sectionOffset < owner->value + owner->size) {
            found.emplace_back(reloc.sectionOffset - owner->value, reloc.type);
        }
    }
    return found;
}

std::string JoinMessages(const std::vector<Diagnostic> &diagnostics) {
    std::string joined;
    for (const auto &diagnostic : diagnostics) {
        if (!joined.empty()) {
            joined += " | ";
        }
        joined += diagnostic.message;
    }
    return joined;
}
} // namespace Rux::Testing
