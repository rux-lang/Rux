#include "Driver/CompilerDriver.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Optimization/Pipeline.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "System/Os.h"

#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;
using namespace Rux::Driver;
using namespace Rux::System;

namespace {
class DriverDiagnosticFixture {
public:
    DriverDiagnosticFixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = TempDirectory() / ("rux-driver-diagnostic-test-" + std::to_string(nonce));
        std::filesystem::create_directories(root / "Src");
        manifest.package.name = *IdentitySegment::Parse("DiagnosticFixture");
        manifest.package.version = *SemanticVersion::Parse("0.1.0");
        manifest.package.type = ManifestPackageType::Executable;
        REQUIRE(manifest.Save(root / "Rux.toml"));
        WriteSource("func Main() -> int { return 0; }\n");
    }

    ~DriverDiagnosticFixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    DriverDiagnosticFixture(const DriverDiagnosticFixture &) = delete;
    DriverDiagnosticFixture &operator=(const DriverDiagnosticFixture &) = delete;

    void WriteSource(const std::string &source) const {
        std::ofstream output(root / "Src" / "Main.rux", std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << source;
        REQUIRE(output.good());
    }

    void BlockArtifactOutput() {
        manifest.build.output = "BlockedOutput";
        std::ofstream output(root / manifest.build.output, std::ios::binary | std::ios::trunc);
        REQUIRE(output.good());
        output << "not a directory";
        REQUIRE(output.good());
    }

    [[nodiscard]] CompileOptions Options(const bool checkOnly) const {
        CompileOptions options;
        options.manifestPath = root / "Rux.toml";
        options.manifest = manifest;
        options.target = Target::TargetTriple::Host();
        options.checkOnly = checkOnly;
        return options;
    }

private:
    std::filesystem::path root;
    Manifest manifest;
};

class ScopedEnvironmentValue {
public:
    explicit ScopedEnvironmentValue(std::string inputName)
        : name(std::move(inputName))
        , saved(GetEnv(name.c_str())) {
    }

    ~ScopedEnvironmentValue() {
        if (saved) {
            static_cast<void>(SetEnv(name.c_str(), *saved));
        }
        else {
            static_cast<void>(UnsetEnv(name.c_str()));
        }
    }

private:
    std::string name;
    std::optional<std::string> saved;
};

struct UnsupportedExpr final : Expr {};

class AlwaysChangingPass final : public Optimization::HirPass {
public:
    [[nodiscard]] std::string_view Name() const noexcept override {
        return "always-changing";
    }

    Optimization::PassChange Run(HirPackage &, const Optimization::PassContext &) override {
        return Optimization::PassChange::Changed;
    }
};
} // namespace

TEST_CASE("compiler driver accumulates diagnostics without a presentation callback") {
    DriverDiagnosticFixture fixture;
    fixture.WriteSource("func Main() -> int { return Missing; }\n");

    const auto result = CompilerDriver(fixture.Options(true)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].message.contains("Missing"));
    CHECK(std::ranges::find(result.diagnostics[0].notes, "compiler phase: Analyzing") !=
          result.diagnostics[0].notes.end());
}

TEST_CASE("compiler driver rejects an invalid reproducible build timestamp during configuration") {
    ScopedEnvironmentValue environment("SOURCE_DATE_EPOCH");
    REQUIRE(SetEnv("SOURCE_DATE_EPOCH", "tomorrow"));
    DriverDiagnosticFixture fixture;
    std::vector<Diagnostic> presented;
    auto options = fixture.Options(false);
    options.emitDiagnostic = [&](const Diagnostic &diagnostic, const SourceLineLookup &) {
        presented.push_back(diagnostic);
    };

    const auto result = CompilerDriver(std::move(options)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(result.diagnostics.size() == 1);
    REQUIRE(presented.size() == result.diagnostics.size());
    CHECK(presented[0].message == result.diagnostics[0].message);
    CHECK(result.diagnostics[0].message.contains("invalid SOURCE_DATE_EPOCH value 'tomorrow'"));
    CHECK(result.diagnostics[0].notes.back() == "compiler phase: Configuring");
    REQUIRE(result.diagnostics[0].help.has_value());
}

TEST_CASE("compiler driver reports artifact output preparation failures in the linking phase") {
    DriverDiagnosticFixture fixture;
    fixture.BlockArtifactOutput();

    const auto result = CompilerDriver(fixture.Options(false)).Compile();

    CHECK_FALSE(result.ok);
    REQUIRE(result.diagnostics.size() == 1);
    CHECK(result.diagnostics[0].message.contains("cannot prepare artifact output directory"));
    CHECK(result.diagnostics[0].notes.back() == "compiler phase: Linking");
    CHECK(std::ranges::any_of(result.diagnostics[0].notes,
                              [](const std::string &note) { return note.contains("system error"); }));
}

TEST_CASE("AST-to-HIR lowering diagnoses an unsupported expression with source context") {
    constexpr std::string_view source = "func Main() -> int { return 1; }";
    Lexer lexer(std::string(source), "unsupported.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "unsupported.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());

    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "windows");
    auto model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());

    auto *function = dynamic_cast<FuncDecl *>(parsed.module.items.front().get());
    REQUIRE(function != nullptr);
    auto *returnStatement = dynamic_cast<ReturnStmt *>(function->body->stmts.front().get());
    REQUIRE(returnStatement != nullptr);
    REQUIRE(returnStatement->value.has_value());
    auto unsupported = std::make_unique<UnsupportedExpr>();
    unsupported->location = (**returnStatement->value).location;
    *returnStatement->value = std::move(unsupported);

    AstToHirLowering lowering(model);
    static_cast<void>(lowering.Generate());

    REQUIRE(lowering.Diagnostics().size() == 1);
    const auto &diagnostic = lowering.Diagnostics().front();
    CHECK(diagnostic.sourceName == "unsupported.rux");
    CHECK(diagnostic.location.line == 1);
    CHECK(diagnostic.message == "cannot lower 'unknown' expression to HIR");
    REQUIRE(diagnostic.notes.size() == 1);
    CHECK(diagnostic.notes.front().contains("internal compiler limitation"));
    REQUIRE(diagnostic.help.has_value());
}

TEST_CASE("HIR-to-LIR lowering accumulates invalid internal IR diagnostics") {
    auto left = std::make_unique<HirLiteralExpr>();
    left->type = TypeRef::MakeInt32();
    left->value = "1";
    auto right = std::make_unique<HirLiteralExpr>();
    right->type = TypeRef::MakeInt32();
    right->value = "2";
    auto invalid = std::make_unique<HirBinaryExpr>();
    invalid->type = TypeRef::MakeInt32();
    invalid->op = TokenKind::Assign;
    invalid->left = std::move(left);
    invalid->right = std::move(right);
    auto returned = std::make_unique<HirReturnStmt>();
    returned->value = std::move(invalid);

    HirFunc function;
    function.name = "Broken";
    function.returnType = TypeRef::MakeInt32();
    function.body.emplace();
    function.body->stmts.push_back(std::move(returned));
    HirModule module;
    module.funcs.push_back(std::move(function));
    HirPackage package;
    package.modules.push_back(std::move(module));

    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    static_cast<void>(lowering.Generate());

    REQUIRE(lowering.Diagnostics().size() == 1);
    CHECK(lowering.Diagnostics().front().message.contains("invalid internal LIR"));
    CHECK(lowering.Diagnostics().front().message.contains("source operator has no LIR opcode mapping"));
    REQUIRE(lowering.Diagnostics().front().help.has_value());
}

TEST_CASE("HIR-to-LIR lowering does not emit a value after a terminating block expression") {
    auto returned = std::make_unique<HirReturnStmt>();
    auto blockExpression = std::make_unique<HirBlockExpr>();
    blockExpression->block.stmts.push_back(std::move(returned));
    auto expressionStatement = std::make_unique<HirExprStmt>();
    expressionStatement->expr = std::move(blockExpression);

    HirFunc function;
    function.name = "TerminatingBlock";
    function.body.emplace();
    function.body->stmts.push_back(std::move(expressionStatement));
    HirModule module;
    module.funcs.push_back(std::move(function));
    HirPackage package;
    package.modules.push_back(std::move(module));

    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    static_cast<void>(lowering.Generate());

    CHECK(lowering.Diagnostics().empty());
}

TEST_CASE("optimization diagnostics identify the verifier pass and iteration") {
    LirFunc function;
    function.name = "Broken";
    LirModule module;
    module.funcs.push_back(std::move(function));
    LirPackage package;
    package.modules.push_back(std::move(module));

    auto pipeline = Optimization::OptimizationPipeline::ForProfile(BuildProfile::Debug);
    const auto report = pipeline.RunLir(package);

    REQUIRE(report.diagnostics.size() == 1);
    REQUIRE(report.diagnostics[0].notes.size() == 1);
    CHECK(report.diagnostics[0].notes[0].contains("lir-cfg-verifier"));
    CHECK(report.diagnostics[0].notes[0].contains("iteration 1"));
}

TEST_CASE("optimization fixed-point failures are accumulated as diagnostics") {
    HirPackage package;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 2);
    pipeline.Add(std::make_unique<AlwaysChangingPass>());

    const auto report = pipeline.Run(package);

    CHECK_FALSE(report.reachedFixedPoint);
    REQUIRE(report.diagnostics.size() == 1);
    CHECK(report.diagnostics[0].message == "IR optimization did not reach a fixed point after 2 iterations");
    REQUIRE(report.diagnostics[0].help.has_value());
}

TEST_CASE("optimization snapshots distinguish repeated pass instances and stop on output failure") {
    HirPackage package;
    Optimization::HirPassPipeline pipeline(BuildProfile::Release, 2);
    pipeline.Add(std::make_unique<AlwaysChangingPass>());
    pipeline.Add(std::make_unique<AlwaysChangingPass>());
    std::vector<std::pair<std::size_t, std::size_t>> snapshots;
    pipeline.SetObserver(
        [&](const HirPackage &, const std::string_view, const std::size_t iteration, const std::size_t index) {
            snapshots.emplace_back(iteration, index);
            return true;
        });
    static_cast<void>(pipeline.Run(package));
    const std::vector<std::pair<std::size_t, std::size_t>> expected = {{0, 0}, {1, 1}, {1, 2}, {2, 1}, {2, 2}};
    CHECK(snapshots == expected);

    snapshots.clear();
    const std::vector<std::string> disabled = {"always-changing"};
    pipeline.Configure(disabled, false);
    CHECK(pipeline.Run(package).reachedFixedPoint);
    REQUIRE_EQ(snapshots.size(), 1);
    pipeline.SetObserver([](const HirPackage &, std::string_view, std::size_t, std::size_t) { return false; });
    const auto failed = pipeline.Run(package);
    CHECK_FALSE(failed.reachedFixedPoint);
    REQUIRE(failed.HasErrors());
    CHECK(failed.diagnostics.front().message.contains("snapshot"));
}

TEST_CASE("driver retains distinct optimization snapshots without changing generated assembly") {
    DriverDiagnosticFixture fixture;
    fixture.WriteSource("func Main() -> int { let value = 21; return value * 2; }\n");
    auto options = fixture.Options(false);
    options.profile = BuildProfile::Release;
    options.target = *Target::TargetTriple::Parse("linux-x86_64");
    options.dumpAsm = true;
    const auto plain = CompilerDriver(options).Compile();
    REQUIRE(plain.ok);
    const auto asmPath = options.manifestPath.parent_path() / "Temp" / "Asm" / "out.asm";
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto assembly = read(asmPath);
    REQUIRE_FALSE(assembly.empty());
    options.dumpOptimizationPasses = true;
    const auto observed = CompilerDriver(options).Compile();
    REQUIRE(observed.ok);
    CHECK_EQ(read(asmPath), assembly);
    std::vector<std::filesystem::path> paths;
    for (const auto &output : observed.inspectionOutputs) {
        if (output.path.string().find("Optimization") != std::string::npos) {
            REQUIRE(std::filesystem::is_regular_file(output.path));
            paths.push_back(output.path);
        }
    }
    CHECK_GT(paths.size(), 10);
    const auto repeated = CompilerDriver(options).Compile();
    REQUIRE(repeated.ok);
    for (const auto &output : repeated.inspectionOutputs)
        if (output.path.string().find("Optimization") != std::string::npos)
            CHECK(std::ranges::find(paths, output.path) == paths.end());
}

TEST_CASE("driver reports an unwritable optimization snapshot destination") {
    DriverDiagnosticFixture fixture;
    auto options = fixture.Options(false);
    const auto temporary = options.manifestPath.parent_path() / "Temp";
    std::filesystem::create_directories(temporary);
    {
        std::ofstream blocked(temporary / "Optimization");
        blocked << "not a directory";
    }
    options.dumpOptimizationPasses = true;
    const auto result = CompilerDriver(options).Compile();
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.diagnostics.empty());
    CHECK(result.diagnostics.back().message.contains("snapshot directory"));
}
