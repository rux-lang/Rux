// The inline assembly the first-party packages ship, read out of the packages
// themselves rather than out of a copy: each source is lexed, parsed for the
// architecture being compiled for, folded through `when` the way the driver
// folds it, and handed to that architecture's assembler.
//
// Two things are asserted. The system-call wrappers assemble to the words each
// kernel's ABI asks for, which is what makes `linux-aarch64` reach the kernel
// at all before anything can run there. And no first-party body is left written
// for the other architecture, which is the premise under which a foreign
// mnemonic is an error rather than a warning.
//
// Every expected word below came from `llvm-mc -triple=aarch64 -show-encoding`
// on the mnemonic named beside it, so a disagreement here is a disagreement
// with a second implementation rather than with someone's reading of the ARM
// manual.

#include "CodeGen/AArch64/Assembler.h"
#include "CodeGen/X86_64/Assembler.h"
#include "Driver/BuildTarget.h"
#include "Driver/CompilerDriver.h"
#include "Lexer/Lexer.h"
#include "Semantic/CompileTimeContext.h"
#include "Semantic/ConditionalCompilation.h"
#include "Syntax/Ast/Ast.h"
#include "Syntax/Parser/Parser.h"
#include "Target/AsmInstr.h"

#include <cstddef>
#include <cstdint>
#include <doctest.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace Rux;

namespace {
// Every AArch64 instruction is this many bytes wide, with no exceptions.
constexpr std::size_t InstrSize = 4;

// One `asm func` a package left behind, and the machine code it assembled to.
struct AssembledBody {
    std::string name;
    std::vector<std::uint32_t> words;
};

struct AssembledSource {
    std::vector<AssembledBody> bodies;
    std::map<std::string, std::string> constants;
    std::vector<Diagnostic> diagnostics;
};

std::string ReadFileText(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// An instruction word as a table spells it: doctest reports an integer in
// decimal, which says nothing at a glance against a column of hexadecimal.
[[nodiscard]] std::string HexWord(const std::uint32_t word) {
    return std::format("0x{:08X}", word);
}

// Assemble every `asm func` `path` leaves behind once `when` has folded for
// `triple`. Everything the driver does ahead of code generation is done here in
// the same order, since which body survives the fold is half of what is being
// asserted.
[[nodiscard]] AssembledSource AssembleSourceFor(const std::filesystem::path &path, const std::string &triple) {
    const std::string fileName = path.filename().string();
    const std::string source = ReadFileText(path);
    REQUIRE_MESSAGE(!source.empty(), "package source is missing or empty: ", path.string());

    Lexer lexer(source, fileName);
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());

    const TargetContext target = Driver::TargetContextForTriple(*Target::TargetTriple::Parse(triple));
    Parser parser(std::move(lexed.tokens), fileName, target.arch);
    auto parsed = parser.Parse();
    for (const auto &diag : parsed.diagnostics) {
        INFO("unexpected parse diagnostic in ", path.string(), " for ", triple, ": ", diag.message);
        CHECK(diag.severity != Diagnostic::Severity::Error);
    }

    AssembledSource result;
    CompileTimeContext context;
    context.target = target;
    context.targetTriple = triple;
    std::vector<Module *> modules = {&parsed.module};
    ResolveConditionalCompilation(modules, context, result.diagnostics);

    std::vector<std::uint8_t> code;
    for (const auto &item : parsed.module.items) {
        if (const auto *constant = dynamic_cast<const ConstDecl *>(item.get())) {
            if (const auto *literal = dynamic_cast<const LiteralExpr *>(constant->value.get())) {
                result.constants.emplace(constant->name, literal->token.text);
            }
            continue;
        }
        const auto *func = dynamic_cast<const FuncDecl *>(item.get());
        if (func == nullptr || !func->isAsm) {
            continue;
        }
        // The mnemonics are the only thing that says which machine a body was
        // written for, and after the fold every one of them must say this one.
        for (const auto &instr : func->asmBody) {
            if (instr.mnemonic.empty()) {
                continue; // a label definition
            }
            const Target::Arch mnemonicArch = AsmMnemonicArch(instr.mnemonic);
            const bool belongsHere = mnemonicArch == Target::Arch::Unknown || mnemonicArch == target.arch;
            INFO("in ", path.string(), " for ", triple, ", asm func '", func->name, "'");
            CHECK_MESSAGE(belongsHere, "instruction of the other architecture: ", instr.mnemonic);
        }

        code.clear();
        const AsmAssembly assembled = target.arch == Target::Arch::AArch64
                                        ? AssembleAArch64AsmFunc(func->asmBody, fileName, code, target.os)
                                        : AssembleAsmFunc(func->asmBody, fileName, code, target.os);
        for (const auto &diag : assembled.diagnostics) {
            INFO("assembling ", path.string(), " for ", triple);
            CHECK_MESSAGE(diag.severity != Diagnostic::Severity::Error, func->name, ": ", diag.message);
        }
        CHECK_MESSAGE(assembled.fixups.empty(), "a package body left a relocation behind: ", func->name);

        AssembledBody body;
        body.name = func->name;
        for (std::size_t offset = 0; offset + InstrSize <= code.size(); offset += InstrSize) {
            std::uint32_t word = 0;
            for (std::size_t i = 0; i < InstrSize; ++i) {
                word |= static_cast<std::uint32_t>(code[offset + i]) << (i * 8U);
            }
            body.words.push_back(word);
        }
        result.bodies.push_back(std::move(body));
    }
    return result;
}

[[nodiscard]] const AssembledBody *FindBody(const AssembledSource &source, const std::string_view name) {
    for (const auto &body : source.bodies) {
        if (body.name == name) {
            return &body;
        }
    }
    return nullptr;
}

[[nodiscard]] std::filesystem::path PackageSource(const std::string_view package, const std::string_view file) {
    return std::filesystem::path(RUX_PACKAGES_DIR) / package / "Src" / file;
}

[[nodiscard]] std::filesystem::path PackageTestManifest(const std::string_view platform, const std::string_view test) {
    return std::filesystem::path(RUX_TESTS_DIR) / "Packages" / platform / test / "Rux.toml";
}

// The words the two system-call wrappers are built from. `MOV` between two
// X registers is `ORR Xd, XZR, Xm`, which is why the shuffle is one opcode with
// the source register moving through the Rm field.
//
// Darwin's `mov x16, x0` and `svc #0x80` are gone with the package that wrote them: Apple does not support the trap
// interface, and that package now binds libSystem.
constexpr std::uint32_t MovX8FromX0 = 0xAA0003E8; // mov x8, x0
constexpr std::uint32_t MovDownOne[] = {
    0xAA0103E0, // mov x0, x1
    0xAA0203E1, // mov x1, x2
    0xAA0303E2, // mov x2, x3
    0xAA0403E3, // mov x3, x4
    0xAA0503E4, // mov x4, x5
    0xAA0603E5, // mov x5, x6
};
constexpr std::uint32_t Svc0 = 0xD4000001;       // svc #0
constexpr std::uint32_t CnegX0OnCs = 0xDA803400; // cneg x0, x0, cs
constexpr std::uint32_t Ret = 0xD65F03C0;        // ret

// The wrapper taking `argumentCount` arguments: the call number aside into the
// register the kernel reads it from, the arguments down by one register each,
// the trap, and — where errors arrive as carry with a positive errno — the
// negation that turns one into the -errno every wrapper returns.
[[nodiscard]] std::vector<std::uint32_t> ExpectedWrapper(const std::size_t argumentCount,
                                                         const std::uint32_t numberMove, const std::uint32_t trap,
                                                         const bool carryMeansError) {
    std::vector<std::uint32_t> words{numberMove};
    for (std::size_t i = 0; i < argumentCount; ++i) {
        words.push_back(MovDownOne[i]);
    }
    words.push_back(trap);
    if (carryMeansError) {
        words.push_back(CnegX0OnCs);
    }
    words.push_back(Ret);
    return words;
}

void CheckWrappers(const AssembledSource &source, const std::uint32_t numberMove, const std::uint32_t trap,
                   const bool carryMeansError) {
    for (std::size_t arguments = 0; arguments <= 6; ++arguments) {
        const std::string name = std::format("Syscall{}", arguments);
        const auto *body = FindBody(source, name);
        REQUIRE_MESSAGE(body != nullptr, "the fold left no body for ", name);

        const std::vector<std::uint32_t> expected = ExpectedWrapper(arguments, numberMove, trap, carryMeansError);
        REQUIRE_MESSAGE(body->words.size() == expected.size(), name, " assembled to the wrong number of instructions");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            INFO(name, " instruction ", i);
            CHECK(HexWord(body->words[i]) == HexWord(expected[i]));
        }
    }
}

// The operating system a package's `when #target.os` arms are written for. The
// portable packages have none and are folded for Linux, which their sources
// never ask about.
[[nodiscard]] std::string_view OsForPackage(const std::string_view package) {
    if (package == "FreeBSD") {
        return "freebsd";
    }
    if (package == "macOS") {
        return "macos";
    }
    if (package == "Windows") {
        return "windows";
    }
    return "linux";
}

} // namespace

TEST_CASE("Linux AArch64 system-call wrappers trap through x8 and return the kernel's result") {
    const AssembledSource linux = AssembleSourceFor(PackageSource("Linux", "Syscall.rux"), "linux-aarch64");
    CHECK(linux.diagnostics.empty());
    // Linux hands back either a non-negative result or -errno, so nothing
    // follows the trap but the return.
    CheckWrappers(linux, MovX8FromX0, Svc0, /*carryMeansError=*/false);
}

TEST_CASE("macOS writes no inline assembly at all, because Apple does not support the trap interface") {
    // This package used to issue `svc #0x80` with a Darwin trap number. Apple does not support that: the numbers are
    // private, they have changed between releases, and on Apple Silicon the interface is not reachable from ordinary
    // code, so the AArch64 half was binding something that cannot be called. Every call goes through libSystem now,
    // and the guard against a regression is that no source in the package writes the keyword.
    const std::filesystem::path macosSource =
        std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR)) / "macOS" / "Src";
    REQUIRE(std::filesystem::is_directory(macosSource));

    bool bindsLibSystem = false;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(macosSource)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".rux") {
            continue;
        }
        const std::string text = ReadFileText(entry.path());
        INFO(entry.path().string());
        CHECK(text.find("asm func") == std::string::npos);
        if (text.find("libSystem.B.dylib") != std::string::npos) {
            bindsLibSystem = true;
        }
    }
    CHECK_MESSAGE(bindsLibSystem, "the package binds no library, so it reaches the system some other way");
}

TEST_CASE("FreeBSD AArch64 BSD system-call wrappers assemble through x8 and normalize a carry error") {
    const AssembledSource bsd = AssembleSourceFor(PackageSource("FreeBSD", "Syscall.rux"), "freebsd-aarch64");
    CHECK(bsd.diagnostics.empty());
    CheckWrappers(bsd, MovX8FromX0, Svc0, /*carryMeansError=*/true);
}

TEST_CASE("FreeBSD AArch64 BSD constants match the 14.4 syscall and mmap surface") {
    // The constants now live in the module that owns the domain, so each group is read from the file that declares
    // it. That the numbers are unchanged by the split is the whole point of checking them here.
    const std::map<std::string, std::map<std::string, std::string>> expected = {
        {"Syscall.rux",
         {
             {"SysExit", "1"},
             {"SysRead", "3"},
             {"SysWrite", "4"},
             {"SysClose", "6"},
             {"SysBrk", "17"},
             {"SysGetPid", "20"},
             {"SysMunmap", "73"},
             {"SysClockGetTime", "232"},
             {"SysNanosleep", "240"},
             {"SysMmap", "477"},
         }},
        {"Clock.rux",
         {
             {"ClockRealtime", "0"},
             {"ClockMonotonic", "4"},
         }},
        {"Memory.rux",
         {
             {"ProtectionNone", "0x00"},
             {"ProtectionRead", "0x01"},
             {"ProtectionWrite", "0x02"},
             {"ProtectionExecute", "0x04"},
             {"MapShared", "0x0001"},
             {"MapPrivate", "0x0002"},
             {"MapFixed", "0x0010"},
             {"MapAnonymous", "0x1000"},
         }},
    };
    for (const auto &[file, constants] : expected) {
        const AssembledSource module = AssembleSourceFor(PackageSource("FreeBSD", file), "freebsd-aarch64");
        INFO("FreeBSD module ", file);
        CHECK(module.diagnostics.empty());
        for (const auto &[name, value] : constants) {
            INFO("FreeBSD constant ", name);
            const auto found = module.constants.find(name);
            REQUIRE(found != module.constants.end());
            CHECK_EQ(found->second, value);
        }
    }
}

TEST_CASE("FreeBSD AArch64 BSD package checks both target conditions through the public artifact path") {
    const std::filesystem::path manifestPath = PackageTestManifest("FreeBSD", "Syscall");
    auto loaded = Manifest::Load(manifestPath);
    REQUIRE_MESSAGE(loaded.Ok(), manifestPath.string());

    std::vector<Diagnostic> diagnostics;

    Driver::CompileOptions options;
    options.manifestPath = manifestPath;
    options.manifest = std::move(*loaded.manifest);
    options.target = *Target::TargetTriple::Parse("freebsd-aarch64");
    options.profile = BuildProfile::Release;
    options.isTest = true;
    options.checkOnly = true;
    options.emitDiagnostic = [&diagnostics](const Diagnostic &diagnostic, const SourceLineLookup &) {
        diagnostics.push_back(diagnostic);
    };

    const Driver::CompileResult result = Driver::CompilerDriver(std::move(options)).Compile();
    std::string reports;
    for (const auto &diagnostic : diagnostics) {
        reports += (reports.empty() ? "" : " | ") + diagnostic.message;
    }
    CHECK_MESSAGE(result.ok, "the FreeBSD/AArch64-conditioned BSD package did not check: ", reports);
    CHECK(diagnostics.empty());
    CHECK(result.primaryArtifactPath.empty());

    // The exact inline bodies were assembled to AArch64 bytes above, and the
    // public driver exposes the same target to artifact-producing commands.
    CHECK(Driver::IsSupportedTargetTriple("freebsd-aarch64"));
    CHECK(Driver::IsSupportedTargetTriple("freebsd-arm64"));
}

TEST_CASE("Math's AArch64 bodies are the one instruction each operation names") {
    const AssembledSource sqrt = AssembleSourceFor(PackageSource("Math", "Sqrt.rux"), "linux-aarch64");
    REQUIRE(sqrt.bodies.size() == 2);
    // Both overloads are spelled `Sqrt` in the source; the order they were
    // written in is the order they arrive in.
    CHECK(HexWord(sqrt.bodies[0].words.at(0)) == HexWord(0x1E21C000)); // fsqrt s0, s0
    CHECK(HexWord(sqrt.bodies[1].words.at(0)) == HexWord(0x1E61C000)); // fsqrt d0, d0
    for (const auto &body : sqrt.bodies) {
        REQUIRE(body.words.size() == 2);
        CHECK(HexWord(body.words[1]) == HexWord(Ret));
    }

    const AssembledSource bits = AssembleSourceFor(PackageSource("Math", "Bits.rux"), "linux-aarch64");
    const auto *floatBits = FindBody(bits, "FloatBits");
    const auto *fromBits = FindBody(bits, "FromBits");
    REQUIRE(floatBits != nullptr);
    REQUIRE(fromBits != nullptr);
    CHECK(floatBits->words == std::vector<std::uint32_t>{0x9E660000, Ret}); // fmov x0, d0
    CHECK(fromBits->words == std::vector<std::uint32_t>{0x9E670000, Ret});  // fmov d0, x0
}

TEST_CASE("every first-party asm func has a body for the architecture being compiled for") {
    std::size_t sourcesWithAsm = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             std::filesystem::weakly_canonical(std::filesystem::path(RUX_PACKAGES_DIR)))) {
        if (!entry.is_regular_file() || entry.path().extension() != ".rux") {
            continue;
        }
        // Parsing every package to find out which of them writes assembly would
        // cost the whole tree; the keyword is in the source or it is not.
        if (ReadFileText(entry.path()).find("asm func") == std::string::npos) {
            continue;
        }
        ++sourcesWithAsm;

        const std::string package = entry.path().parent_path().parent_path().filename().string();
        for (const std::string_view arch : {"x86_64", "aarch64"}) {
            // AssembleSourceFor asserts, per body, that every mnemonic belongs
            // to this architecture and that the assembler for it encodes the
            // whole body with nothing left unresolved.
            const AssembledSource assembled =
                AssembleSourceFor(entry.path(), std::format("{}-{}", OsForPackage(package), arch));
            for (const auto &diag : assembled.diagnostics) {
                INFO(entry.path().string(), " for ", arch);
                CHECK_MESSAGE(diag.severity != Diagnostic::Severity::Error, diag.message);
            }
            CHECK_MESSAGE(!assembled.bodies.empty(), "the fold left no asm func at all in ", entry.path().string(),
                          " for ", arch);
        }
    }
    // Four: the two syscall modules, and the two Math sources that reach for an instruction the language has no
    // operator for. macOS was the fifth until it moved to libSystem, which is what the case above guards.
    CHECK_MESSAGE(sourcesWithAsm >= 4, "a package source writing inline assembly went unchecked");
}

TEST_CASE("x86-64 encodes atomic and synchronization instructions") {
    const std::string source = R"(
        asm func Atomics() {
            pause
            mfence
            lfence
            sfence
            lock cmpxchg qword [rcx], rdx
            lock xadd qword [rcx], rax
            xchg qword [rcx], rdx
            lock add qword [rcx], 1
            ret
        }
    )";
    Lexer lexer(source, "atomics.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "atomics.rux", Target::Arch::X86_64);
    auto parsed = parser.Parse();
    REQUIRE(parsed.diagnostics.empty());
    std::vector<std::uint8_t> code;
    for (const auto &item : parsed.module.items) {
        if (const auto *func = dynamic_cast<const FuncDecl *>(item.get()); func && func->isAsm) {
            auto asmResult = AssembleAsmFunc(func->asmBody, "atomics.rux", code, Target::OS::Windows);
            CHECK(asmResult.ok);
            CHECK(asmResult.diagnostics.empty());
        }
    }
    CHECK_FALSE(code.empty());
}

TEST_CASE("x86-64 encodes packed SSE2 vector instructions") {
    const std::string source = R"(
        asm func VectorOps() {
            movups xmm0, [rcx]
            movups xmm1, [rdx]
            addps xmm0, xmm1
            subps xmm0, xmm1
            mulps xmm0, xmm1
            divps xmm0, xmm1
            sqrtps xmm0, xmm1
            paddd xmm0, xmm1
            psubd xmm0, xmm1
            movups [rax], xmm0
            ret
        }
    )";
    Lexer lexer(source, "vectors.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "vectors.rux", Target::Arch::X86_64);
    auto parsed = parser.Parse();
    REQUIRE(parsed.diagnostics.empty());
    std::vector<std::uint8_t> code;
    for (const auto &item : parsed.module.items) {
        if (const auto *func = dynamic_cast<const FuncDecl *>(item.get()); func && func->isAsm) {
            auto asmResult = AssembleAsmFunc(func->asmBody, "vectors.rux", code, Target::OS::Windows);
            CHECK(asmResult.ok);
            CHECK(asmResult.diagnostics.empty());
        }
    }
    CHECK_FALSE(code.empty());
}
