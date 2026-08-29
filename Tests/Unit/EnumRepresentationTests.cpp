// One representation per variant: the shape a case is built in has to be the shape a match decodes, wherever the value
// came from. A generic variant wider than a word is an aggregate -- a tag at offset 0 and payloads after it -- and a
// value returned from a method is no different from one built in the same function.

#include "Lexer/Lexer.h"
#include "Lowering/AstToHir/AstToHir.h"
#include "Lowering/HirToLir/HirToLir.h"
#include "Semantic/SemanticAnalyzer.h"
#include "Syntax/Parser/Parser.h"

#include <algorithm>
#include <doctest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Rux;

namespace {
LirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "enums.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "enums.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    REQUIRE_FALSE(model.HasErrors());
    HirToLirLowering lowering(AstToHirLowering(model).Generate(), TargetContext::CreateNative());
    LirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
    REQUIRE_EQ(package.modules.size(), 1);
    for (const LirFunc &function : package.modules.front().funcs) {
        if (function.name == name) {
            return function;
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

std::vector<LirOpcode> OpcodesOf(const LirFunc &function) {
    std::vector<LirOpcode> opcodes;
    for (const LirBlock &block : function.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            opcodes.push_back(instruction.op);
        }
    }
    return opcodes;
}

bool Contains(const std::vector<LirOpcode> &opcodes, const LirOpcode op) {
    return std::find(opcodes.begin(), opcodes.end(), op) != opcodes.end();
}

/// A payload reached by byte offset from the variant's own storage is the aggregate representation; one reached by
/// shifting the tag out of a single word is the compact one.
bool ReadsPayloadByOffset(const LirFunc &function) {
    return Contains(OpcodesOf(function), LirOpcode::IndexPtr);
}

bool ReadsPayloadByShift(const LirFunc &function) {
    return Contains(OpcodesOf(function), LirOpcode::Shr);
}

std::unordered_map<std::string, ResolvedTypeLayout> AnalyzeLayouts(const std::string &source,
                                                                   const std::vector<std::string> &types) {
    Lexer lexer(source, "layouts.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "layouts.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const SemanticDiagnostic &diagnostic : model.diagnostics) {
        INFO(diagnostic.message);
    }
    REQUIRE_FALSE(model.HasErrors());

    std::unordered_map<std::string, ResolvedTypeLayout> layouts;
    for (const std::string &name : types) {
        const ResolvedTypeLayout *layout = model.TryGetLayout(TypeRef::MakeNamed(name));
        REQUIRE_MESSAGE(layout != nullptr, name);
        layouts.emplace(name, *layout);
    }
    return layouts;
}

const std::string kOptionPrelude = R"(
    variant Option<T> { Some(T), None }
    struct Counter { value: int32; limit: int32; }
    extend Counter {
        func Next(self: &var Counter) -> Option<int32> {
            if self.value >= self.limit { return Option::None<int32>(); }
            let current = self.value;
            self.value = self.value + 1i32;
            return Option::Some<int32>(current);
        }
    }
)";
} // namespace

TEST_CASE("a returned variant is matched in the representation it was built in") {
    const LirPackage package = LowerSource(kOptionPrelude + R"(
        func Main() -> int {
            var it = Counter { value: 0i32, limit: 3i32 };
            let a = match it.Next() { .Some(v) => v as int, .None => 90 };
            let b = match it.Next() { .Some(v) => v as int, .None => 90 };
            return a * 10 + b;
        }
    )");

    // The method builds the aggregate: every payload it writes is placed at an offset from the enum's storage.
    const LirFunc &next = RequireFunction(package, "Counter::Next");
    CHECK(ReadsPayloadByOffset(next));
    CHECK_FALSE(ReadsPayloadByShift(next));

    // Every match of what it returned decodes that same shape, however many times the method is called. Reading the
    // payload out of the upper half of a word instead decoded a value the method never wrote there, and the call
    // itself was made under an ABI too narrow to carry the returned aggregate.
    const LirFunc &main = RequireFunction(package, "Main");
    CHECK(ReadsPayloadByOffset(main));
    CHECK_FALSE(ReadsPayloadByShift(main));
}

TEST_CASE("a method's returned variant type carries the same layout as one built in place") {
    const LirPackage package = LowerSource(kOptionPrelude + R"(
        func Main() -> int {
            var it = Counter { value: 0i32, limit: 1i32 };
            let built = Option::Some<int32>(1i32);
            let returned = it.Next();
            return match built { .Some(v) => v as int, .None => 0 } +
                   match returned { .Some(v) => v as int, .None => 0 };
        }
    )");

    const LirFunc &main = RequireFunction(package, "Main");
    std::vector<TypeRef> optionSlots;
    for (const LirBlock &block : main.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca && instruction.type.name == "Option<int32>") {
                optionSlots.push_back(instruction.type);
            }
        }
    }

    // A locally built value and a returned one are the same type, so they have to agree on their size: the returned
    // one used to arrive as a bare name with no layout at all, which read as a compact variant.
    REQUIRE_GE(optionSlots.size(), 2);
    for (const TypeRef &slot : optionSlots) {
        CHECK_EQ(slot.SizeInBytes(), optionSlots.front().SizeInBytes());
        CHECK_GT(slot.SizeInBytes().value_or(0), 8);
    }
}

TEST_CASE("scalar enum layout is exactly its declared integer base") {
    const auto layouts = AnalyzeLayouts(R"(
        enum Byte: uint8 { Zero = 0, One = 1 }
        enum Signed: int16 { Below = -1, Equal = 0, Above = 1 }
        enum Default { First, Second }
    )",
                                        {"Byte", "Signed", "Default"});

    CHECK_EQ(layouts.at("Byte").size, 1);
    CHECK_EQ(layouts.at("Byte").alignment, 1);
    CHECK_EQ(layouts.at("Signed").size, 2);
    CHECK_EQ(layouts.at("Signed").alignment, 2);
    CHECK_EQ(layouts.at("Default").size, 8);
    CHECK_EQ(layouts.at("Default").alignment, 8);
}

TEST_CASE("all-unit variants remain tag-sized but retain variant representation") {
    const auto layouts = AnalyzeLayouts(R"(
        variant State { Idle, Ready, Complete }
        enum StateCode: int64 { Idle = 0, Ready = 1, Complete = 2 }
    )",
                                        {"State", "StateCode"});
    CHECK_EQ(layouts.at("State").size, layouts.at("StateCode").size);
    CHECK_EQ(layouts.at("State").alignment, layouts.at("StateCode").alignment);

    const LirPackage package = LowerSource(R"(
        variant State { Idle, Ready, Complete }
        func Main() -> int {
            let state = State::Ready;
            return match state { .Idle => 1, .Ready => 2, .Complete => 3 };
        }
    )");
    const LirFunc &main = RequireFunction(package, "Main");
    CHECK(std::ranges::any_of(main.blocks, [](const LirBlock &block) {
        return std::ranges::any_of(block.instrs, [](const LirInstr &instruction) {
            return instruction.op == LirOpcode::Alloca && instruction.type.name == "State" &&
                   instruction.type.inner.size() == 1 && instruction.type.inner.front().kind == TypeRef::Kind::Array;
        });
    }));
}

TEST_CASE("variant layout selects the widest payload and strongest alignment") {
    const auto layouts = AnalyzeLayouts(R"(
        struct Aligned { first: uint64; second: uint64; }
        variant Mixed {
            Empty,
            Byte(uint8),
            Pair(uint32, uint32),
            Named { value: Aligned; flag: bool8; }
        }
    )",
                                        {"Aligned", "Mixed"});

    CHECK_EQ(layouts.at("Aligned").size, 16);
    CHECK_EQ(layouts.at("Aligned").alignment, 8);
    CHECK_EQ(layouts.at("Mixed").alignment, 8);
    CHECK_GE(layouts.at("Mixed").size, 32);
    CHECK_EQ(layouts.at("Mixed").size % layouts.at("Mixed").alignment, 0);
}

TEST_CASE("nested and generic variants receive concrete independent layouts") {
    const auto layouts = AnalyzeLayouts(R"(
        variant Inner { None, Number(int32) }
        variant Outer { Empty, Nested(Inner), Pair(Inner, uint64) }
        variant Maybe<T> { None, Some(T) }
        func UseByte(value: Maybe<uint8>) -> Maybe<uint8> { return value; }
        func UseWord(value: Maybe<uint128>) -> Maybe<uint128> { return value; }
        func UseInner(value: Maybe<Inner>) -> Maybe<Inner> { return value; }
    )",
                                        {"Inner", "Outer", "Maybe<uint8>", "Maybe<uint128>", "Maybe<Inner>"});

    CHECK_GT(layouts.at("Inner").size, 8);
    CHECK_GT(layouts.at("Outer").size, layouts.at("Inner").size);
    CHECK_EQ(layouts.at("Maybe<uint8>").alignment, 8);
    CHECK_EQ(layouts.at("Maybe<uint128>").alignment, 8);
    CHECK_GT(layouts.at("Maybe<uint128>").size, layouts.at("Maybe<uint8>").size);
    CHECK_EQ(layouts.at("Maybe<Inner>").size, layouts.at("Maybe<uint128>").size);
}

TEST_CASE("zero-sized payloads do not erase the private variant tag") {
    const auto layouts = AnalyzeLayouts(R"(
        struct Marker {}
        variant Tagged { Empty, Marked(Marker) }
    )",
                                        {"Marker", "Tagged"});
    CHECK_EQ(layouts.at("Marker").size, 0);
    CHECK_EQ(layouts.at("Marker").alignment, 1);
    CHECK_GE(layouts.at("Tagged").size, 8);
    CHECK_EQ(layouts.at("Tagged").alignment, 8);
}

TEST_CASE("first-party option result and parse-error shapes keep their established layouts") {
    const auto layouts = AnalyzeLayouts(R"(
        variant Option<T> { Some(T), None }
        variant Result<T, E> { Success(T), Error(E) }
        variant ParseError { Empty, InvalidCharacter(uint), Overflow }
        func OptionValue(value: Option<int32>) -> Option<int32> { return value; }
        func ResultValue(value: Result<int32, uint64>) -> Result<int32, uint64> { return value; }
    )",
                                        {"Option<int32>", "Result<int32, uint64>", "ParseError"});

    CHECK_EQ(layouts.at("Option<int32>").size, 16);
    CHECK_EQ(layouts.at("Option<int32>").alignment, 8);
    CHECK_EQ(layouts.at("Result<int32, uint64>").size, 16);
    CHECK_EQ(layouts.at("Result<int32, uint64>").alignment, 8);
    CHECK_EQ(layouts.at("ParseError").size, 16);
    CHECK_EQ(layouts.at("ParseError").alignment, 8);
}

TEST_CASE("enum and variant type markers reflect declaration kind rather than case shape") {
    const LirPackage package = LowerSource(R"(
        enum Scalar: uint8 { First = 1, Second = 2 }
        variant Units { First, Second }
        variant Payload { Empty, Byte(uint8) }
        func Main() -> int {
            let scalar = Scalar::First;
            let units = Units::First;
            let payload = Payload::Byte(2u8);
            return (scalar as int) + match units { .First => 1, .Second => 2 } +
                   match payload { .Empty => 0, .Byte(value) => value as int };
        }
    )");
    const LirFunc &main = RequireFunction(package, "Main");
    std::unordered_map<std::string, TypeRef> allocations;
    for (const LirBlock &block : main.blocks) {
        for (const LirInstr &instruction : block.instrs) {
            if (instruction.op == LirOpcode::Alloca &&
                (instruction.type.name == "Scalar" || instruction.type.name == "Units" ||
                 instruction.type.name == "Payload")) {
                allocations.insert_or_assign(instruction.type.name, instruction.type);
            }
        }
    }
    REQUIRE(allocations.contains("Scalar"));
    REQUIRE(allocations.contains("Units"));
    REQUIRE(allocations.contains("Payload"));
    REQUIRE_EQ(allocations.at("Scalar").inner.size(), 1);
    CHECK_EQ(allocations.at("Scalar").inner.front().kind, TypeRef::Kind::UInt8);
    for (const std::string name : {"Units", "Payload"}) {
        REQUIRE_EQ(allocations.at(name).inner.size(), 1);
        CHECK_EQ(allocations.at(name).inner.front().kind, TypeRef::Kind::Array);
        CHECK_EQ(allocations.at(name).inner.front().inner.front().kind, TypeRef::Kind::Char8);
    }
}

TEST_CASE("generic variant parameters and returns carry identical concrete markers") {
    const LirPackage package = LowerSource(R"(
        variant Maybe<T> { None, Some(T) }
        func Byte(value: Maybe<uint8>) -> Maybe<uint8> { return value; }
        func Word(value: Maybe<uint128>) -> Maybe<uint128> { return value; }
        func Main() -> int {
            let byte = Byte(Maybe::Some<uint8>(1u8));
            let word = Word(Maybe::Some<uint128>(2u128));
            return match byte { .None => 0, .Some(value) => value as int } +
                   match word { .None => 0, .Some(value) => value as int };
        }
    )");

    for (const std::string name : {"Byte", "Word"}) {
        const LirFunc &function = RequireFunction(package, name);
        REQUIRE_EQ(function.params.size(), 1);
        CHECK_EQ(function.params.front().type, function.returnType);
        REQUIRE_EQ(function.returnType.inner.size(), 1);
        CHECK_EQ(function.returnType.inner.front().kind, TypeRef::Kind::Array);
        CHECK_EQ(function.returnType.inner.front().inner.front().kind, TypeRef::Kind::Char8);
    }
    const LirFunc &byte = RequireFunction(package, "Byte");
    const LirFunc &word = RequireFunction(package, "Word");
    CHECK_LT(byte.returnType.SizeInBytes().value_or(0), word.returnType.SizeInBytes().value_or(0));
}

TEST_CASE("declaration spelling does not enter constructor or public function symbols") {
    const LirPackage package = LowerSource(R"(
        pub enum Code { Ready = 1, Busy = 2 }
        pub variant Outcome { Ready, Busy(int32) }
        pub func PassCode(value: Code) -> Code { return value; }
        pub func PassOutcome(value: Outcome) -> Outcome { return value; }
        func Main() -> int {
            let code = PassCode(Code::Ready);
            let outcome = PassOutcome(Outcome::Busy(2i32));
            return (code as int) + match outcome { .Ready => 0, .Busy(value) => value as int };
        }
    )");
    const LirFunc &passCode = RequireFunction(package, "PassCode");
    const LirFunc &passOutcome = RequireFunction(package, "PassOutcome");
    CHECK(passCode.isPublic);
    CHECK(passOutcome.isPublic);
    CHECK_EQ(passCode.name, "PassCode");
    CHECK_EQ(passOutcome.name, "PassOutcome");
    CHECK_EQ(passCode.params.front().type.name, "Code");
    CHECK_EQ(passOutcome.params.front().type.name, "Outcome");
    CHECK_EQ(passCode.returnType.name, "Code");
    CHECK_EQ(passOutcome.returnType.name, "Outcome");
}

TEST_CASE("several concrete generic layouts are stable across repeated analysis") {
    const std::string source = R"(
        variant Box<T> { Empty, Stored(T), Pair(T, T) }
        func Byte(value: Box<uint8>) -> Box<uint8> { return value; }
        func Half(value: Box<uint16>) -> Box<uint16> { return value; }
        func Word(value: Box<uint64>) -> Box<uint64> { return value; }
    )";
    const std::vector<std::string> names = {"Box<uint8>", "Box<uint16>", "Box<uint64>"};
    const auto first = AnalyzeLayouts(source, names);
    const auto second = AnalyzeLayouts(source, names);
    for (const std::string &name : names) {
        CHECK_EQ(first.at(name).size, second.at(name).size);
        CHECK_EQ(first.at(name).alignment, second.at(name).alignment);
        CHECK_EQ(first.at(name).size % first.at(name).alignment, 0);
    }
    CHECK_LE(first.at("Box<uint8>").size, first.at("Box<uint16>").size);
    CHECK_LT(first.at("Box<uint16>").size, first.at("Box<uint64>").size);
}

TEST_CASE("tuple and array payload layouts use their structural size and alignment") {
    const auto layouts = AnalyzeLayouts(R"(
        variant Tuples {
            Empty,
            Narrow((uint8, uint16)),
            Wide((uint64, uint8))
        }
        variant Arrays {
            Empty,
            Bytes(uint8[3]),
            Words(uint32[3])
        }
    )",
                                        {"Tuples", "Arrays"});
    CHECK_EQ(layouts.at("Tuples").alignment, 8);
    CHECK_EQ(layouts.at("Tuples").size, 24);
    CHECK_EQ(layouts.at("Arrays").alignment, 8);
    CHECK_EQ(layouts.at("Arrays").size, 24);

    const LirPackage package = LowerSource(R"(
        variant Arrays { Empty, Bytes(uint8[3]), Words(uint32[3]) }
        func Main() -> int {
            let value = Arrays::Words([1u32, 2u32, 3u32]);
            return match value { .Empty => 0, .Bytes(_) => 1, .Words(items) => items[2] as int };
        }
    )");
    CHECK(ReadsPayloadByOffset(RequireFunction(package, "Main")));
    CHECK_FALSE(ReadsPayloadByShift(RequireFunction(package, "Main")));
}

TEST_CASE("generic zero-sized and pointer payloads keep distinct valid layouts") {
    const auto layouts = AnalyzeLayouts(R"(
        struct Marker {}
        variant Maybe<T> { None, Some(T) }
        func MarkerValue(value: Maybe<Marker>) -> Maybe<Marker> { return value; }
        func PointerValue(value: Maybe<*Marker>) -> Maybe<*Marker> { return value; }
    )",
                                        {"Maybe<Marker>", "Maybe<*Marker>"});

    CHECK_EQ(layouts.at("Maybe<Marker>").alignment, 8);
    CHECK_EQ(layouts.at("Maybe<Marker>").size, 16);
    CHECK_EQ(layouts.at("Maybe<*Marker>").alignment, 8);
    CHECK_EQ(layouts.at("Maybe<*Marker>").size, 16);
    CHECK_EQ(layouts.at("Maybe<Marker>").size % layouts.at("Maybe<Marker>").alignment, 0);
    CHECK_EQ(layouts.at("Maybe<*Marker>").size % layouts.at("Maybe<*Marker>").alignment, 0);
}

TEST_CASE("a nested variant is aligned once inside a named payload") {
    const auto layouts = AnalyzeLayouts(R"(
        variant Inner { Empty, Byte(uint8), Word(uint64) }
        struct Envelope { prefix: uint8; value: Inner; suffix: uint16; }
        variant Message { None, Wrapped { sequence: uint32; envelope: Envelope; } }
    )",
                                        {"Inner", "Envelope", "Message"});

    CHECK_EQ(layouts.at("Inner").size, 16);
    CHECK_EQ(layouts.at("Inner").alignment, 8);
    CHECK_EQ(layouts.at("Envelope").size, 32);
    CHECK_EQ(layouts.at("Envelope").alignment, 8);
    CHECK_EQ(layouts.at("Message").size, 48);
    CHECK_EQ(layouts.at("Message").alignment, 8);
}

TEST_CASE("wide scalar enum matches preserve every declared base-type bit") {
    const LirPackage package = LowerSource(R"(
        enum Signal: uint64 { Low = 1, High = 0x100000001 }
        func Decode(value: Signal) -> int {
            return match value { .Low => 1, .High => 2 };
        }
    )");
    const LirFunc &decode = RequireFunction(package, "Decode");
    const std::vector<LirOpcode> opcodes = OpcodesOf(decode);
    CHECK_GE(std::ranges::count(opcodes, LirOpcode::CmpEq), 2);
    CHECK_FALSE(Contains(opcodes, LirOpcode::And));
    CHECK_FALSE(Contains(opcodes, LirOpcode::Shr));
}
