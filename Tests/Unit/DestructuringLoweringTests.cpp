// What tuple and structure patterns lower to: each part carries the type it has inside the subject, an omitted field
// is spelled as a wildcard, refutable parts branch out before any binding is set, and a part a moving destructure
// leaves unbound is destroyed where it is discarded.

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
/// The HIR of `source`, which must analyze without errors.
HirPackage LowerSource(const std::string &source) {
    Lexer lexer(source, "destructuring.rux");
    auto lexed = lexer.Tokenize();
    REQUIRE_FALSE(lexed.HasErrors());
    Parser parser(std::move(lexed.tokens), "destructuring.rux");
    auto parsed = parser.Parse();
    REQUIRE_FALSE(parsed.HasErrors());
    SemanticAnalyzer analyzer({&parsed.module}, {}, "test", "Windows");
    const SemanticModel model = analyzer.Analyze();
    for (const auto &diagnostic : model.diagnostics) {
        if (diagnostic.severity == SemanticDiagnostic::Severity::Error) {
            FAIL_CHECK(diagnostic.message);
        }
    }
    AstToHirLowering lowering(model);
    HirPackage package = lowering.Generate();
    REQUIRE(lowering.Diagnostics().empty());
    return package;
}

/// The LIR of `package`, after checking that both back ends accept it.
LirPackage LowerToLir(HirPackage package) {
    HirToLirLowering lowering(std::move(package), TargetContext::CreateNative());
    LirPackage lir = lowering.Generate();
    CHECK(lowering.Diagnostics().empty());
    const RcuEmitter x86(lir, "test", Target::OS::Windows);
    static_cast<void>(x86.Generate());
    CHECK(x86.Diagnostics().empty());
    const AArch64RcuEmitter aarch64(lir, "test", Target::OS::Linux);
    static_cast<void>(aarch64.Generate());
    CHECK(aarch64.Diagnostics().empty());
    return lir;
}

const HirFunc &RequireFunction(const HirPackage &package, const std::string &name) {
    for (const HirModule &module : package.modules) {
        for (const HirFunc &function : module.funcs) {
            if (function.name == name) {
                return function;
            }
        }
    }
    FAIL("missing lowered function " << name);
    throw std::runtime_error("missing lowered function");
}

const LirFunc &RequireFunction(const LirPackage &package, const std::string &name) {
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

/// The arms of the match a function returns.
const std::vector<HirMatchArm> &ReturnedMatchArms(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    for (const auto &statement : function.body->stmts) {
        if (const auto *returned = dynamic_cast<const HirReturnStmt *>(statement.get()); returned && returned->value) {
            const auto *match = dynamic_cast<const HirMatchExpr *>(returned->value->get());
            REQUIRE(match != nullptr);
            return match->arms;
        }
    }
    FAIL("no returned match");
    throw std::runtime_error("no returned match");
}

/// The pattern of the first destructuring `let` in a function.
const HirPattern &LetPattern(const HirFunc &function) {
    REQUIRE(function.body.has_value());
    for (const auto &statement : function.body->stmts) {
        if (const auto *let = dynamic_cast<const HirLetStmt *>(statement.get()); let && let->pattern) {
            return *let->pattern;
        }
    }
    FAIL("no destructuring let");
    throw std::runtime_error("no destructuring let");
}

bool StartsWith(const std::string &text, const std::string &prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool CallsGlue(const LirBlock &block) {
    return std::ranges::any_of(block.instrs, [](const LirInstr &instruction) {
        return instruction.op == LirOpcode::Call && StartsWith(instruction.strArg, "__rux_drop__");
    });
}

constexpr const char *kTag = R"(
    struct Tag { id: int32; }
    extend Tag {
        func =(self: &var Tag, other: &Tag);
        func ~Tag(self: &var Tag) {}
    }
)";
} // namespace

TEST_CASE("structure pattern fields carry their substituted types and omitted fields become wildcards") {
    const std::string source = R"(
        struct Pair<T> { first: T; second: T; tag: uint8; }
        func Second(pair: Pair<int64>) -> int64 {
            return match pair {
                Pair { second: 7, tag: 1 } => 0i64,
                Pair { second: value } => value
            };
        }
    )";
    const HirPackage package = LowerSource(source);
    const auto &arms = ReturnedMatchArms(RequireFunction(package, "Second"));
    REQUIRE_EQ(arms.size(), 2);
    const auto *literal = dynamic_cast<const HirStructPattern *>(arms[0].pattern.get());
    REQUIRE(literal != nullptr);
    REQUIRE_EQ(literal->fields.size(), 3);
    CHECK_EQ(literal->fields[0].name, "second");
    CHECK_EQ(literal->fields[0].type, TypeRef::MakeInt64());
    // A literal is compared at the width of the field it stands in, not at its own default type.
    const auto *seven = dynamic_cast<const HirLiteralPattern *>(literal->fields[0].pattern.get());
    REQUIRE(seven != nullptr);
    CHECK_EQ(seven->type, TypeRef::MakeInt64());
    const auto *one = dynamic_cast<const HirLiteralPattern *>(literal->fields[1].pattern.get());
    REQUIRE(one != nullptr);
    CHECK_EQ(one->type, TypeRef::MakeUInt8());
    CHECK_EQ(literal->fields[2].name, "first");
    CHECK_EQ(literal->fields[2].type, TypeRef::MakeInt64());
    CHECK(dynamic_cast<const HirWildcardPattern *>(literal->fields[2].pattern.get()) != nullptr);

    const auto *bound = dynamic_cast<const HirStructPattern *>(arms[1].pattern.get());
    REQUIRE(bound != nullptr);
    const auto *value = dynamic_cast<const HirBindingPattern *>(bound->fields[0].pattern.get());
    REQUIRE(value != nullptr);
    CHECK_EQ(value->type, TypeRef::MakeInt64());
    static_cast<void>(LowerToLir(LowerSource(source)));
}

TEST_CASE("a refutable tuple element branches out before the bindings are set") {
    const LirPackage lir = LowerToLir(LowerSource(R"(
        func Second(pair: (int32, int32)) -> int32 {
            return match pair {
                (0, value) => value,
                else => -1i32
            };
        }
    )"));
    const LirFunc &function = RequireFunction(lir, "Second");
    const auto mismatch = std::ranges::find_if(
        function.blocks, [](const LirBlock &block) { return StartsWith(block.label, "destructure.mismatch"); });
    REQUIRE(mismatch != function.blocks.end());
    const auto mismatchIndex = static_cast<std::uint32_t>(mismatch - function.blocks.begin());

    // The block testing the literal leaves for the mismatch block and stores nothing: the binding is written only on
    // the path where every part matched.
    const auto test = std::ranges::find_if(function.blocks, [&](const LirBlock &block) {
        return block.term && block.term->kind == LirTermKind::Branch && block.term->falseTarget == mismatchIndex;
    });
    REQUIRE(test != function.blocks.end());
    CHECK(std::ranges::any_of(test->instrs,
                              [](const LirInstr &instruction) { return instruction.op == LirOpcode::CmpEq; }));
    const LirBlock &matched = function.blocks[test->term->trueTarget];
    CHECK(std::ranges::any_of(matched.instrs,
                              [](const LirInstr &instruction) { return instruction.op == LirOpcode::Store; }));
}

TEST_CASE("a destructuring let records the drop glue of each droppable part it discards") {
    const std::string source = std::string(kTag) + R"(
        func Keep(pair: (Tag, int32, Tag)) -> int32 {
            let (_, _, kept) <- pair;
            return kept.id;
        }
    )";
    const HirPackage package = LowerSource(source);
    const auto *tuple = dynamic_cast<const HirTuplePattern *>(&LetPattern(RequireFunction(package, "Keep")));
    REQUIRE(tuple != nullptr);
    REQUIRE_EQ(tuple->elements.size(), 3);
    const auto *droppable = dynamic_cast<const HirWildcardPattern *>(tuple->elements[0].get());
    REQUIRE(droppable != nullptr);
    CHECK(StartsWith(droppable->discardGlue, "__rux_drop__"));
    const auto *scalar = dynamic_cast<const HirWildcardPattern *>(tuple->elements[1].get());
    REQUIRE(scalar != nullptr);
    CHECK(scalar->discardGlue.empty());

    const LirPackage lir = LowerToLir(LowerSource(source));
    CHECK(CallsGlue(RequireFunction(lir, "Keep").blocks.front()));
}

TEST_CASE("a structure pattern in let binds substituted fields and discards the ones it leaves unbound") {
    const std::string source = std::string(kTag) + R"(
        struct Parcel<T> { label: Tag; contents: T; count: int32; }
        func Keep(parcel: Parcel<Tag>) -> int32 {
            let Parcel { contents: kept, label: _ } <- parcel;
            return kept.id;
        }
        func Whole(tag: Tag) {
            let Tag { id: _ } <- tag;
        }
    )";
    const HirPackage package = LowerSource(source);
    const auto *structure = dynamic_cast<const HirStructPattern *>(&LetPattern(RequireFunction(package, "Keep")));
    REQUIRE(structure != nullptr);
    // The written fields come first, in source order, then the omitted ones as wildcards.
    REQUIRE_EQ(structure->fields.size(), 3);
    CHECK_EQ(structure->fields[0].name, "contents");
    const auto *kept = dynamic_cast<const HirBindingPattern *>(structure->fields[0].pattern.get());
    REQUIRE(kept != nullptr);
    // `contents: T` is read as `Tag`, the type the `label` field declares.
    CHECK_EQ(structure->fields[0].type, structure->fields[1].type);
    CHECK_EQ(kept->type, structure->fields[1].type);
    CHECK_EQ(structure->fields[1].name, "label");
    const auto *label = dynamic_cast<const HirWildcardPattern *>(structure->fields[1].pattern.get());
    REQUIRE(label != nullptr);
    CHECK(StartsWith(label->discardGlue, "__rux_drop__"));
    CHECK_EQ(structure->fields[2].name, "count");
    const auto *count = dynamic_cast<const HirWildcardPattern *>(structure->fields[2].pattern.get());
    REQUIRE(count != nullptr);
    CHECK(count->discardGlue.empty());

    // A pattern that binds nothing discards the value whole, so the destructor its type declares still runs.
    const auto *whole = dynamic_cast<const HirWildcardPattern *>(&LetPattern(RequireFunction(package, "Whole")));
    REQUIRE(whole != nullptr);
    CHECK(StartsWith(whole->discardGlue, "__rux_drop__"));

    const LirPackage lir = LowerToLir(LowerSource(source));
    CHECK(CallsGlue(RequireFunction(lir, "Keep").blocks.front()));
    CHECK(CallsGlue(RequireFunction(lir, "Whole").blocks.front()));
}

TEST_CASE("an arm over a consumed tuple destroys the elements it leaves unbound") {
    const LirPackage lir = LowerToLir(LowerSource(std::string(kTag) + R"(
        func Second(pair: (Tag, Tag)) -> int32 {
            return match <-pair { (_, second) => second.id };
        }
        func Borrowed(pair: &(Tag, Tag)) -> int32 {
            return match pair { (_, second) => second.id };
        }
    )"));
    const LirFunc &consumed = RequireFunction(lir, "Second");
    const auto arm = std::ranges::find_if(
        consumed.blocks, [](const LirBlock &block) { return StartsWith(block.label, "match.expr.store.arm0"); });
    REQUIRE(arm != consumed.blocks.end());
    CHECK(CallsGlue(*arm));

    // A borrowed subject keeps every element, so its arm destroys nothing.
    const LirFunc &borrowed = RequireFunction(lir, "Borrowed");
    CHECK_FALSE(std::ranges::any_of(borrowed.blocks, CallsGlue));
}

namespace {
constexpr const char *kCounted = R"(
    struct Counted { id: int32; }
    extend Counted {
        func =(self: &var Counted, other: &Counted) { self.id = other.id; }
        func ~Counted(self: &var Counted) {}
    }
)";
} // namespace

TEST_CASE("a by-value match of a named copyable value takes the copy it makes") {
    const std::string source = std::string(kCounted) + R"(
        func First(pair: (Counted, int32)) -> int32 {
            return match pair { (item, _) => item.id };
        }
        func Count(pair: (Counted, int32)) -> int32 {
            return match pair { (_, count) => count };
        }
        func Look(pair: (Counted, int32)) -> int32 {
            return match pair { (_, 2) => 1i32, else => 0i32 };
        }
    )";
    const HirPackage package = LowerSource(source);
    const auto subjectOf = [&](const std::string &name) -> const HirExpr & {
        const HirFunc &function = RequireFunction(package, name);
        for (const auto &statement : function.body->stmts) {
            if (const auto *returned = dynamic_cast<const HirReturnStmt *>(statement.get());
                returned && returned->value) {
                const auto *match = dynamic_cast<const HirMatchExpr *>(returned->value->get());
                REQUIRE(match != nullptr);
                return *match->subject;
            }
        }
        FAIL("no returned match");
        throw std::runtime_error("no returned match");
    };

    // The arms own the copy, so the binding is destroyed with its arm.
    const HirExpr &copied = subjectOf("First");
    CHECK(dynamic_cast<const HirCopyExpr *>(&copied) != nullptr);
    REQUIRE(copied.consumption.has_value());
    CHECK_EQ(*copied.consumption, ValueConsumptionKind::MatchSubject);
    const auto &arms = ReturnedMatchArms(RequireFunction(package, "First"));
    REQUIRE_EQ(arms.size(), 1);
    CHECK_EQ(arms.front().cleanups.size(), 1);

    // Nothing is bound from the subject, so it stays with its owner: no copy, nothing owned.
    const HirExpr &kept = subjectOf("Look");
    CHECK(dynamic_cast<const HirCopyExpr *>(&kept) == nullptr);
    CHECK_FALSE(kept.consumption.has_value());

    // The element an arm leaves is destroyed at the top of that arm.
    const LirPackage lir = LowerToLir(LowerSource(source));
    const LirFunc &count = RequireFunction(lir, "Count");
    const auto arm = std::ranges::find_if(
        count.blocks, [](const LirBlock &block) { return StartsWith(block.label, "match.expr.store.arm0"); });
    REQUIRE(arm != count.blocks.end());
    CHECK(CallsGlue(*arm));
}

TEST_CASE("a match destroys an owned subject that no arm takes") {
    const LirPackage lir = LowerToLir(LowerSource(std::string(kCounted) + R"(
        func Guarded(pair: (Counted, int32)) {
            match pair {
                (item, count) if count > 5 => {}
            }
        }
        func Moved(pair: (Counted, int32)) {
            match <-pair {
                (item, 7) => {}
            }
        }
        func Covered(pair: (Counted, int32)) {
            match pair {
                (item, count) => {}
            }
        }
    )"));
    for (const char *name : {"Guarded", "Moved"}) {
        const LirFunc &function = RequireFunction(lir, name);
        const auto untaken = std::ranges::find_if(
            function.blocks, [](const LirBlock &block) { return StartsWith(block.label, "match.untaken"); });
        REQUIRE_MESSAGE(untaken != function.blocks.end(), name);
        CHECK_MESSAGE(CallsGlue(*untaken), name);
    }
    // A last arm that takes every value leaves nothing untaken.
    const LirFunc &covered = RequireFunction(lir, "Covered");
    CHECK_FALSE(std::ranges::any_of(covered.blocks,
                                    [](const LirBlock &block) { return StartsWith(block.label, "match.untaken"); }));
}
