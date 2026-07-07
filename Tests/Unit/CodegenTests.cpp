#include <doctest.h>
#include <string>
#include <vector>

#include "CodeGen/X86_64/AssemblyPrinter.h"
#include "Ir/Hir/Hir.h"
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

    std::vector<const Module *> modules = {&parsed.module};
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

TEST_CASE("codegen generates correct calling convention for extern functions") {
    std::string source = R"(
        @[Import(lib: "kernel32.dll")]
        extern func CreateFileA(
            lpFileName: *uint8,
            dwDesiredAccess: uint32,
            dwShareMode: uint32,
            lpSecurityAttributes: *uint8,
            dwCreationDisposition: uint32,
            dwFlagsAndAttributes: uint32,
            hTemplateFile: *uint8
        ) -> *uint8;

        func Main() -> int {
            let handle = CreateFileA(
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
