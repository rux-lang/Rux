// Golden diagnostics runner.
//
// Every Tests/Unit/Golden/<Case>.rux file is compiled through the frontend
// (lex -> parse -> sema, mirroring `rux check`), its diagnostics are rendered
// one per line as "line:column: severity: message", and the result is compared
// against the sibling <Case>.expected file. Supplemental notes, help and docs
// are deliberately excluded: these files pin only the stable primary line.
//
// A case compiled for AArch64 goes one stage further: every `asm func` a clean
// frontend leaves behind is handed to the AArch64 assembler, so a case can pin
// what an inline body reports. That assembler is the one with no route through
// the driver yet — the x86-64 one already reports through `rux build` — so a
// golden case is the only place its diagnostics can be read as a body's author
// would see them.
//
// To (re)generate the expected files after an intentional diagnostics change,
// run the test binary with RUX_UPDATE_GOLDEN=1 and review the diff.

#include "CodeGen/AArch64/Assembler.h"
#include "Diagnostics/Diagnostics.h"
#include "Driver/BuildTarget.h"
#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {

std::optional<std::string> ReadFileText(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Normalize line endings so goldens compare equal regardless of how git
    // checked the files out.
    std::erase(text, '\r');
    return text;
}

void AppendDiagnostics(std::string &out, std::span<const Diagnostic> diags) {
    for (const auto &diag : diags) {
        if (diag.sourceName.empty()) {
            out += std::format("{}: {}\n", SeverityName(diag.severity), diag.message);
        }
        else {
            out += std::format("{}:{}: {}: {}\n", diag.location.line, diag.location.column, SeverityName(diag.severity),
                               diag.message);
        }
    }
}

// The target a case is compiled for. A case that says nothing is compiled for
// the host, which is what every case about the language rather than the
// machine wants; one that opens with `// rux:target <triple>` names its own,
// so a diagnostic about a foreign target can be asserted from any host.
std::string CaseTarget(const std::string &source) {
    static constexpr std::string_view directive = "// rux:target ";
    if (source.starts_with(directive)) {
        const auto end = source.find('\n');
        std::string triple = source.substr(directive.size(), end - directive.size());
        while (!triple.empty() && (triple.back() == ' ' || triple.back() == '\t')) {
            triple.pop_back();
        }
        return triple;
    }
    return Driver::HostTargetTriple();
}

// Run the frontend over one in-memory source file and render its diagnostics.
// Later stages only run when the earlier ones are clean, mirroring the driver.
std::string FrontendDiagnostics(std::string source, const std::string &sourceName) {
    std::string out;

    CompileTimeContext context;
    context.targetTriple = CaseTarget(source);
    context.target = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(context.targetTriple));

    Lexer lexer(std::move(source), sourceName);
    auto lexResult = lexer.Tokenize();
    AppendDiagnostics(out, lexResult.diagnostics);
    if (lexResult.HasErrors()) {
        return out;
    }

    Parser parser(std::move(lexResult.tokens), sourceName, context.target.arch);
    auto parseResult = parser.Parse();
    AppendDiagnostics(out, parseResult.diagnostics);
    if (parseResult.HasErrors()) {
        return out;
    }

    SemanticAnalyzer analyzer({&parseResult.module}, {}, "Golden", context);
    const auto semaResult = analyzer.Analyze();
    AppendDiagnostics(out, semaResult.diagnostics);
    if (semaResult.HasErrors() || context.target.arch != Target::Arch::AArch64) {
        return out;
    }

    // Assemble what the frontend accepted. An `asm func` body is machine code
    // the moment it parses, so its mistakes are the assembler's to report.
    for (const auto &item : parseResult.module.items) {
        const auto *func = dynamic_cast<const FuncDecl *>(item.get());
        if (func == nullptr || !func->isAsm) {
            continue;
        }
        std::vector<std::uint8_t> code;
        const AsmAssembly assembled = AssembleAArch64AsmFunc(func->asmBody, sourceName, code);
        AppendDiagnostics(out, assembled.diagnostics);
    }
    return out;
}

} // namespace

TEST_CASE("Golden diagnostic serialization omits supplemental human context") {
    Diagnostic diagnostic{
        Diagnostic::Severity::Error, "Case.rux", {.line = 4, .column = 2}, "primary message", {}, {}, {}};
    diagnostic.notes = {"supporting note"};
    diagnostic.help = "corrective help";
    diagnostic.documentationUrl = "https://example.invalid/docs";

    std::string output;
    AppendDiagnostics(output, std::array{diagnostic});
    CHECK(output == "4:2: error: primary message\n");
}

TEST_CASE("Golden diagnostics match the expected files") {
    const std::filesystem::path goldenDir = RUX_GOLDEN_DIR;
    REQUIRE_MESSAGE(std::filesystem::is_directory(goldenDir), "golden case directory not found: ", goldenDir.string());

    std::vector<std::filesystem::path> cases;
    for (const auto &entry : std::filesystem::directory_iterator(goldenDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".rux") {
            cases.push_back(entry.path());
        }
    }
    std::sort(cases.begin(), cases.end());
    REQUIRE_MESSAGE(!cases.empty(), "no .rux cases in ", goldenDir.string());

    const bool update = System::HasEnv("RUX_UPDATE_GOLDEN");

    for (const auto &casePath : cases) {
        const std::string caseName = casePath.stem().string();
        CAPTURE(caseName);

        auto source = ReadFileText(casePath);
        REQUIRE_MESSAGE(source.has_value(), "cannot read ", casePath.string());

        const std::string actual = FrontendDiagnostics(std::move(*source), caseName + ".rux");
        const auto expectedPath = std::filesystem::path(casePath).replace_extension(".expected");

        if (update) {
            std::ofstream out(expectedPath, std::ios::binary);
            REQUIRE_MESSAGE(out.good(), "cannot write ", expectedPath.string());
            out << actual;
            continue;
        }

        const auto expected = ReadFileText(expectedPath);
        REQUIRE_MESSAGE(expected.has_value(), "missing expected file ", expectedPath.string(),
                        " (run with RUX_UPDATE_GOLDEN=1 to generate)");
        CHECK_EQ(*expected, actual);
    }
}
