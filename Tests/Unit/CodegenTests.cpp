#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/PhiMoveResolver.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Driver/BuildTarget.h"
#include "IntrinsicTestDeclarations.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/LirPrinter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <doctest.h>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Rux;

static constexpr std::string_view SliceDecl = "intrinsic struct Slice<T> { pub data: *T; pub length: uint; }\n";

static LirPackage CompileToLirFor(const std::string &source, const std::string &platform, const TargetContext &target) {
    Lexer lexer(std::string(SliceDecl) + source + std::string(Rux::Testing::StringDeclarations), "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", platform);
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto hirPackage = hirLowering.Generate();
    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Release);
    REQUIRE(pipeline.RunHir(hirPackage).reachedFixedPoint);

    HirToLirLowering lirLowering(std::move(hirPackage), target);
    return lirLowering.Generate();
}

static LirPackage CompileToLir(const std::string &source) {
    return CompileToLirFor(source, RUX_OS_WINDOWS ? "windows" : "linux", TargetContext::CreateNative());
}

static HirPackage CompileToHir(const std::string &source) {
    Lexer lexer(std::string(SliceDecl) + source + std::string(Rux::Testing::StringDeclarations), "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", RUX_OS_WINDOWS ? "windows" : "linux");
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering lowering(semaModel);
    return lowering.Generate();
}

// The printer defaults its target OS to the host, which is only correct when the
// two agree. Pass the same target the LIR was lowered for, so the rendered ABI
// describes the triple below rather than whatever machine the suite runs on.
static std::string CompileToAsm(const std::string &source) {
    const std::string_view triple = RUX_OS_WINDOWS ? "windows-x86_64" : "linux-x86_64";
    const auto target = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple));
    AssemblyPrinter printer(CompileToLirFor(source, RUX_OS_WINDOWS ? "windows" : "linux", target), target.os);
    return printer.Generate();
}

TEST_CASE("floating literal parsing preserves subnormal values") {
    CHECK_EQ(std::bit_cast<std::uint64_t>(ParseFloatLiteral<double>("5.0e-324")), 1);
    CHECK_EQ(std::bit_cast<std::uint32_t>(ParseFloatLiteral<float>("1.4e-45f32")), 1);
    CHECK_EQ(ParseFloatLiteral<double>("1_000.5"), 1000.5);
}

TEST_CASE("RCU backends preserve injected build metadata") {
    LirModule module;
    module.name = "Metadata.rux";
    LirPackage package;
    package.modules.push_back(std::move(module));
    const BuildInfo buildInfo("12.34.56-rc.1+test", 1'234'567'890);

    const auto x86Objects = RcuEmitter(package, "test", Target::OS::Linux, buildInfo).Generate();
    const auto aarch64Objects = AArch64RcuEmitter(package, "test", Target::OS::Linux, buildInfo).Generate();

    REQUIRE(x86Objects.size() == 1);
    REQUIRE(aarch64Objects.size() == 1);
    for (const RcuFile *object : {&x86Objects.front(), &aarch64Objects.front()}) {
        CHECK(object->ruxVersion == 0x0C'22'38);
        CHECK(object->buildTimestamp == 1'234'567'890);
    }
}

TEST_CASE("x86-64 RCU module emission preserves shared builder invariants") {
    const auto package = CompileToLir(R"(
        func Sink(text: string, value: float32) {}

        func Product(left: int, right: int) -> int {
            Sink("shared", 1.25f32);
            Sink("shared", 1.25f32);
            return left * right;
        }

        func Main() -> int {
            return Product(2, 3);
        }
    )");
    RcuEmitter emitter(package, "test", Target::OS::Linux);
    const auto objects = emitter.Generate();
    REQUIRE(emitter.Diagnostics().empty());
    REQUIRE(objects.size() == 1);
    const auto &object = objects.front();
    REQUIRE(object.sections.size() == 3);
    CHECK(object.sections[RCU_TEXT_IDX].name == ".text");
    CHECK(object.sections[RCU_RODATA_IDX].name == ".rodata");
    CHECK(object.sections[RCU_DATA_IDX].name == ".data");

    const auto countSymbols = [&](const std::string_view prefix) {
        return std::ranges::count_if(object.symbols,
                                     [prefix](const RcuSymbol &symbol) { return symbol.name.starts_with(prefix); });
    };
    CHECK(countSymbols("__str") == 1);
    CHECK(countSymbols("__f32_") == 1);
    const auto sink = std::ranges::find(object.symbols, "Sink", &RcuSymbol::name);
    REQUIRE(sink != object.symbols.end());
    CHECK(sink->sectionIdx == RCU_TEXT_IDX);

    std::unordered_set<std::string> names;
    for (const auto &symbol : object.symbols) {
        CHECK(names.insert(symbol.name).second);
    }
}

TEST_CASE("AArch64 RCU module emission matches shared x86-64 data invariants") {
    const auto package = CompileToLir(R"(
        func Sink(text: string, value: float32) {}

        func Product(left: int, right: int) -> int {
            Sink("shared", 1.3f32);
            Sink("shared", 1.3f32);
            return left * right;
        }

        func Main() -> int {
            return Product(2, 3);
        }
    )");
    RcuEmitter x86Emitter(package, "test", Target::OS::Linux);
    AArch64RcuEmitter aarch64Emitter(package, "test", Target::OS::Linux);
    const auto x86Objects = x86Emitter.Generate();
    const auto aarch64Objects = aarch64Emitter.Generate();
    REQUIRE(x86Emitter.Diagnostics().empty());
    REQUIRE(aarch64Emitter.Diagnostics().empty());
    REQUIRE(x86Objects.size() == 1);
    REQUIRE(aarch64Objects.size() == 1);
    const auto &x86 = x86Objects.front();
    const auto &aarch64 = aarch64Objects.front();

    REQUIRE(aarch64.sections.size() == 3);
    for (const std::size_t section : {RCU_TEXT_IDX, RCU_RODATA_IDX, RCU_DATA_IDX}) {
        CHECK(aarch64.sections[section].name == x86.sections[section].name);
        CHECK(aarch64.sections[section].type == x86.sections[section].type);
        CHECK(aarch64.sections[section].flags == x86.sections[section].flags);
        CHECK(aarch64.sections[section].alignment == x86.sections[section].alignment);
    }
    CHECK(aarch64.sections[RCU_RODATA_IDX].data == x86.sections[RCU_RODATA_IDX].data);
    CHECK(aarch64.sections[RCU_DATA_IDX].data == x86.sections[RCU_DATA_IDX].data);

    const auto countSymbols = [&](const std::string_view prefix) {
        return std::ranges::count_if(aarch64.symbols,
                                     [prefix](const RcuSymbol &symbol) { return symbol.name.starts_with(prefix); });
    };
    CHECK(countSymbols("__str") == 1);
    CHECK(countSymbols("__f32_") == 1);
    std::unordered_set<std::string> names;
    for (const auto &symbol : aarch64.symbols) {
        CHECK(names.insert(symbol.name).second);
    }
    CHECK(std::ranges::any_of(aarch64.sections[RCU_TEXT_IDX].relocs,
                              [](const RcuReloc &relocation) { return relocation.type == RcuRelType::AArch64Call26; }));
    CHECK(std::ranges::all_of(aarch64.sections[RCU_TEXT_IDX].relocs, [&](const RcuReloc &relocation) {
        return relocation.symbolIndex < aarch64.symbols.size() &&
               relocation.sectionOffset < aarch64.sections[RCU_TEXT_IDX].data.size();
    }));
}

TEST_CASE("RCU System V calls preserve register-allocated arguments") {
    const auto package = CompileToLir(R"(
        func Consume(value: uint) {}

        func Main() -> int {
            Consume(1024);
            return 0;
        }
    )");

    const auto objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_EQ(objects.size(), 1);

    const auto &object = objects.front();
    const auto main =
        std::ranges::find_if(object.symbols, [](const RcuSymbol &symbol) { return symbol.name == "Main"; });
    REQUIRE(main != object.symbols.end());
    REQUIRE(main->sectionIdx < object.sections.size());

    const auto &text = object.sections[main->sectionIdx].data;
    const std::vector<uint8_t> moveRegisterArgument = {
        0x48, 0x89, 0xD8, // mov rax, rbx
        0x48, 0x89, 0xC7, // mov rdi, rax
    };
    const auto begin = text.begin() + main->value;
    const auto end = begin + main->size;
    CHECK(std::search(begin, end, moveRegisterArgument.begin(), moveRegisterArgument.end()) != end);
}

TEST_CASE("RCU default calling convention follows the requested target") {
    const auto package = CompileToLir(R"(
        func Consume(value: uint) {}

        func Main() -> int {
            Consume(1024);
            return 0;
        }
    )");

    const auto FindMainText = [](const std::vector<RcuFile> &objects) {
        REQUIRE_EQ(objects.size(), 1);
        const auto &object = objects.front();
        const auto main =
            std::ranges::find_if(object.symbols, [](const RcuSymbol &symbol) { return symbol.name == "Main"; });
        REQUIRE(main != object.symbols.end());
        REQUIRE(main->sectionIdx < object.sections.size());
        const auto &text = object.sections[main->sectionIdx].data;
        return std::vector<std::uint8_t>(text.begin() + main->value, text.begin() + main->value + main->size);
    };

    const auto linuxText = FindMainText(RcuEmitter(package, "test", Target::OS::Linux).Generate());
    const auto windowsText = FindMainText(RcuEmitter(package, "test", Target::OS::Windows).Generate());
    const std::vector<std::uint8_t> linuxArgument = {
        0x48, 0x89, 0xD8, // mov rax, rbx
        0x48, 0x89, 0xC7, // mov rdi, rax
    };
    const std::vector<std::uint8_t> windowsArgument = {
        0x48, 0x89, 0xD8, // mov rax, rbx
        0x48, 0x89, 0xC1, // mov rcx, rax
    };
    CHECK(std::ranges::search(linuxText, linuxArgument).begin() != linuxText.end());
    CHECK(std::ranges::search(windowsText, windowsArgument).begin() != windowsText.end());

    const std::string linuxAssembly = AssemblyPrinter(package, Target::OS::Linux).Generate();
    const std::string windowsAssembly = AssemblyPrinter(package, Target::OS::Windows).Generate();
    CHECK(linuxAssembly.find("mov     rdi, rax\n    call    Consume") != std::string::npos);
    CHECK(windowsAssembly.find("mov     rcx, rax\n    sub     rsp, 32\n    call    Consume") != std::string::npos);
}

TEST_CASE("RCU System V calls pass two-word aggregates in two registers") {
    const auto package = CompileToLir(R"(
        struct Pair {
            first: uint;
            second: uint;
        }

        func Consume(value: Pair) -> uint {
            return value.second;
        }

        func Main() -> int {
            let value = Pair { first: 10, second: 20 };
            return Consume(value) == 20 ? 0 : 1;
        }
    )");

    const auto objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_EQ(objects.size(), 1);

    const auto &object = objects.front();
    const auto main =
        std::ranges::find_if(object.symbols, [](const RcuSymbol &symbol) { return symbol.name == "Main"; });
    REQUIRE(main != object.symbols.end());
    REQUIRE(main->sectionIdx < object.sections.size());

    const auto &text = object.sections[main->sectionIdx].data;
    const auto begin = text.begin() + main->value;
    const auto end = begin + main->size;
    const std::vector<uint8_t> moveFirstWord = {0x48, 0x89, 0xC7};  // mov rdi, rax
    const std::vector<uint8_t> moveSecondWord = {0x48, 0x89, 0xC6}; // mov rsi, rax
    CHECK(std::search(begin, end, moveFirstWord.begin(), moveFirstWord.end()) != end);
    CHECK(std::search(begin, end, moveSecondWord.begin(), moveSecondWord.end()) != end);
}

TEST_CASE("RCU System V calls return large aggregates through hidden storage") {
    const auto package = CompileToLir(R"(
        struct Triple {
            first: uint;
            second: uint;
            third: uint;
        }

        func MakeTriple() -> Triple {
            return Triple { first: 10, second: 20, third: 30 };
        }

        func Main() -> int {
            let value = MakeTriple();
            return value.third == 30 ? 0 : 1;
        }
    )");

    const auto objects = RcuEmitter(package, "test", Target::OS::Linux).Generate();
    REQUIRE_EQ(objects.size(), 1);

    const auto &object = objects.front();
    const auto main =
        std::ranges::find_if(object.symbols, [](const RcuSymbol &symbol) { return symbol.name == "Main"; });
    REQUIRE(main != object.symbols.end());
    REQUIRE(main->sectionIdx < object.sections.size());

    const auto &text = object.sections[main->sectionIdx].data;
    const auto begin = text.begin() + main->value;
    const auto end = begin + main->size;
    const std::vector<uint8_t> moveHiddenDestination = {0x48, 0x89, 0xC7}; // mov rdi, rax
    CHECK(std::search(begin, end, moveHiddenDestination.begin(), moveHiddenDestination.end()) != end);
}

TEST_CASE("phi parallel-copy resolver preserves cycles and duplicate sources") {
    const TypeRef intType = TypeRef::MakeInt64();
    const std::vector<PhiMove> moves = {
        {1, 2, intType},
        {2, 1, intType},
        {3, 1, intType},
    };
    const auto steps = ResolvePhiMoves(moves);

    std::unordered_map<LirReg, int64_t> values = {{1, 10}, {2, 20}, {3, 30}};
    int64_t temporary = 0;
    for (const auto &step : steps) {
        if (step.kind == PhiMoveStep::Kind::SaveDestination) {
            temporary = values.at(step.dst);
        }
        else {
            values[step.dst] = step.sourceIsTemporary ? temporary : values.at(step.src);
        }
    }

    CHECK(values.at(1) == 20);
    CHECK(values.at(2) == 10);
    CHECK(values.at(3) == 10);
}

TEST_CASE("assembly phi lowering breaks a swap cycle with a stack temporary") {
    const TypeRef intType = TypeRef::MakeInt64();

    LirBlock entry;
    entry.label = "entry";
    LirInstr first;
    first.dst = 1;
    first.type = intType;
    first.op = LirOpcode::Const;
    first.strArg = "1";
    LirInstr second = first;
    second.dst = 2;
    second.strArg = "2";
    entry.instrs = {std::move(first), std::move(second)};
    entry.term.emplace();
    entry.term->kind = LirTermKind::Jump;
    entry.term->trueTarget = 1;

    LirBlock loop;
    loop.label = "loop";
    LirInstr phiA;
    phiA.dst = 3;
    phiA.type = intType;
    phiA.op = LirOpcode::Phi;
    phiA.phiPreds = {{1, 0}, {4, 1}};
    LirInstr phiB = phiA;
    phiB.dst = 4;
    phiB.phiPreds = {{2, 0}, {3, 1}};
    loop.instrs = {std::move(phiA), std::move(phiB)};
    loop.term.emplace();
    loop.term->kind = LirTermKind::Jump;
    loop.term->trueTarget = 1;

    LirFunc function;
    function.name = "PhiSwap";
    function.returnType = intType;
    function.blocks = {std::move(entry), std::move(loop)};
    const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, Target::OS::Linux);
    LirModule module;
    module.name = "test";
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    const std::string output = AssemblyPrinter(std::move(package), Target::OS::Linux).Generate();
    const auto &slots = plan.SlotOffsets();
    const std::string expected = std::format("mov     rax, qword [rbp - {}]\n"
                                             "    mov     qword [rbp - {}], rax\n"
                                             "    mov     rax, qword [rbp - {}]\n"
                                             "    mov     qword [rbp - {}], rax\n"
                                             "    mov     rax, qword [rbp - {}]\n"
                                             "    mov     qword [rbp - {}], rax",
                                             slots.at(3), plan.PhiTemporaryOffset(), slots.at(4), slots.at(3),
                                             plan.PhiTemporaryOffset(), slots.at(4));
    CHECK(output.find(std::format("sub     rsp, {}", plan.FrameSize())) != std::string::npos);
    CHECK(output.find(expected) != std::string::npos);
}

TEST_CASE("assembly printer uses shared x86-64 frame slots and register homes") {
    LirFunc function;
    function.name = "FrameParity";
    function.callConv = CallingConvention::SysV;
    function.returnType = TypeRef::MakeInt64();
    function.params = {{0, TypeRef::MakeInt64(), "left"}, {1, TypeRef::MakeInt64(), "right"}};

    LirInstr add;
    add.op = LirOpcode::Add;
    add.dst = 2;
    add.type = TypeRef::MakeInt64();
    add.srcs = {0, 1};
    LirInstr alloca;
    alloca.op = LirOpcode::Alloca;
    alloca.dst = 3;
    alloca.type = TypeRef::MakeInt64();
    alloca.strArg = "3";

    LirBlock block;
    block.instrs = {add, alloca};
    block.term.emplace();
    block.term->kind = LirTermKind::Return;
    block.term->retVal = 2;
    block.term->retType = TypeRef::MakeInt64();
    function.blocks.push_back(std::move(block));

    const X86_64FramePlan plan = PlanX86_64Frame(function, {}, {}, Target::OS::Linux);
    LirModule module;
    module.name = "test";
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    const std::string output = AssemblyPrinter(std::move(package), Target::OS::Linux).Generate();
    const std::array<std::string_view, 5> registerNames = {"rbx", "r12", "r13", "r14", "r15"};
    for (const int physicalRegister : plan.UsedPhysicalRegisters()) {
        CHECK(output.find(std::format("push    {}", registerNames.at(physicalRegister))) != std::string::npos);
    }
    const std::int32_t localFrame =
        plan.FrameSize() - static_cast<std::int32_t>(plan.UsedPhysicalRegisters().size() * 8);
    CHECK(output.find(std::format("sub     rsp, {}", localFrame)) != std::string::npos);
    CHECK(output.find(std::format("qword [rbp - {}], rdi", plan.SlotOffsets().at(0))) != std::string::npos);
    CHECK(output.find(std::format("qword [rbp - {}], rsi", plan.SlotOffsets().at(1))) != std::string::npos);
    CHECK(output.find(std::format("lea     rax, [rbp - {}]", plan.AllocaDataOffsets().at(3))) != std::string::npos);
}

TEST_CASE("string literal slices reference static storage") {
    const std::string output = CompileToAsm(R"(
        func Name() -> string {
            return "Windows";
        }
    )");

    CHECK(output.find("section .rodata") != std::string::npos);
    CHECK(output.find("db    87, 105, 110, 100, 111, 119, 115, 0") != std::string::npos);
    CHECK(output.find("lea     rax, [rel __str") != std::string::npos);
}

TEST_CASE("array literals infer fixed inline arrays and coerce to Slice views") {
    const LirPackage package = CompileToLir(R"(
        func Sum(values: Slice<int>) -> int {
            return values[0] + values[3];
        }

        func Main() -> int {
            return Sum([0, 1, 2, 10]);
        }
    )");

    const auto &functions = package.modules.front().funcs;
    const auto main = std::ranges::find_if(functions, [](const LirFunc &function) { return function.name == "Main"; });
    REQUIRE(main != functions.end());

    bool hasInlineArray = false;
    bool hasSliceView = false;
    for (const LirBlock &block : main->blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op != LirOpcode::Alloca) {
                continue;
            }
            hasInlineArray |= instruction.type.kind == TypeRef::Kind::Array && instruction.type.arrayLength == 4 &&
                              instruction.type.inner == std::vector{TypeRef::MakeInt()};
            hasSliceView |= instruction.type == TypeRef::MakeSlice(TypeRef::MakeInt());
        }
    }

    CHECK(hasInlineArray);
    CHECK(hasSliceView);
}

TEST_CASE("array repeats remain compact in HIR and evaluate their operand once") {
    const HirPackage hir = CompileToHir(R"(
        func Next(counter: *var int) -> int {
            *counter = *counter + 1;
            return *counter;
        }

        func Main() -> int {
            var counter = 0;
            let values = [Next(@counter); 4];
            return values[3];
        }
    )");

    const auto function = std::ranges::find_if(hir.modules.front().funcs,
                                               [](const HirFunc &candidate) { return candidate.name == "Main"; });
    REQUIRE(function != hir.modules.front().funcs.end());
    REQUIRE(function->body.has_value());
    const auto *binding = dynamic_cast<const HirLetStmt *>(function->body->stmts[1].get());
    REQUIRE(binding != nullptr);
    const auto *repeat = dynamic_cast<const HirArrayExpr *>(binding->init.get());
    REQUIRE(repeat != nullptr);
    CHECK(repeat->elements.empty());
    REQUIRE(repeat->repeatedElement != nullptr);
    CHECK_EQ(repeat->repeatCount, 4);

    const LirPackage lir = CompileToLir(R"(
        func Next(counter: *var int) -> int {
            *counter = *counter + 1;
            return *counter;
        }

        func Main() -> int {
            var counter = 0;
            let values = [Next(@counter); 4];
            return values[3];
        }
    )");

    const auto main = std::ranges::find_if(lir.modules.front().funcs,
                                           [](const LirFunc &candidate) { return candidate.name == "Main"; });
    REQUIRE(main != lir.modules.front().funcs.end());
    std::size_t calls = 0;
    for (const LirBlock &block : main->blocks) {
        calls += std::ranges::count_if(block.instrs, [](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Call && instruction.strArg == "Next";
        });
    }
    CHECK_EQ(calls, 1);
}

TEST_CASE("address-of an indexed array element uses the original storage") {
    const LirPackage package = CompileToLir(R"(
        func AddressOfFirst() -> uint {
            let values: uint[4] = [255u, 127u, 10u, 0u];
            return @values[0] as uint;
        }
    )");

    const auto &functions = package.modules.front().funcs;
    const auto function =
        std::ranges::find_if(functions, [](const LirFunc &candidate) { return candidate.name == "AddressOfFirst"; });
    REQUIRE(function != functions.end());

    const LirInstr *addressCast = nullptr;
    for (const LirBlock &block : function->blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Cast && instruction.type == TypeRef::MakeUInt() &&
                instruction.strArg == "*uint") {
                addressCast = &instruction;
            }
        }
    }
    REQUIRE(addressCast != nullptr);
    REQUIRE_EQ(addressCast->srcs.size(), 1);

    const LirInstr *pointerDefinition = nullptr;
    for (const LirBlock &block : function->blocks) {
        const auto instruction = std::ranges::find_if(
            block.instrs, [&](const LirInstr &candidate) { return candidate.dst == addressCast->srcs.front(); });
        if (instruction != block.instrs.end()) {
            pointerDefinition = &*instruction;
            break;
        }
    }
    REQUIRE(pointerDefinition != nullptr);
    CHECK_EQ(pointerDefinition->op, LirOpcode::IndexPtr);
}

TEST_CASE("metadata blocks are rejected before extern functions") {
    Lexer lexer(R"(
        #{ library: "Kernel32.dll" }
        extern func CreateFileA() -> *uint8;
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();

    REQUIRE(parsed.HasErrors());
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics.front().message,
             "metadata blocks '#{...}' are unsupported; use attribute calls such as '#Abi(.Win64)'");
}

TEST_CASE("metadata blocks are rejected after compatibility attributes") {
    Lexer lexer(R"(
        #Library("Kernel32.dll")
        #{ symbol: "Beep" }
        extern func Tone(freq: uint32, duration: uint32) -> bool32;
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();

    REQUIRE(parsed.HasErrors());
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics.front().message,
             "metadata blocks '#{...}' are unsupported; use attribute calls such as '#Abi(.Win64)'");
}

TEST_CASE("extern C calls follow the target's ABI rather than the host's") {
    const std::string source = R"(
        #Link("hostlib")
        extern func Emit(text: *uint8) -> int32;

        func Main() -> int {
            Emit(null);
            return 0;
        }
    )";

    const auto conventionOfEmitCall = [](const LirPackage &package) {
        for (const auto &mod : package.modules) {
            for (const auto &func : mod.funcs) {
                for (const auto &block : func.blocks) {
                    for (const auto &instr : block.instrs) {
                        if (instr.op == LirOpcode::Call && instr.strArg == "Emit") {
                            return instr.callConv;
                        }
                    }
                }
            }
        }
        return CallingConvention::Default;
    };

    // An extern declaration without an explicit #Abi resolves to the C ABI of
    // the target being built for. Both cases have to hold on every host, which
    // is the whole point: before the convention became target-driven, each of
    // these recorded whatever the compiler was running on.
    const auto windows = CompileToLirFor(
        source, "windows", Driver::TargetContextForTriple(*Target::TargetTriple::Parse("windows-x86_64")));
    CHECK_EQ(conventionOfEmitCall(windows), CallingConvention::Win64);

    const auto linuxTarget =
        CompileToLirFor(source, "linux", Driver::TargetContextForTriple(*Target::TargetTriple::Parse("linux-x86_64")));
    CHECK_EQ(conventionOfEmitCall(linuxTarget), CallingConvention::SysV);

    const auto windowsAArch64 = CompileToLirFor(
        source, "windows", Driver::TargetContextForTriple(*Target::TargetTriple::Parse("windows-aarch64")));
    CHECK_EQ(conventionOfEmitCall(windowsAArch64), CallingConvention::AAPCS64);
}

TEST_CASE("platform conventions are decided by the target OS and architecture") {
    CHECK_EQ(PlatformCConvention(Target::OS::Windows, Target::Arch::X86_64), CallingConvention::Win64);
    CHECK_EQ(PlatformCConvention(Target::OS::Linux, Target::Arch::X86_64), CallingConvention::SysV);
    CHECK_EQ(PlatformCConvention(Target::OS::MacOS, Target::Arch::X86_64), CallingConvention::SysV);
    CHECK_EQ(PlatformCConvention(Target::OS::Windows, Target::Arch::AArch64), CallingConvention::AAPCS64);
    CHECK_EQ(PlatformCConvention(Target::OS::Linux, Target::Arch::AArch64), CallingConvention::AAPCS64);

    CHECK_EQ(PlatformDefaultConvention(Target::OS::Linux, Target::Arch::X86_64), CallingConvention::SysV);
    CHECK_EQ(PlatformDefaultConvention(Target::OS::Windows, Target::Arch::X86_64), CallingConvention::Win64);
    CHECK_EQ(PlatformDefaultConvention(Target::OS::Windows, Target::Arch::AArch64), CallingConvention::AAPCS64);

    // Win64 belongs to Windows alone. macOS and FreeBSD are System V on x86-64,
    // for the internal ABI exactly as for the C ABI.
    CHECK_EQ(PlatformDefaultConvention(Target::OS::MacOS, Target::Arch::X86_64), CallingConvention::SysV);
    CHECK_EQ(PlatformDefaultConvention(Target::OS::FreeBSD, Target::Arch::X86_64), CallingConvention::SysV);
    CHECK_EQ(PlatformCConvention(Target::OS::FreeBSD, Target::Arch::X86_64), CallingConvention::SysV);

    // `.C` collapses against the target; concrete conventions pass through.
    CHECK_EQ(ResolveCConvention(CallingConvention::C, Target::OS::Windows, Target::Arch::X86_64),
             CallingConvention::Win64);
    CHECK_EQ(ResolveCConvention(CallingConvention::C, Target::OS::Linux, Target::Arch::X86_64),
             CallingConvention::SysV);
    CHECK_EQ(ResolveCConvention(CallingConvention::C, Target::OS::Windows, Target::Arch::AArch64),
             CallingConvention::AAPCS64);
    CHECK_EQ(ResolveCConvention(CallingConvention::SysV, Target::OS::Windows, Target::Arch::AArch64),
             CallingConvention::SysV);
    CHECK_EQ(ResolveCConvention(CallingConvention::Default, Target::OS::Linux, Target::Arch::AArch64),
             CallingConvention::Default);

    // The argument-less forms stay host-defaulted.
    CHECK_EQ(PlatformCConvention(), PlatformCConvention(Target::HostOS, Target::HostArch));
    CHECK_EQ(PlatformDefaultConvention(), PlatformDefaultConvention(Target::HostOS, Target::HostArch));
}

TEST_CASE("C variadic call metadata survives package-wide extern lookup and Link renaming") {
    HirPackage hir;

    HirModule interop;
    interop.name = "Interop";
    HirExternFunc external;
    external.name = "Format";
    external.dll = "runtime";
    external.symbolName = "native_format";
    external.callConv = CallingConvention::C;
    external.isVariadic = true;
    external.params.push_back({"format", TypeRef::MakePointer(TypeRef::MakeChar8()), false});
    external.returnType = TypeRef::MakeInt32();
    interop.externFuncs.push_back(std::move(external));
    hir.modules.push_back(std::move(interop));

    HirModule application;
    application.name = "Application";
    HirFunc main;
    main.name = "Main";
    main.returnType = TypeRef::MakeOpaque();
    HirBlock body;
    auto statement = std::make_unique<HirExprStmt>();
    auto call = std::make_unique<HirCallExpr>();
    auto callee = std::make_unique<HirPathExpr>();
    callee->segments = {"Interop", "Format"};
    call->callee = std::move(callee);
    call->type = TypeRef::MakeInt32();
    auto format = std::make_unique<HirLiteralExpr>();
    format->value = "null";
    format->type = TypeRef::MakePointer(TypeRef::MakeChar8());
    call->args.push_back(std::move(format));
    auto argument = std::make_unique<HirLiteralExpr>();
    argument->value = "42";
    argument->type = TypeRef::MakeInt32();
    call->args.push_back(std::move(argument));
    statement->expr = std::move(call);
    body.stmts.push_back(std::move(statement));
    main.body = std::move(body);
    application.funcs.push_back(std::move(main));
    hir.modules.push_back(std::move(application));

    const auto package =
        HirToLirLowering(std::move(hir),
                         Driver::TargetContextForTriple(*Target::TargetTriple::Parse("windows-aarch64")))
            .Generate();
    REQUIRE_EQ(package.modules.size(), 2);
    REQUIRE_EQ(package.modules[0].funcs.size(), 1);
    CHECK_EQ(package.modules[0].funcs[0].name, "native_format");
    CHECK_EQ(package.modules[0].funcs[0].callConv, CallingConvention::AAPCS64);
    CHECK(package.modules[0].funcs[0].isVariadic);

    const auto &instructions = package.modules[1].funcs[0].blocks[0].instrs;
    const auto found = std::ranges::find_if(
        instructions, [](const LirInstr &instruction) { return instruction.op == LirOpcode::Call; });
    REQUIRE(found != instructions.end());
    CHECK_EQ(found->strArg, "native_format");
    CHECK_EQ(found->callConv, CallingConvention::AAPCS64);
    CHECK(found->isCVariadic);
    CHECK_EQ(found->cVariadicFixedParamCount, std::optional<std::uint32_t>(1));
}

TEST_CASE("Rux variadics remain slice calls rather than C variadic calls") {
    const auto package =
        CompileToLirFor(R"(
        func Sum(values: int...) -> int {
            return 0;
        }

        func Main() -> int {
            return Sum(1, 2);
        }
    )",
                        "windows", Driver::TargetContextForTriple(*Target::TargetTriple::Parse("windows-aarch64")));

    for (const auto &function : package.modules.front().funcs) {
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Call && instruction.strArg == "Sum") {
                    CHECK_FALSE(instruction.isCVariadic);
                    CHECK_FALSE(instruction.cVariadicFixedParamCount.has_value());
                    CHECK_EQ(instruction.callConv, CallingConvention::Default);
                    return;
                }
            }
        }
    }
    FAIL("expected a direct call to Sum");
}

TEST_CASE("LIR dumps resolved convention and C variadic call metadata") {
    LirPackage package;
    LirModule module;
    module.name = "Test";

    LirFunc external;
    external.name = "native_format";
    external.isExtern = true;
    external.isVariadic = true;
    external.callConv = CallingConvention::AAPCS64;
    external.returnType = TypeRef::MakeOpaque();
    module.funcs.push_back(std::move(external));

    LirFunc caller;
    caller.name = "Main";
    caller.returnType = TypeRef::MakeOpaque();
    LirBlock entry;
    entry.label = "entry";
    LirInstr call;
    call.op = LirOpcode::Call;
    call.type = TypeRef::MakeOpaque();
    call.strArg = "native_format";
    call.callConv = CallingConvention::AAPCS64;
    call.isCVariadic = true;
    call.cVariadicFixedParamCount = 0;
    entry.instrs.push_back(std::move(call));
    caller.blocks.push_back(std::move(entry));
    module.funcs.push_back(std::move(caller));
    package.modules.push_back(std::move(module));

    const auto path = std::filesystem::temp_directory_path() / "rux-call-metadata-lir.txt";
    REQUIRE(LirPrinter::Dump(package, path));
    std::ifstream input(path);
    REQUIRE(input.good());
    const std::string dump((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    std::error_code error;
    std::filesystem::remove(path, error);

    CHECK(dump.find("extern func native_format(...) cc=aapcs64") != std::string::npos);
    CHECK(dump.find("call opaque @native_format() cc=aapcs64 c_variadic fixed=0") != std::string::npos);
}

TEST_CASE("Abi attribute replaces ABI metadata blocks") {
    Lexer lexer(R"(
        #Abi(.SysV)
        func Native() {}
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    REQUIRE_EQ(parsed.module.items.size(), 1);
    const auto *function = dynamic_cast<const FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    CHECK_EQ(function->callConv, CallingConvention::SysV);
}

TEST_CASE("Abi attribute validates its target, value, and uniqueness") {
    Lexer lexer(R"(
        #Abi(.Unknown)
        const Value = 1;

        #Abi(.C)
        #Abi(.Win64)
        func Duplicate() {}
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();
    REQUIRE(parsed.HasErrors());
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("unknown ABI '.Unknown'") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("'#Abi' can only be applied to a function or extern block") != std::string::npos;
    }));
    CHECK(std::ranges::any_of(parsed.diagnostics, [](const Diagnostic &diagnostic) {
        return diagnostic.message.find("duplicate '#Abi' attribute") != std::string::npos;
    }));
}

TEST_CASE("Link cannot be combined with compatibility import attributes") {
    Lexer lexer(R"(
        #Link("Kernel32.dll")
        #Library("Kernel32.dll")
        extern func Beep(freq: uint32, duration: uint32) -> bool32;
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();

    REQUIRE(parsed.HasErrors());
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics.front().message, "'#Library' cannot be combined with '#Link'");
}

TEST_CASE("two-argument Link cannot be applied to an extern block") {
    Lexer lexer(R"(
        #Link("Kernel32.dll", "Beep")
        extern { func Beep(freq: uint32, duration: uint32) -> bool32; }
    )",
                "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    const auto parsed = parser.Parse();

    REQUIRE(parsed.HasErrors());
    REQUIRE_EQ(parsed.diagnostics.size(), 1);
    CHECK_EQ(parsed.diagnostics.front().message, "an imported symbol name cannot be applied to an extern block; "
                                                 "use the one-argument '#Link(\"library\")' form");
}

TEST_CASE("codegen generates correct calling convention for extern functions") {
    std::string source = R"(
        #Link("Kernel32.dll", "CreateFileA")
        extern func OpenFile(
            lpFileName: *uint8,
            dwDesiredAccess: uint32,
            dwShareMode: uint32,
            lpSecurityAttributes: *uint8,
            dwCreationDisposition: uint32,
            dwFlagsAndAttributes: uint32,
            hTemplateFile: *uint8
        ) -> *uint8;

        func Main() -> int {
            let handle = OpenFile(
                null,
                1073741824u32,
                0u32,
                null,
                2u32,
                0u32,
                null
            );
            return 0;
        }
    )";

    std::string asmOutput = CompileToAsm(source);
    CHECK(asmOutput.find("CreateFileA") != std::string::npos);

#if RUX_OS_WINDOWS
    // Sur Windows, on s'attend à la convention d'appel Win64.
    // L'en-tête de l'assembleur généré doit mentionner l'ABI Windows x64.
    CHECK(asmOutput.find("Target:  x86-64  (Windows x64 ABI") != std::string::npos);

    // Les 4 premiers arguments vont dans rcx, rdx, r8, r9.
    CHECK(asmOutput.find("mov     rcx,") != std::string::npos);
    CHECK(asmOutput.find("mov     rdx,") != std::string::npos);
    CHECK(asmOutput.find("mov     r8,") != std::string::npos);
    CHECK(asmOutput.find("mov     r9,") != std::string::npos);

    // La pile est diminuée de 64 octets (32 shadow + 32 arguments pile alignés).
    CHECK(asmOutput.find("sub     rsp, 64") != std::string::npos);
#else
    // Sur Linux (ou autre plateforme non-Windows), on s'attend à la convention System V AMD64.
    CHECK(asmOutput.find("Target:  x86-64  (System V AMD64 ABI") != std::string::npos);

    // Les 6 premiers arguments vont dans rdi, rsi, rdx, rcx, r8, r9.
    CHECK(asmOutput.find("mov     rdi,") != std::string::npos);
    CHECK(asmOutput.find("mov     rsi,") != std::string::npos);
    CHECK(asmOutput.find("mov     rdx,") != std::string::npos);
    CHECK(asmOutput.find("mov     rcx,") != std::string::npos);
    CHECK(asmOutput.find("mov     r8,") != std::string::npos);
    CHECK(asmOutput.find("mov     r9,") != std::string::npos);

    // Un seul argument sur la pile (le 7ème), aligné à 16 octets.
    CHECK(asmOutput.find("sub     rsp, 16") != std::string::npos);
#endif
}
