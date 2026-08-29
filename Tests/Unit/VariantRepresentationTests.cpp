#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Rux;

namespace {
HirPackage VariantHir(const std::string &source, const TargetContext target = TargetContext::CreateNative()) {
    Lexer lexer(source, "variant-representation.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "variant-representation.rux", target.arch);
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    CompileTimeContext context;
    context.target = target;
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", context);
    const SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

LirPackage VariantLir(const std::string &source, const TargetContext target = TargetContext::CreateNative()) {
    HirToLirLowering lowering(VariantHir(source, target), target);
    LirPackage package = lowering.Generate();
    for (const Diagnostic &diagnostic : lowering.Diagnostics()) {
        INFO(diagnostic.message);
    }
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const HirFunc &HirFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing HIR function " << name);
    throw std::runtime_error("missing HIR function");
}

const LirFunc &LirFunction(const LirPackage &package, const std::string &name) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing LIR function " << name);
    throw std::runtime_error("missing LIR function");
}

const HirMatchExpr &ReturnedMatch(const HirPackage &package, const std::string &name) {
    const HirFunc &function = HirFunction(package, name);
    REQUIRE(function.body.has_value());
    REQUIRE_EQ(function.body->stmts.size(), 1);
    const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *match = dynamic_cast<const HirMatchExpr *>(returned->value->get());
    REQUIRE(match != nullptr);
    return *match;
}

std::size_t CountOpcode(const LirFunc &function, const LirOpcode opcode) {
    std::size_t count = 0;
    for (const LirBlock &block : function.blocks) {
        count += std::ranges::count_if(block.instrs,
                                       [opcode](const LirInstr &instruction) { return instruction.op == opcode; });
    }
    return count;
}

std::vector<TypeRef> Allocations(const LirFunc &function, const std::string &typeName) {
    std::vector<TypeRef> types;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && instruction.type.name == typeName) {
                types.push_back(instruction.type);
            }
        }
    }
    return types;
}

TargetContext AArch64Target() {
    return {.os = Target::OS::Linux,
            .arch = Target::Arch::AArch64,
            .data_model = Target::DataModel::LP64,
            .abi = Target::ABI::AAPCS64,
            .default_cc = Target::CallingConv::AAPCS64,
            .endianness = Target::Endian::Little,
            .object_format = Target::ObjectFormat::ELF,
            .pointer_size = 8,
            .cpu_features = Target::CpuFeature::NEON};
}

TargetContext X86Target() {
    return {.os = Target::OS::Linux,
            .arch = Target::Arch::X86_64,
            .data_model = Target::DataModel::LP64,
            .abi = Target::ABI::SystemV,
            .default_cc = Target::CallingConv::SysV,
            .endianness = Target::Endian::Little,
            .object_format = Target::ObjectFormat::ELF,
            .pointer_size = 8,
            .cpu_features = Target::CpuFeature::SSE2};
}

const std::string kShapes = R"(
    variant State { Idle, Ready, Complete }
    variant Parcel {
        Empty,
        Byte(uint8),
        Pair(uint32, uint32),
        Named { sequence: uint64; enabled: bool8; }
    }
    variant Maybe<T> { None, Some(T) }
)";
} // namespace

TEST_CASE("HIR construction retains variant form for unit tuple and named cases") {
    const HirPackage package = VariantHir(kShapes + R"(
        func Unit() -> State { return State::Ready; }
        func Tuple() -> Parcel { return Parcel::Pair(20u32, 22u32); }
        func Named() -> Parcel { return Parcel::Named { sequence: 42u64, enabled: true }; }
    )");

    for (const std::string name : {"Unit", "Tuple", "Named"}) {
        const HirFunc &function = HirFunction(package, name);
        REQUIRE(function.body.has_value());
        REQUIRE_EQ(function.body->stmts.size(), 1);
        const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
        REQUIRE(returned != nullptr);
        REQUIRE(returned->value.has_value());
        const auto *construction = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
        REQUIRE_MESSAGE(construction != nullptr, name);
        CHECK_EQ(construction->form, CaseTypeForm::Variant);
        CHECK_EQ(construction->type.name, name == "Unit" ? "State" : "Parcel");
    }
}

TEST_CASE("scalar enum members remain scalar literals rather than tagged constructions") {
    const HirPackage package = VariantHir(R"(
        enum Code: uint8 { Ready = 1, Busy = 2 }
        func Ready() -> Code { return Code::Ready; }
    )");
    const HirFunc &function = HirFunction(package, "Ready");
    REQUIRE(function.body.has_value());
    const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    REQUIRE(returned->value.has_value());
    const auto *literal = dynamic_cast<const HirLiteralExpr *>(returned->value->get());
    REQUIRE(literal != nullptr);
    CHECK_EQ(literal->value, "1");
    CHECK_EQ(literal->type.name, "Code");
}

TEST_CASE("unit variants allocate tagged storage even without payload fields") {
    const LirPackage package = VariantLir(kShapes + R"(
        func Unit() -> State { return State::Ready; }
        func Main() -> int { let value = Unit(); return 0; }
    )");
    const LirFunc &unit = LirFunction(package, "Unit");
    const auto slots = Allocations(unit, "State");
    REQUIRE_EQ(slots.size(), 1);
    REQUIRE_EQ(slots.front().inner.size(), 1);
    CHECK_EQ(slots.front().inner.front().kind, TypeRef::Kind::Array);
    CHECK_EQ(slots.front().SizeInBytes(), 8);
    CHECK_GE(CountOpcode(unit, LirOpcode::Store), 1);
}

TEST_CASE("tuple and named payload construction use addressable offsets") {
    const LirPackage package = VariantLir(kShapes + R"(
        func Tuple() -> Parcel { return Parcel::Pair(20u32, 22u32); }
        func Named() -> Parcel { return Parcel::Named { sequence: 42u64, enabled: true }; }
    )");
    for (const std::string name : {"Tuple", "Named"}) {
        const LirFunc &function = LirFunction(package, name);
        CHECK_GE(CountOpcode(function, LirOpcode::IndexPtr), 2);
        CHECK_GE(CountOpcode(function, LirOpcode::Store), 3);
        const auto slots = Allocations(function, "Parcel");
        REQUIRE_EQ(slots.size(), 1);
        CHECK_EQ(slots.front().SizeInBytes(), 24);
    }
}

TEST_CASE("generic variant construction keeps concrete layout in parameters and returns") {
    const LirPackage package = VariantLir(kShapes + R"(
        func MakeByte() -> Maybe<uint8> { return Maybe::Some<uint8>(7u8); }
        func MakeWide() -> Maybe<uint128> { return Maybe::Some<uint128>(9u128); }
        func PassByte(value: Maybe<uint8>) -> Maybe<uint8> { return value; }
        func PassWide(value: Maybe<uint128>) -> Maybe<uint128> { return value; }
    )");
    const LirFunc &passByte = LirFunction(package, "PassByte");
    const LirFunc &passWide = LirFunction(package, "PassWide");
    REQUIRE_EQ(passByte.params.size(), 1);
    REQUIRE_EQ(passWide.params.size(), 1);
    CHECK_EQ(passByte.params.front().type, passByte.returnType);
    CHECK_EQ(passWide.params.front().type, passWide.returnType);
    CHECK_LT(passByte.returnType.SizeInBytes().value_or(0), passWide.returnType.SizeInBytes().value_or(0));
    CHECK_EQ(Allocations(LirFunction(package, "MakeByte"), "Maybe<uint8>").size(), 1);
    CHECK_EQ(Allocations(LirFunction(package, "MakeWide"), "Maybe<uint128>").size(), 1);
}

TEST_CASE("variant by-value calls retain concrete ABI types without decoding") {
    const LirPackage package = VariantLir(kShapes + R"(
        func Identity(value: Parcel) -> Parcel { return value; }
        func Build() -> Parcel { return Parcel::Named { sequence: 99u64, enabled: true }; }
        func Main() -> int { let value = Identity(Build()); return 0; }
    )");
    const LirFunc &identity = LirFunction(package, "Identity");
    REQUIRE_EQ(identity.params.size(), 1);
    CHECK_EQ(identity.params.front().type, identity.returnType);
    CHECK_EQ(identity.returnType.name, "Parcel");
    CHECK_EQ(identity.returnType.SizeInBytes(), 24);
    const LirFunc &main = LirFunction(package, "Main");
    CHECK_GE(CountOpcode(main, LirOpcode::Call), 2);
    CHECK_GE(Allocations(main, "Parcel").size(), 1);
}

TEST_CASE("variant constructor and public function symbols do not encode source keyword spelling") {
    const LirPackage package = VariantLir(R"(
        pub variant Outcome { Ready, Value(int32) }
        pub func Build() -> Outcome { return Outcome::Value(42i32); }
        pub func Pass(value: Outcome) -> Outcome { return value; }
        func Main() -> int { let value = Pass(Build()); return 0; }
    )");
    const LirFunc &build = LirFunction(package, "Build");
    const LirFunc &pass = LirFunction(package, "Pass");
    CHECK(build.isPublic);
    CHECK(pass.isPublic);
    CHECK_EQ(build.returnType.name, "Outcome");
    CHECK_EQ(pass.params.front().type.name, "Outcome");
    CHECK_EQ(pass.returnType.name, "Outcome");
}

TEST_CASE("AArch64 lowering receives the same tagged construction metadata") {
    const LirPackage package = VariantLir(kShapes + R"(
        func Build() -> Parcel { return Parcel::Pair(20u32, 22u32); }
        func Pass(value: Parcel) -> Parcel { return value; }
        func Main() -> int { let value = Pass(Build()); return 0; }
    )",
                                          AArch64Target());
    const LirFunc &build = LirFunction(package, "Build");
    const LirFunc &pass = LirFunction(package, "Pass");
    CHECK_GE(CountOpcode(build, LirOpcode::IndexPtr), 2);
    REQUIRE_EQ(pass.params.size(), 1);
    CHECK_EQ(pass.params.front().type, pass.returnType);
    CHECK_EQ(pass.returnType.SizeInBytes(), 24);
}

TEST_CASE("x86-64 lowering receives compact aligned and multiword variant metadata") {
    const LirPackage package = VariantLir(kShapes + R"(
        func Byte() -> Parcel { return Parcel::Byte(7u8); }
        func Named() -> Parcel { return Parcel::Named { sequence: 42u64, enabled: true }; }
        func Pass(value: Parcel) -> Parcel { return value; }
    )",
                                          X86Target());
    const LirFunc &byte = LirFunction(package, "Byte");
    const LirFunc &named = LirFunction(package, "Named");
    const LirFunc &pass = LirFunction(package, "Pass");
    CHECK_EQ(byte.returnType.SizeInBytes(), 24);
    CHECK_EQ(named.returnType.SizeInBytes(), 24);
    CHECK_GE(CountOpcode(byte, LirOpcode::IndexPtr), 1);
    CHECK_GE(CountOpcode(named, LirOpcode::IndexPtr), 2);
    REQUIRE_EQ(pass.params.size(), 1);
    CHECK_EQ(pass.params.front().type, pass.returnType);
    CHECK_EQ(pass.returnType.SizeInBytes(), 24);
}

TEST_CASE("HIR declarations and constructions agree on their case-bearing form") {
    const HirPackage package = VariantHir(R"(
        enum Code: uint8 { Ready = 1, Busy = 2 }
        variant Outcome { Ready, Busy(int32) }
        func CodeValue() -> Code { return Code::Ready; }
        func OutcomeValue() -> Outcome { return Outcome::Ready; }
    )");
    REQUIRE_EQ(package.modules.size(), 1);
    REQUIRE_EQ(package.modules.front().enums.size(), 2);
    const auto code =
        std::ranges::find_if(package.modules.front().enums, [](const HirEnum &type) { return type.name == "Code"; });
    const auto outcome =
        std::ranges::find_if(package.modules.front().enums, [](const HirEnum &type) { return type.name == "Outcome"; });
    REQUIRE(code != package.modules.front().enums.end());
    REQUIRE(outcome != package.modules.front().enums.end());
    CHECK_EQ(code->form, CaseTypeForm::Enumeration);
    CHECK_EQ(outcome->form, CaseTypeForm::Variant);
    const HirFunc &function = HirFunction(package, "OutcomeValue");
    const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
    REQUIRE(returned != nullptr);
    const auto *construction = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
    REQUIRE(construction != nullptr);
    CHECK_EQ(construction->form, outcome->form);
}

TEST_CASE("nested variant construction preserves both concrete storage layouts") {
    const LirPackage package = VariantLir(R"(
        variant Inner { Empty, Number(int64) }
        variant Outer { Empty, Nested(Inner), Pair(Inner, uint64) }
        func BuildInner() -> Inner { return Inner::Number(42i64); }
        func BuildNested() -> Outer { return Outer::Nested(BuildInner()); }
        func BuildPair() -> Outer { return Outer::Pair(BuildInner(), 7u64); }
        func Main() -> int { let nested = BuildNested(); let pair = BuildPair(); return 0; }
    )");
    const LirFunc &inner = LirFunction(package, "BuildInner");
    const LirFunc &nested = LirFunction(package, "BuildNested");
    const LirFunc &pair = LirFunction(package, "BuildPair");
    CHECK_EQ(inner.returnType.name, "Inner");
    CHECK_EQ(nested.returnType.name, "Outer");
    CHECK_EQ(pair.returnType.name, "Outer");
    CHECK_GT(nested.returnType.SizeInBytes().value_or(0), inner.returnType.SizeInBytes().value_or(0));
    CHECK_GE(CountOpcode(nested, LirOpcode::Call), 1);
    CHECK_GE(CountOpcode(pair, LirOpcode::Call), 1);
    CHECK_GE(CountOpcode(pair, LirOpcode::IndexPtr), 2);
}

TEST_CASE("payload-producing calls are evaluated once during construction") {
    const LirPackage package = VariantLir(R"(
        variant Pair { Values(int32, int32) }
        func First() -> int32 { return 20i32; }
        func Second() -> int32 { return 22i32; }
        func Build() -> Pair { return Pair::Values(First(), Second()); }
    )");
    const LirFunc &build = LirFunction(package, "Build");
    std::size_t firstCalls = 0;
    std::size_t secondCalls = 0;
    for (const LirBlock &block : build.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Call && instruction.strArg == "First") {
                ++firstCalls;
            }
            if (instruction.op == LirOpcode::Call && instruction.strArg == "Second") {
                ++secondCalls;
            }
        }
    }
    CHECK_EQ(firstCalls, 1);
    CHECK_EQ(secondCalls, 1);
    CHECK_GE(CountOpcode(build, LirOpcode::Store), 3);
    CHECK_GE(CountOpcode(build, LirOpcode::IndexPtr), 2);
}

TEST_CASE("case discriminants remain declaration-ordered across construction sites") {
    const HirPackage package = VariantHir(R"(
        variant Event { Started, Progress(uint32), Finished }
        func Start() -> Event { return Event::Started; }
        func Mid() -> Event { return Event::Progress(50u32); }
        func Finish() -> Event { return Event::Finished; }
    )");
    const auto discriminant = [&](const std::string &name) {
        const HirFunc &function = HirFunction(package, name);
        const auto *returned = dynamic_cast<const HirReturnStmt *>(function.body->stmts.front().get());
        REQUIRE(returned != nullptr);
        const auto *construction = dynamic_cast<const HirEnumConstructExpr *>(returned->value->get());
        REQUIRE(construction != nullptr);
        CHECK_EQ(construction->form, CaseTypeForm::Variant);
        return construction->discriminant;
    };
    CHECK_EQ(discriminant("Start"), "0");
    CHECK_EQ(discriminant("Mid"), "1");
    CHECK_EQ(discriminant("Finish"), "2");
}

TEST_CASE("HIR variant patterns retain form discriminants and declaration payload types") {
    const HirPackage package = VariantHir(R"(
        variant Packet {
            Empty,
            Pair(uint8, uint64),
            Named { code: uint32; enabled: bool8; sequence: uint64; }
        }
        func Tuple(value: Packet) -> uint64 {
            return match value { .Empty => 0u64, .Pair(first, second) => (first as uint64) + second,
                                 .Named { code, enabled, sequence } => (code as uint64) + sequence };
        }
        func Named(value: Packet) -> uint64 {
            return match value { .Empty => 0u64, .Pair(_, _) => 1u64,
                                 .Named { sequence, code, enabled } => sequence + (code as uint64) };
        }
    )");

    for (const std::string functionName : {"Tuple", "Named"}) {
        const HirMatchExpr &match = ReturnedMatch(package, functionName);
        REQUIRE_EQ(match.arms.size(), 3);
        for (const HirMatchArm &arm : match.arms) {
            const auto *pattern = dynamic_cast<const HirEnumPattern *>(arm.pattern.get());
            REQUIRE(pattern != nullptr);
            CHECK_EQ(pattern->form, CaseTypeForm::Variant);
            REQUIRE(pattern->discriminant.has_value());
        }
        const auto *named = dynamic_cast<const HirEnumPattern *>(match.arms.back().pattern.get());
        REQUIRE(named != nullptr);
        REQUIRE_EQ(named->payloadTypes.size(), 3);
        CHECK_EQ(named->payloadTypes[0].kind, TypeRef::Kind::UInt32);
        CHECK_EQ(named->payloadTypes[1].kind, TypeRef::Kind::Bool8);
        CHECK_EQ(named->payloadTypes[2].kind, TypeRef::Kind::UInt64);
        CHECK_EQ(named->argIndices, std::vector<std::size_t>{0, 1, 2});
    }
}

TEST_CASE("variant payload reads are emitted only in tag-gated blocks") {
    const LirPackage package = VariantLir(R"(
        variant Packet { Empty, Pair(uint32, uint64), Named { code: uint32; sequence: uint64; } }
        func Decode(value: Packet) -> uint64 {
            return match value { .Empty => 0u64, .Pair(first, second) => (first as uint64) + second,
                                 .Named { sequence, code } => sequence + (code as uint64) };
        }
    )");
    const LirFunc &decode = LirFunction(package, "Decode");
    std::size_t payloadBlocks = 0;
    std::size_t mismatchBlocks = 0;
    for (const LirBlock &block : decode.blocks) {
        if (block.label.starts_with("variant.pattern.payload")) {
            ++payloadBlocks;
            CHECK(std::ranges::any_of(block.instrs, [](const LirInstr &instruction) {
                return instruction.op == LirOpcode::IndexPtr || instruction.op == LirOpcode::Load;
            }));
        }
        if (block.label.starts_with("variant.pattern.mismatch")) {
            ++mismatchBlocks;
            CHECK_FALSE(std::ranges::any_of(block.instrs, [](const LirInstr &instruction) {
                return instruction.op == LirOpcode::IndexPtr || instruction.op == LirOpcode::Load;
            }));
        }
    }
    CHECK_EQ(payloadBlocks, 2);
    CHECK_EQ(mismatchBlocks, 2);
    CHECK_GE(CountOpcode(decode, LirOpcode::Phi), 2);
}

TEST_CASE("all-unit variant matching reads tags without creating payload paths") {
    const LirPackage package = VariantLir(R"(
        variant State { Idle, Ready, Complete }
        func Decode(value: State) -> int {
            return match value { .Idle => 10, .Ready => 20, .Complete => 30 };
        }
    )");
    const LirFunc &decode = LirFunction(package, "Decode");
    CHECK_GE(CountOpcode(decode, LirOpcode::CmpEq), 3);
    CHECK_FALSE(std::ranges::any_of(
        decode.blocks, [](const LirBlock &block) { return block.label.starts_with("variant.pattern.payload"); }));
    CHECK_EQ(CountOpcode(decode, LirOpcode::Shr), 0);
}

TEST_CASE("guard evaluation is dominated by a successful variant pattern") {
    const LirPackage package = VariantLir(R"(
        variant Maybe { None, Some(int64) }
        func Decode(value: Maybe) -> int64 {
            return match value { .Some(item) if item > 10i64 => item, .Some(_) => 0i64, .None => -1i64 };
        }
    )");
    const LirFunc &decode = LirFunction(package, "Decode");
    CHECK(std::ranges::any_of(decode.blocks,
                              [](const LirBlock &block) { return block.label.starts_with("pattern.guard"); }));
    CHECK_GE(CountOpcode(decode, LirOpcode::CmpGt), 1);
    CHECK_GE(CountOpcode(decode, LirOpcode::Phi), 2);
}

TEST_CASE("generic returned variants decode using substituted payload widths") {
    const LirPackage package = VariantLir(R"(
        variant Maybe<T> { None, Some(T) }
        func Byte() -> Maybe<uint8> { return Maybe::Some<uint8>(7u8); }
        func Wide() -> Maybe<uint128> { return Maybe::Some<uint128>(9u128); }
        func ReadByte() -> int { return match Byte() { .None => 0, .Some(value) => value as int }; }
        func ReadWide() -> int { return match Wide() { .None => 0, .Some(value) => value as int }; }
    )");
    const LirFunc &byte = LirFunction(package, "ReadByte");
    const LirFunc &wide = LirFunction(package, "ReadWide");
    CHECK_GE(CountOpcode(byte, LirOpcode::Call), 1);
    CHECK_GE(CountOpcode(wide, LirOpcode::Call), 1);
    CHECK_GE(CountOpcode(byte, LirOpcode::IndexPtr), 1);
    CHECK_GE(CountOpcode(wide, LirOpcode::IndexPtr), 1);
    CHECK_EQ(CountOpcode(byte, LirOpcode::Shr), 0);
    CHECK_EQ(CountOpcode(wide, LirOpcode::Shr), 0);
}

TEST_CASE("borrowed variant subjects use the same tag-gated payload offsets") {
    const LirPackage package = VariantLir(R"(
        variant Packet { Empty, Pair(int32, int64) }
        func Decode(value: *Packet) -> int64 {
            return match *value { .Empty => -1i64, .Pair(first, second) => (first as int64) + second };
        }
    )");
    const LirFunc &decode = LirFunction(package, "Decode");
    CHECK_GE(CountOpcode(decode, LirOpcode::CmpEq), 2);
    CHECK_GE(CountOpcode(decode, LirOpcode::IndexPtr), 2);
    CHECK_EQ(CountOpcode(decode, LirOpcode::Shr), 0);
    CHECK(std::ranges::any_of(
        decode.blocks, [](const LirBlock &block) { return block.label.starts_with("variant.pattern.payload"); }));
}
