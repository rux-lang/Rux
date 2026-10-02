// How native values are stored in LIR: each level is a tag word at offset zero followed by the active payload at the
// offset the shared native layout gives it, a zero-sized payload writes no bytes, copies follow a case-aware recipe,
// and both back ends accept the storage as an ordinary aggregate.

#include "CodeGen/AArch64/RcuEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"
#include "Target/Target.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
/// The LIR of `source`. `none` is still stopped before compilation by a pending diagnostic, which is the only error the
/// source may produce; lowering reads the facts analysis recorded anyway.
LirPackage LowerToLir(const std::string &source) {
    Lexer lexer(source, "native.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "native.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            INFO(diagnostic.message);
            REQUIRE(diagnostic.message.contains("is not supported"));
        }
    }
    AstToHirLowering hirLowering(model);
    HirPackage hir = hirLowering.Generate();
    REQUIRE(hirLowering.Diagnostics().empty());
    HirToLirLowering lirLowering(std::move(hir), TargetContext::CreateNative());
    LirPackage lir = lirLowering.Generate();
    for (const auto &diagnostic : lirLowering.Diagnostics()) {
        INFO(diagnostic.message);
        CHECK(diagnostic.severity != Diagnostic::Severity::Error);
    }
    return lir;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name || function.name.ends_with("::" + name) || function.name.ends_with("." + name)) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

std::vector<const LirInstr *> Instructions(const LirFunc &function) {
    std::vector<const LirInstr *> instructions;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            instructions.push_back(&instruction);
        }
    }
    return instructions;
}

/// The constant a register was defined from, if it was.
std::optional<std::string> ConstantOf(const LirFunc &function, const LirReg reg) {
    for (const LirInstr *instruction : Instructions(function)) {
        if (instruction->dst == reg && instruction->op == LirOpcode::Const) {
            return instruction->strArg;
        }
    }
    return std::nullopt;
}

/// The byte offsets at which values of `type` are stored through a byte-indexed pointer.
std::vector<std::string> StoredOffsets(const LirFunc &function, const TypeRef &type) {
    std::vector<std::string> offsets;
    const std::vector<const LirInstr *> instructions = Instructions(function);
    for (const LirInstr *store : instructions) {
        if (store->op != LirOpcode::Store || store->type != type || store->srcs.size() < 2) {
            continue;
        }
        for (const LirInstr *index : instructions) {
            if (index->dst == store->srcs[1] && index->op == LirOpcode::IndexPtr && index->srcs.size() == 2) {
                offsets.push_back(ConstantOf(function, index->srcs[1]).value_or("?"));
            }
        }
    }
    return offsets;
}

bool StoresType(const LirFunc &function, const TypeRef &type) {
    return std::ranges::any_of(Instructions(function), [&](const LirInstr *instruction) {
        return instruction->op == LirOpcode::Store && instruction->type == type;
    });
}

void RequireBackEndsAccept(const LirPackage &package) {
    const RcuEmitter x86(package, "test", Target::OS::Windows);
    static_cast<void>(x86.Generate());
    CHECK(x86.Diagnostics().empty());
    const AArch64RcuEmitter aarch64(package, "test", Target::OS::Linux);
    static_cast<void>(aarch64.Generate());
    CHECK(aarch64.Diagnostics().empty());
}
} // namespace

TEST_CASE("a native level stores its tag first and its payload after the tag") {
    const LirPackage package = LowerToLir(R"(
        struct ParseError { code: int32; }
        func Present() -> int32? {
            return .Some(7i32);
        }
        func Failed() -> int64 ! ParseError {
            return .Failure(ParseError { code: 3i32 });
        }
    )");
    const LirFunc &present = RequireFunction(package, "Present");
    CHECK(StoresType(present, TypeRef::MakeInt64()));
    CHECK_EQ(StoredOffsets(present, TypeRef::MakeInt32()), std::vector<std::string>{"8"});

    const LirFunc &failed = RequireFunction(package, "Failed");
    CHECK_EQ(StoredOffsets(failed, TypeRef::MakeNamed("ParseError")), std::vector<std::string>{"8"});
    RequireBackEndsAccept(package);
}

TEST_CASE("payloads wider than a register and nested levels keep their place") {
    const LirPackage package = LowerToLir(R"(
        struct Big { a: int64; b: int64; c: int64; }
        struct E {}
        func Wide() -> Big ! E {
            return .Success(Big { a: 1i64, b: 2i64, c: 3i64 });
        }
        func Nested() -> (int32 ! E)? {
            return .Some(.Success(4i32));
        }
    )");
    const LirFunc &wide = RequireFunction(package, "Wide");
    CHECK_EQ(StoredOffsets(wide, TypeRef::MakeNamed("Big")), std::vector<std::string>{"8"});

    // The inner level is a complete value of its own type at the outer payload's offset, written by the inner level.
    const LirFunc &nested = RequireFunction(package, "Nested");
    CHECK(StoresType(nested, TypeRef::MakeFallible(TypeRef::MakeInt32(), TypeRef::MakeNamed("E"))));
    CHECK_EQ(StoredOffsets(nested, TypeRef::MakeInt32()), std::vector<std::string>{"8"});
    RequireBackEndsAccept(package);
}

TEST_CASE("a zero-sized payload writes no data bytes") {
    const LirPackage package = LowerToLir(R"(
        struct E {}
        func Done() -> ! E {
            return .Success(());
        }
        func Unit() -> ()? {
            return .Some(());
        }
    )");
    for (const char *name : {"Done", "Unit"}) {
        const LirFunc &function = RequireFunction(package, name);
        CHECK_FALSE(StoresType(function, TypeRef::MakeUnit()));
        CHECK(StoresType(function, TypeRef::MakeInt64()));
    }
    RequireBackEndsAccept(package);
}

TEST_CASE("copying a native value copies only the active payload recursively") {
    const LirPackage package = LowerToLir(R"(
        struct Counted { value: int32; }
        extend Counted {
            func =(self: &var Counted, other: &Counted) {
                self.value = other.value + 1i32;
            }
        }
        func Keep(value: Counted?) {}
        func Copy(value: Counted?) {
            Keep(value);
            Keep(value);
        }
    )");
    const LirFunc &copy = RequireFunction(package, "Copy");
    const bool branchesOnTag =
        std::ranges::any_of(copy.blocks, [](const LirBlock &block) { return block.label.starts_with("copy.variant"); });
    CHECK(branchesOnTag);
    RequireBackEndsAccept(package);
}

TEST_CASE("widening rebuilds a value in the destination's storage") {
    const LirPackage package = LowerToLir(R"(
        struct A { value: int32; }
        struct B {}
        struct C { value: int64; }
        func Widen(pair: A | C) -> A | B | C {
            return pair;
        }
    )");
    const LirFunc &widen = RequireFunction(package, "Widen");
    // `C` moves from member one to member two, with its payload still after the tag.
    CHECK_EQ(StoredOffsets(widen, TypeRef::MakeNamed("C")), std::vector<std::string>{"8"});
    CHECK_EQ(StoredOffsets(widen, TypeRef::MakeNamed("A")), std::vector<std::string>{"8"});
    RequireBackEndsAccept(package);
}
