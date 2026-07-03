// Golden diagnostics runner.
//
// Every Tests/Golden/<Case>.rux file is compiled through the frontend
// (lex -> parse -> sema, mirroring `rux check`), its diagnostics are rendered
// one per line as "line:column: severity: message", and the result is compared
// against the sibling <Case>.expected file.
//
// To (re)generate the expected files after an intentional diagnostics change,
// run the test binary with RUX_UPDATE_GOLDEN=1 and review the diff.

#include "Driver/BuildTarget.h"

#include <algorithm>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Diagnostics/Diagnostics.h"
#include "Lexer/Lexer.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

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

// Run the frontend over one in-memory source file and render its diagnostics.
// Later stages only run when the earlier ones are clean, mirroring the driver.
std::string FrontendDiagnostics(std::string source, const std::string &sourceName) {
    std::string out;

    Lexer lexer(std::move(source), sourceName);
    auto lexResult = lexer.Tokenize();
    AppendDiagnostics(out, lexResult.diagnostics);
    if (lexResult.HasErrors()) {
        return out;
    }

    Parser parser(std::move(lexResult.tokens), sourceName);
    auto parseResult = parser.Parse();
    AppendDiagnostics(out, parseResult.diagnostics);
    if (parseResult.HasErrors()) {
        return out;
    }

    SemanticAnalyzer analyzer({&parseResult.module}, {}, "Golden",
                              std::string(Driver::TargetOsName(Driver::HostTargetTriple())));
    const auto semaResult = analyzer.Analyze();
    AppendDiagnostics(out, semaResult.diagnostics);
    return out;
}

} // namespace

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
