#include "Ir/Hir/Hir.h"

#include <algorithm>
#include <doctest.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "CodeGen/PhiMoveResolver.h"
#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Platform.h"

using namespace Rux;

static std::string CompileToAsm(const std::string &source) {
    Lexer lexer(source, "test.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    Parser parser(std::move(lexed.tokens), "test.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    std::vector<Module *> modules = {&parsed.module};
    SemanticAnalyzer analyzer(modules, {}, "test", RUX_OS_WINDOWS ? "windows" : "linux");
    auto semaModel = analyzer.Analyze();
    REQUIRE_FALSE(semaModel.HasErrors());

    AstToHirLowering hirLowering(semaModel);
    auto hirPackage = hirLowering.Generate();

    HirToLirLowering lirLowering(std::move(hirPackage));
    auto lirPackage = lirLowering.Generate();

    AssemblyPrinter printer(std::move(lirPackage));
    return printer.Generate();
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
    LirModule module;
    module.name = "test";
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    const std::string output = AssemblyPrinter(std::move(package)).Generate();
    const std::string expected = "mov     rax, qword [rbp - 24]\n"
                                 "    mov     qword [rbp - 40], rax\n"
                                 "    mov     rax, qword [rbp - 32]\n"
                                 "    mov     qword [rbp - 24], rax\n"
                                 "    mov     rax, qword [rbp - 40]\n"
                                 "    mov     qword [rbp - 32], rax";
    CHECK(output.find(expected) != std::string::npos);
}

TEST_CASE("string literal slices reference static storage") {
    const std::string output = CompileToAsm(R"(
        func Name() -> char8[] {
            return "Windows";
        }
    )");

    CHECK(output.find("section .rodata") != std::string::npos);
    CHECK(output.find("db    87, 105, 110, 100, 111, 119, 115, 0") != std::string::npos);
    CHECK(output.find("lea     rax, [rel __str") != std::string::npos);
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
