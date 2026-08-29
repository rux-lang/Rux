#include "System/Os.h"
#include "System/Process.h"
#include "Target/TargetTriple.h"

#include <array>
#include <chrono>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace Rux;

namespace {
std::filesystem::path RuxExecutable() {
    return std::filesystem::path(RUX_ROOT_DIR) / "Bin" / System::ExecutableFileName("rux");
}

template <std::size_t N>
System::RunResult Run(const std::array<std::string_view, N> &arguments) {
    const auto result = System::RunCaptured(RuxExecutable(), arguments);
    REQUIRE(result.has_value());
    return *result;
}

std::string ReadTextFile(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void WriteTextFile(const std::filesystem::path &path, const std::string_view contents) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    REQUIRE(!error);
    std::ofstream output(path, std::ios::binary);
    output << contents;
    output.close();
    REQUIRE(output);
}
} // namespace

TEST_CASE("build reports every requested inspection output without changing dump payloads") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-inspection-output-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, R"([Manifest]
Version = 1

[Package]
Name = "InspectTest"
Version = "0.1.0"
Type = "Executable"

[Build]
Output = "Artifacts"
)");
    WriteTextFile(root / "Src" / "Nested" / "Main.rux", R"(func Main() -> int { return 0; }
struct Cell { value: int32; }
extend Cell {
    func =(self: &var Cell, other: &Cell);
    func <-(self: &var Cell, other: Cell) {}
    func ~Cell(self: &var Cell) {}
}
func Borrow(shared: &Cell, exclusive: &var Cell) -> int { return shared.value; }
enum State: uint8 { Idle = 1, Ready = 2 }
variant Result<T, E> { Success(T), Error(E) }
func ReadResult(value: Result<int32, State>) -> int32 {
    return match value { .Success(item) => item, .Error(_) => -1i32 };
}
)");
    const std::string manifest = manifestPath.string();

    struct EmissionCase {
        std::string_view kind;
        std::string_view heading;
        std::filesystem::path output;
    };

    const std::array cases{
        EmissionCase{"tokens", "token inspection output", "Temp/Tokens/Nested/Main.tokens"},
        EmissionCase{"ast", "AST inspection output", "Temp/Ast/Nested/Main.ast"},
        EmissionCase{"sema", "semantic inspection output", "Temp/Sema/sema.txt"},
        EmissionCase{"hir", "HIR inspection output", "Temp/Hir/hir.txt"},
        EmissionCase{"lir", "LIR inspection output", "Temp/Lir/lir.txt"},
        EmissionCase{"asm", "assembly inspection output", "Temp/Asm/out.asm"},
        EmissionCase{"rcu", "RCU inspection output", "Temp/Rcu/Main.rcu.txt"},
    };
    for (const auto &test : cases) {
        const auto target =
            test.kind == "asm" ? std::string_view("linux-x86_64") : Target::TargetTriple::Host().CanonicalName();
        const auto emitted = Run(std::array<std::string_view, 8>{"--manifest", manifest, "--color=never", "build",
                                                                 "--target", target, "--emit", test.kind});
        CAPTURE(test.kind);
        INFO(emitted.output);
        CHECK(emitted.exitCode == 0);
        CHECK(emitted.output.contains("Emitted " + std::string(test.heading)));
        CHECK(emitted.output.contains("Description: "));
        if (test.kind == "sema") {
            CHECK(emitted.output.contains("resolved symbols, signatures, type capabilities, and diagnostics"));
        }
        CHECK(emitted.output.contains("Output: " + std::filesystem::path(test.output).make_preferred().string()));
        CHECK(std::filesystem::is_regular_file(root / test.output));
    }

    const auto combined =
        Run(std::array<std::string_view, 8>{"--manifest", manifest, "--color=never", "build", "--target",
                                            "linux-x86_64", "--emit", "tokens,ast,sema,hir,lir,asm,rcu"});
    CHECK(combined.exitCode == 0);
    for (const auto &test : cases) {
        CHECK(combined.output.contains("Emitted " + std::string(test.heading)));
    }
    CHECK(combined.output.contains("Emitted RCU object"));
    CHECK(combined.output.contains(
        "Output: " + std::filesystem::path("Artifacts/Debug/Linux/x86-64/InspectTest").make_preferred().string()));

    const auto tokens = ReadTextFile(root / "Temp" / "Tokens" / "Nested" / "Main.tokens");
    const auto ast = ReadTextFile(root / "Temp" / "Ast" / "Nested" / "Main.ast");
    const auto sema = ReadTextFile(root / "Temp" / "Sema" / "sema.txt");
    const auto hir = ReadTextFile(root / "Temp" / "Hir" / "hir.txt");
    const auto lir = ReadTextFile(root / "Temp" / "Lir" / "lir.txt");
    const auto assembly = ReadTextFile(root / "Temp" / "Asm" / "out.asm");
    const auto rcu = ReadTextFile(root / "Temp" / "Rcu" / "Main.rcu.txt");
    CHECK(tokens.starts_with("   1:1     FuncKeyword"));
    CHECK(ast.starts_with("Module \""));
    CHECK(ast.contains("FuncDecl '~Cell' (self: &var Cell)"));
    CHECK(ast.contains("EnumDecl 'State' : uint8"));
    CHECK(ast.contains("Member 'Idle' = 1"));
    CHECK(ast.contains("VariantDecl 'Result'<T, E>"));
    CHECK(ast.contains("Case 'Success' (T)"));
    CHECK(sema.contains("func Borrow(shared: &Cell, exclusive: &var Cell) -> int"));
    CHECK(sema.contains("Cell                          copy=prohibited move=custom drop=yes"));
    CHECK(hir.starts_with("=== High-level Intermediate Representation ==="));
    CHECK(hir.contains("enum State: uint8"));
    CHECK(hir.contains("variant Result<T, E>"));
    CHECK_FALSE(hir.contains("variant Result<T, E>:"));
    CHECK(lir.starts_with("=== Low-level Intermediate Representation ==="));
    CHECK(lir.contains("enum State: uint8"));
    CHECK(lir.contains("variant Result<T, E>"));
    CHECK_FALSE(lir.contains("variant Result<T, E>:"));
    CHECK_FALSE(assembly.starts_with("Emitted"));
    CHECK(rcu.starts_with("; RCU  Rux Compiled Unit  v1.0"));
    for (const std::string_view payload : {tokens, ast, sema, hir, lir, assembly, rcu}) {
        CHECK_FALSE(payload.contains("Description:"));
    }

    const auto unavailable = Run(std::array<std::string_view, 8>{"--manifest", manifest, "--color=never", "build",
                                                                 "--target", "linux-aarch64", "--emit", "asm"});
    CHECK(unavailable.exitCode == 0);
    CHECK(unavailable.output.contains("warning: assembly inspection output is unavailable for target 'linux-aarch64'"));
    CHECK(unavailable.output.contains("textual assembly inspection is currently supported only for x86-64 targets"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

TEST_CASE("inspection write failures identify the output and filesystem cause") {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = System::TempDirectory() / ("rux-inspection-failure-test-" + std::to_string(nonce));
    const auto manifestPath = root / "Rux.toml";
    WriteTextFile(manifestPath, "[Manifest]\nVersion = 1\n\n[Package]\nName = \"InspectFailure\"\n"
                                "Version = \"0.1.0\"\nType = \"Executable\"\n");
    WriteTextFile(root / "Src" / "Main.rux", "func Main() -> int { return 0; }\n");
    WriteTextFile(root / "Temp" / "Hir", "blocks the inspection directory\n");
    const std::string manifest = manifestPath.string();

    const auto failed =
        Run(std::array<std::string_view, 6>{"--manifest", manifest, "--color=never", "build", "--emit", "hir"});
    CHECK(failed.exitCode == 1);
    CHECK(failed.output.contains("error: could not write HIR inspection output to '"));
    CHECK(failed.output.contains((root / "Temp" / "Hir" / "hir.txt").string()));
    CHECK(failed.output.contains("note: filesystem error"));
    CHECK(failed.output.contains("help: check that the destination directory is writable"));

    std::error_code error;
    std::filesystem::remove_all(root, error);
}
