// The stack-trace printer every panic and failed assertion calls before it stops the program.
// It walks the chain of frame records that every compiled function's prologue builds — on both targets a record holds
// the caller's record and then the return address — and names each caller by looking its return address up in the
// tables of function ranges code generation emits for the program. The walk stops at the first return address no
// table covers, which is where the program's own code ends: the entry stub, the loader or a C caller. It is written
// once here as LIR, so it needs nothing from any package and reads the same on every target.
//
// The tables are `__rux_function_tables`, a count and then one address per module, each module's table being a count
// and then, per function, its start address, its length in bytes and its name as a pointer and a length.

#include "Lowering/HirToLir/CheckedLirBuilder.h"
#include "Lowering/HirToLir/HirToLirContext.h"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Rux::HirToLirDetail {
namespace {
// How many callers a trace names at most, so a damaged chain cannot loop forever.
constexpr std::uint64_t kMaximumFrames = 64;

class BacktraceBuilder {
public:
    explicit BacktraceBuilder(LirFunc &function)
        : builder(function) {
    }

    LirReg Emit(const LirOpcode op, const TypeRef &type, std::vector<LirReg> sources = {}, std::string argument = {}) {
        LirInstr instruction;
        instruction.dst =
            op == LirOpcode::Store || op == LirOpcode::WriteStandardError ? LirNoReg : builder.AllocateRegister();
        instruction.op = op;
        instruction.type = type;
        instruction.srcs = std::move(sources);
        instruction.strArg = std::move(argument);
        const LirReg dst = instruction.dst;
        if (!builder.Insert(std::move(instruction))) {
            Fail(builder.FailureReason());
        }
        return dst;
    }

    LirReg Word(const std::uint64_t value) {
        return Emit(LirOpcode::Const, TypeRef::MakeUInt64(), {}, std::to_string(value));
    }

    // The 64-bit word at `index` words past `base`.
    LirReg WordAt(const LirReg base, const LirReg index, const TypeRef &type) {
        const LirReg address = Emit(LirOpcode::IndexPtr, TypeRef::MakePointer(TypeRef::MakeUInt64()), {base, index});
        return Emit(LirOpcode::Load, type, {address});
    }

    LirReg Variable(const TypeRef &type) {
        return Emit(LirOpcode::Alloca, type);
    }

    void Set(const LirReg variable, const LirReg value, const TypeRef &type) {
        static_cast<void>(Emit(LirOpcode::Store, type, {value, variable}));
    }

    LirReg Get(const LirReg variable, const TypeRef &type) {
        return Emit(LirOpcode::Load, type, {variable});
    }

    CheckedLirBuilder::BlockIndex Block(std::string label) {
        return builder.CreateBlock(std::move(label));
    }

    void Select(const CheckedLirBuilder::BlockIndex block) {
        if (!builder.SelectBlock(block)) {
            Fail(builder.FailureReason());
        }
    }

    void Jump(const CheckedLirBuilder::BlockIndex target) {
        LirTerminator terminator;
        terminator.kind = LirTermKind::Jump;
        terminator.trueTarget = target;
        Terminate(std::move(terminator));
    }

    void Branch(const LirReg condition, const CheckedLirBuilder::BlockIndex whenTrue,
                const CheckedLirBuilder::BlockIndex whenFalse) {
        LirTerminator terminator;
        terminator.kind = LirTermKind::Branch;
        terminator.cond = condition;
        terminator.trueTarget = whenTrue;
        terminator.falseTarget = whenFalse;
        Terminate(std::move(terminator));
    }

    void Return() {
        LirTerminator terminator;
        terminator.kind = LirTermKind::Return;
        terminator.retType = TypeRef::MakeOpaque();
        Terminate(std::move(terminator));
    }

    void WriteText(const std::string &text) {
        const LirReg address = Emit(LirOpcode::StringAddr, TypeRef::MakePointer(TypeRef::MakeChar8()), {}, text);
        const LirReg length = Word(text.size());
        static_cast<void>(Emit(LirOpcode::WriteStandardError, TypeRef::MakeOpaque(), {address, length}));
    }

    [[nodiscard]] const std::string &Failure() const {
        return failure;
    }

private:
    void Terminate(LirTerminator terminator) {
        if (!builder.Terminate(std::move(terminator))) {
            Fail(builder.FailureReason());
        }
    }

    void Fail(const std::string_view reason) {
        if (failure.empty()) {
            failure = std::string(reason);
        }
    }

    CheckedLirBuilder builder;
    std::string failure;
};
} // namespace

LirFunc SynthesizeBacktrace(std::vector<Diagnostic> &diagnostics) {
    LirFunc function;
    function.name = std::string(kBacktraceFunctionName);
    function.displayName = function.name;
    function.isPublic = false;
    function.returnType = TypeRef::MakeOpaque();

    BacktraceBuilder b(function);
    const TypeRef word = TypeRef::MakeUInt64();
    const TypeRef wordPointer = TypeRef::MakePointer(TypeRef::MakeUInt64());
    const TypeRef textPointer = TypeRef::MakePointer(TypeRef::MakeChar8());

    const auto entry = b.Block("entry");
    const auto frameLoop = b.Block("frame");
    const auto frameBody = b.Block("frame_body");
    const auto tableLoop = b.Block("table");
    const auto tableBody = b.Block("table_body");
    const auto entryLoop = b.Block("entry_loop");
    const auto entryBody = b.Block("entry_body");
    const auto entryAbove = b.Block("entry_above");
    const auto found = b.Block("found");
    const auto entryNext = b.Block("entry_next");
    const auto tableNext = b.Block("table_next");
    const auto advance = b.Block("advance");
    const auto done = b.Block("done");

    // entry: start at the panicking function's frame, which is the caller of this one.
    b.Select(entry);
    const LirReg zero = b.Word(0);
    const LirReg one = b.Word(1);
    const LirReg own = b.Emit(LirOpcode::FramePointer, wordPointer);
    const LirReg frame = b.Variable(wordPointer);
    const LirReg frameWord = b.Variable(word);
    const LirReg depth = b.Variable(word);
    const LirReg tableIndex = b.Variable(word);
    const LirReg entryIndex = b.Variable(word);
    b.Set(frame, b.WordAt(own, zero, wordPointer), wordPointer);
    b.Set(frameWord, b.WordAt(own, zero, word), word);
    b.Set(depth, zero, word);
    b.Jump(frameLoop);

    // frame: stop at a null record or after the most frames a trace shows.
    b.Select(frameLoop);
    const LirReg currentWord = b.Get(frameWord, word);
    const LirReg isNull = b.Emit(LirOpcode::CmpEq, TypeRef::MakeBool(), {currentWord, zero});
    const LirReg tooDeep = b.Emit(LirOpcode::CmpGe, TypeRef::MakeBool(), {b.Get(depth, word), b.Word(kMaximumFrames)});
    const LirReg stop = b.Emit(LirOpcode::Or, TypeRef::MakeBool(), {isNull, tooDeep});
    b.Branch(stop, done, frameBody);

    // frame_body: look the return address up, one byte back so a call that ends its function still falls inside it.
    b.Select(frameBody);
    b.Set(tableIndex, zero, word);
    b.Jump(tableLoop);

    b.Select(tableLoop);
    const LirReg tables = b.Emit(LirOpcode::GlobalAddr, word, {}, "__rux_function_tables");
    const LirReg tableCount = b.WordAt(tables, zero, word);
    const LirReg currentTable = b.Get(tableIndex, word);
    const LirReg tablesLeft = b.Emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {currentTable, tableCount});
    b.Branch(tablesLeft, tableBody, done);

    b.Select(tableBody);
    b.Set(entryIndex, zero, word);
    b.Jump(entryLoop);

    b.Select(entryLoop);
    const LirReg tablesAgain = b.Emit(LirOpcode::GlobalAddr, word, {}, "__rux_function_tables");
    const LirReg tableSlot = b.Emit(LirOpcode::Add, word, {b.Get(tableIndex, word), one});
    const LirReg table = b.WordAt(tablesAgain, tableSlot, wordPointer);
    const LirReg entryCount = b.WordAt(table, zero, word);
    const LirReg currentEntry = b.Get(entryIndex, word);
    const LirReg entriesLeft = b.Emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {currentEntry, entryCount});
    b.Branch(entriesLeft, entryBody, tableNext);

    // entry_body: the entry at 1 + 4 * index holds start, length, name and name length.
    b.Select(entryBody);
    const LirReg entryBase =
        b.Emit(LirOpcode::Add, word, {b.Emit(LirOpcode::Mul, word, {b.Get(entryIndex, word), b.Word(4)}), one});
    const LirReg start = b.WordAt(table, entryBase, word);
    const LirReg frameAgain = b.Get(frame, wordPointer);
    const LirReg returnAddress = b.WordAt(frameAgain, one, word);
    const LirReg target = b.Emit(LirOpcode::Sub, word, {returnAddress, one});
    const LirReg atOrAfter = b.Emit(LirOpcode::CmpGe, TypeRef::MakeBool(), {target, start});
    b.Branch(atOrAfter, entryAbove, entryNext);

    b.Select(entryAbove);
    const LirReg lengthSlot = b.Emit(LirOpcode::Add, word, {entryBase, one});
    const LirReg end = b.Emit(LirOpcode::Add, word, {start, b.WordAt(table, lengthSlot, word)});
    const LirReg frameThird = b.Get(frame, wordPointer);
    const LirReg targetAgain = b.Emit(LirOpcode::Sub, word, {b.WordAt(frameThird, one, word), one});
    const LirReg inside = b.Emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {targetAgain, end});
    b.Branch(inside, found, entryNext);

    // found: print the caller, then follow its record while it moves up the stack.
    b.Select(found);
    const LirReg nameSlot = b.Emit(LirOpcode::Add, word, {entryBase, b.Word(2)});
    const LirReg nameLengthSlot = b.Emit(LirOpcode::Add, word, {entryBase, b.Word(3)});
    const LirReg name = b.WordAt(table, nameSlot, textPointer);
    const LirReg nameLength = b.WordAt(table, nameLengthSlot, word);
    b.WriteText("  from ");
    static_cast<void>(b.Emit(LirOpcode::WriteStandardError, TypeRef::MakeOpaque(), {name, nameLength}));
    b.WriteText("\n");
    b.Jump(advance);

    b.Select(entryNext);
    b.Set(entryIndex, b.Emit(LirOpcode::Add, word, {b.Get(entryIndex, word), one}), word);
    b.Jump(entryLoop);

    b.Select(tableNext);
    b.Set(tableIndex, b.Emit(LirOpcode::Add, word, {b.Get(tableIndex, word), one}), word);
    b.Jump(tableLoop);

    // advance: a record that does not move up the stack ends the walk rather than looping on it.
    b.Select(advance);
    const LirReg record = b.Get(frame, wordPointer);
    const LirReg nextWord = b.WordAt(record, zero, word);
    const LirReg nextFrame = b.WordAt(record, zero, wordPointer);
    const LirReg upward = b.Emit(LirOpcode::CmpGt, TypeRef::MakeBool(), {nextWord, b.Get(frameWord, word)});
    b.Set(frame, nextFrame, wordPointer);
    b.Set(frameWord, nextWord, word);
    b.Set(depth, b.Emit(LirOpcode::Add, word, {b.Get(depth, word), one}), word);
    b.Branch(upward, frameLoop, done);

    b.Select(done);
    b.Return();

    if (!b.Failure().empty()) {
        diagnostics.push_back(
            ErrorDiagnostic(std::format("internal compiler error: invalid internal LIR while synthesizing '{}': {}",
                                        function.name, b.Failure()),
                            {"the stack-trace printer was built from instructions the checked builder rejected"}));
    }
    return function;
}
} // namespace Rux::HirToLirDetail
