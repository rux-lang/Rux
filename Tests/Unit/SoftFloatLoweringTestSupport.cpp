#include "SoftFloatLoweringTestSupport.h"

#include "Numeric/FloatEncoding.h"
#include "Numeric/FloatFormat.h"
#include "Numeric/FloatParsing.h"
#include "Numeric/IntegerLiteral.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <array>
#include <doctest.h>
#include <format>
#include <utility>

namespace Rux::Testing {
namespace {
enum class ValueClass {
    Integer,
    Bool,
    Float,
    Pointer
};

/// How the interpreter holds one type's values: `width` value bits in `storage` bytes.
struct Shape {
    ValueClass valueClass = ValueClass::Integer;
    TypeRef::Kind kind = TypeRef::Kind::Unknown;
    std::uint32_t width = 0;
    std::uint32_t storage = 0;
    bool isSigned = false;
};

struct Value {
    WideInteger bits;
    Shape shape;
};

std::optional<Shape> ShapeOf(const TypeRef &type) {
    if (type->kind == TypeRef::Kind::Pointer || type->kind == TypeRef::Kind::Reference) {
        return Shape{ValueClass::Pointer, type->kind, 64, 8, false};
    }
    if (type.IsFloat()) {
        const PrimitiveInfo *primitive = FindPrimitive(type->kind);
        const FloatFormat *format = primitive ? FindFloatFormat(primitive->bits) : nullptr;
        return format ? std::optional{Shape{ValueClass::Float, type->kind, format->valueBits, primitive->size, false}}
                      : std::nullopt;
    }
    if (!type.IsBool() && !type.IsInteger() && !type.IsChar()) {
        return std::nullopt;
    }
    const auto size = type.SizeInBytes().value_or(0);
    if (size == 0 || size * 8 > WideInteger::MaxBits) {
        return std::nullopt;
    }
    const auto bytes = static_cast<std::uint32_t>(size);
    return Shape{type.IsBool() ? ValueClass::Bool : ValueClass::Integer, type->kind, bytes * 8, bytes, type.IsSigned()};
}

/// Addresses start well above zero, so a null or small integer used as a pointer is caught rather than read.
constexpr std::uint64_t MemoryBase = 0x10000;

/// A fault unwinds every interpreted frame at once, carrying what `LirRunResult` reports.
struct Trap {
    LirFault fault;
    std::string message;
};

class Machine {
public:
    Machine(const std::unordered_map<std::string_view, const LirFunc *> &functions,
            const LirInterpreterOptions &options)
        : functions(functions)
        , options(options) {
    }

    std::optional<Value> Invoke(const LirFunc &func, const std::vector<Value> &arguments, std::uint32_t depth);
    void Step();

    [[noreturn]] void Fail(const LirFault fault, std::string message) const {
        throw Trap{fault, std::format("in '{}': {}", current ? current->name : "?", std::move(message))};
    }

    const LirFunc &Find(std::string_view name) const;
    Shape ShapeFor(const TypeRef &type) const;

    std::uint64_t steps = 0;
    std::vector<std::string> violations;

private:
    struct Frame {
        std::vector<std::optional<Value>> registers;
    };

    void Execute(Frame &frame, const LirInstr &instr, std::uint32_t depth);
    const Value &Read(const Frame &frame, LirReg reg) const;
    void Define(Frame &frame, LirReg reg, Value value) const;
    WideInteger Literal(std::string_view text, const Shape &shape) const;
    Value Arithmetic(const LirInstr &instr, const Value &lhs, const Value &rhs);
    Value Shift(const LirInstr &instr, const Value &value, const Value &amount);
    Value Compare(const LirInstr &instr, const Value &lhs, const Value &rhs) const;
    Value Cast(const LirInstr &instr, const Value &source) const;
    std::uint64_t Allocate(std::uint64_t bytes);
    std::size_t Address(const Value &pointer, std::uint32_t bytes) const;

    const std::unordered_map<std::string_view, const LirFunc *> &functions;
    const LirInterpreterOptions &options;
    const LirFunc *current = nullptr;
    std::vector<std::uint8_t> memory;
};

void Machine::Step() {
    if (++steps > options.stepLimit) {
        Fail(LirFault::StepLimit, std::format("exceeded the step limit of {}", options.stepLimit));
    }
}

const LirFunc &Machine::Find(const std::string_view name) const {
    const auto found = functions.find(name);
    if (found == functions.end()) {
        Fail(LirFault::Malformed, std::format("no function '{}' in the package", name));
    }
    return *found->second;
}

Shape Machine::ShapeFor(const TypeRef &type) const {
    const auto shape = ShapeOf(type);
    if (!shape) {
        Fail(LirFault::Unsupported, std::format("values of type '{}' are not modeled", type.ToString()));
    }
    return *shape;
}

const Value &Machine::Read(const Frame &frame, const LirReg reg) const {
    if (reg >= frame.registers.size() || !frame.registers[reg]) {
        Fail(LirFault::Malformed, std::format("read of undefined register %{}", reg));
    }
    return *frame.registers[reg];
}

void Machine::Define(Frame &frame, const LirReg reg, Value value) const {
    if (reg == LirNoReg) {
        Fail(LirFault::Malformed, "a value-producing instruction has no destination");
    }
    if (reg >= frame.registers.size()) {
        frame.registers.resize(reg + 1);
    }
    frame.registers[reg] = value;
}

WideInteger Machine::Literal(const std::string_view text, const Shape &shape) const {
    if (shape.valueClass == ValueClass::Bool) {
        if (text == "true" || text == "1" || text == "false" || text == "0") {
            return WideInteger::FromUnsigned(text == "true" || text == "1" ? 1 : 0, shape.width);
        }
    }
    else if (shape.valueClass == ValueClass::Float) {
        if (const auto encoding = ParseFloatEncoding(text, *FindFloatFormat(shape.width))) {
            return encoding->Bits().ConvertedTruncating(shape.width);
        }
    }
    else if (const auto parts = SplitIntegerLiteral(text)) {
        const auto magnitude = WideInteger::Parse(parts->digits, parts->base, WideInteger::MaxBits);
        if (magnitude && WideInteger::MaxBits - magnitude->CountLeadingZeros() <= shape.width) {
            const WideInteger value = magnitude->Truncated(shape.width);
            return parts->negative ? value.Negated() : value;
        }
    }
    Fail(LirFault::Malformed, std::format("literal '{}' does not fit {} bits", text, shape.width));
}

std::uint64_t Machine::Allocate(const std::uint64_t bytes) {
    const std::size_t offset = (memory.size() + 15) / 16 * 16;
    if (offset + bytes > (std::size_t{1} << 26)) {
        Fail(LirFault::StepLimit, "allocated more than 64 MiB");
    }
    memory.resize(offset + bytes, 0xA5);
    return MemoryBase + offset;
}

std::size_t Machine::Address(const Value &pointer, const std::uint32_t bytes) const {
    const std::uint64_t address = pointer.bits.Word64(0);
    if (pointer.shape.valueClass != ValueClass::Pointer || address < MemoryBase ||
        address - MemoryBase + bytes > memory.size()) {
        Fail(LirFault::Malformed,
             std::format("access of {} bytes at 0x{:x} is outside every live allocation", bytes, address));
    }
    return static_cast<std::size_t>(address - MemoryBase);
}

Value Machine::Arithmetic(const LirInstr &instr, const Value &lhs, const Value &rhs) {
    const Shape shape = ShapeFor(instr.type);
    if (shape.valueClass == ValueClass::Float || shape.valueClass == ValueClass::Pointer) {
        Fail(LirFault::Unsupported, std::format("'{}' on '{}'", LirOpcodeName(instr.op), instr.type.ToString()));
    }
    if (lhs.shape.width != shape.width || rhs.shape.width != shape.width) {
        Fail(LirFault::Malformed, std::format("'{}' of {}-bit and {}-bit operands into {} bits",
                                              LirOpcodeName(instr.op), lhs.shape.width, rhs.shape.width, shape.width));
    }
    const WideInteger &a = lhs.bits;
    const WideInteger &b = rhs.bits;
    switch (instr.op) {
    case LirOpcode::Add:
        return {a.Added(b), shape};
    case LirOpcode::Sub:
        return {a.Subtracted(b), shape};
    case LirOpcode::Mul:
        return {a.MultipliedWrapping(b), shape};
    case LirOpcode::And:
        return {a.BitwiseAnd(b), shape};
    case LirOpcode::Or:
        return {a.BitwiseOr(b), shape};
    case LirOpcode::Xor:
        return {a.BitwiseXor(b), shape};
    default:
        break;
    }
    const WideIntegerDivision division = a.Divided(b, shape.isSigned);
    if (division.error == WideIntegerDivisionError::DivideByZero) {
        Fail(LirFault::DivideByZero, std::format("'{}' by zero", LirOpcodeName(instr.op)));
    }
    if (division.error == WideIntegerDivisionError::SignedOverflow) {
        Fail(LirFault::SignedOverflow,
             std::format("'{}' of the {}-bit minimum by -1", LirOpcodeName(instr.op), shape.width));
    }
    return {instr.op == LirOpcode::Div ? division.quotient : division.remainder, shape};
}

Value Machine::Shift(const LirInstr &instr, const Value &value, const Value &amount) {
    const Shape shape = ShapeFor(instr.type);
    if (shape.valueClass != ValueClass::Integer || value.shape.width != shape.width ||
        amount.shape.valueClass != ValueClass::Integer) {
        Fail(LirFault::Malformed,
             std::format("'{}' needs integer operands of the result's width", LirOpcodeName(instr.op)));
    }
    // The back ends read the amount's low word and mask it: x86-64 `and rcx, width - 1` and AArch64 `and` with the
    // same mask on the wide paths, and the 64-bit register shifts' own 63 mask below that.
    const std::uint64_t raw = amount.bits.Word64(0);
    if (amount.bits.ToUnsigned().value_or(UINT64_MAX) >= shape.width) {
        violations.push_back(std::format("in '{}': '{}' by {} at width {}", current->name, LirOpcodeName(instr.op),
                                         amount.bits.ToDecimal(), shape.width));
    }
    const auto masked = static_cast<std::uint32_t>(raw & (shape.width > 64 ? shape.width - 1 : 63));
    if (instr.op == LirOpcode::Shl) {
        return {value.bits.ShiftedLeft(masked), shape};
    }
    return {value.bits.ShiftedRight(masked, instr.op == LirOpcode::Shr && shape.isSigned), shape};
}

Value Machine::Compare(const LirInstr &instr, const Value &lhs, const Value &rhs) const {
    if (lhs.shape.valueClass == ValueClass::Float) {
        Fail(LirFault::Unsupported, std::format("'{}' on a float", LirOpcodeName(instr.op)));
    }
    if (lhs.shape.width != rhs.shape.width) {
        Fail(LirFault::Malformed, std::format("'{}' of {}-bit and {}-bit operands", LirOpcodeName(instr.op),
                                              lhs.shape.width, rhs.shape.width));
    }
    const std::strong_ordering order = lhs.bits.Compare(rhs.bits, lhs.shape.isSigned);
    const bool result = instr.op == LirOpcode::CmpEq ? order == 0
                      : instr.op == LirOpcode::CmpNe ? order != 0
                      : instr.op == LirOpcode::CmpLt ? order < 0
                      : instr.op == LirOpcode::CmpLe ? order <= 0
                      : instr.op == LirOpcode::CmpGt ? order > 0
                                                     : order >= 0;
    const Shape shape = ShapeFor(instr.type);
    return {WideInteger::FromUnsigned(result ? 1 : 0, shape.width), shape};
}

Value Machine::Cast(const LirInstr &instr, const Value &source) const {
    const Shape to = ShapeFor(instr.type);
    const Shape &from = source.shape;
    if (from.valueClass == ValueClass::Float || to.valueClass == ValueClass::Float) {
        if (from.kind != to.kind) {
            Fail(LirFault::Unsupported,
                 std::format("a float conversion to '{}'; reinterpret bits through memory", instr.type.ToString()));
        }
        return {source.bits, to};
    }
    if (to.valueClass == ValueClass::Bool) {
        return {WideInteger::FromUnsigned(source.bits.IsZero() ? 0 : 1, to.width), to};
    }
    return {source.bits.ConvertedWrapping(to.width, from.isSigned), to};
}

void Machine::Execute(Frame &frame, const LirInstr &instr, const std::uint32_t depth) {
    const auto operand = [&](const std::size_t index) -> const Value & {
        if (index >= instr.srcs.size()) {
            Fail(LirFault::Malformed, std::format("'{}' is missing operand {}", LirOpcodeName(instr.op), index));
        }
        return Read(frame, instr.srcs[index]);
    };
    switch (instr.op) {
    case LirOpcode::Const: {
        const Shape shape = ShapeFor(instr.type);
        Define(frame, instr.dst, {Literal(instr.strArg, shape), shape});
        return;
    }
    case LirOpcode::Alloca: {
        const std::uint64_t count = instr.strArg.empty() ? 1 : std::stoull(instr.strArg);
        const auto size = instr.type.SizeInBytes();
        if (!size) {
            Fail(LirFault::Unsupported, std::format("alloca of '{}', which has no known size", instr.type.ToString()));
        }
        const std::uint64_t address = Allocate(*size * count);
        Define(frame, instr.dst, {WideInteger::FromUnsigned(address, 64), *ShapeOf(TypeRef::MakePointer(instr.type))});
        return;
    }
    case LirOpcode::Load: {
        if (instr.srcs.empty()) {
            Fail(LirFault::Unsupported, std::format("load of the named global '{}'", instr.strArg));
        }
        const Shape shape = ShapeFor(instr.type);
        const std::uint32_t bytes = (shape.width + 7) / 8;
        const std::size_t offset = Address(operand(0), bytes);
        std::array<std::uint64_t, WideInteger::MaxBits / 64> words{};
        for (std::uint32_t byte = 0; byte < bytes; ++byte) {
            words[byte / 8] |= std::uint64_t{memory[offset + byte]} << (8 * (byte % 8));
        }
        Define(frame, instr.dst, {WideInteger::FromWords(words, shape.width), shape});
        return;
    }
    case LirOpcode::Store: {
        const Value &value = operand(0);
        const Shape shape = ShapeFor(instr.type);
        if (value.shape.width != shape.width) {
            Fail(LirFault::Malformed,
                 std::format("store of a {}-bit value as '{}'", value.shape.width, instr.type.ToString()));
        }
        const std::size_t offset = Address(operand(1), shape.storage);
        for (std::uint32_t byte = 0; byte < shape.storage; ++byte) {
            const std::uint64_t word = value.bits.Word64(byte / 8);
            memory[offset + byte] = static_cast<std::uint8_t>(word >> (8 * (byte % 8)) & 0xFFU);
        }
        return;
    }
    case LirOpcode::Add:
    case LirOpcode::Sub:
    case LirOpcode::Mul:
    case LirOpcode::Div:
    case LirOpcode::Mod:
    case LirOpcode::And:
    case LirOpcode::Or:
    case LirOpcode::Xor:
        Define(frame, instr.dst, Arithmetic(instr, operand(0), operand(1)));
        return;
    case LirOpcode::Shl:
    case LirOpcode::Shr:
    case LirOpcode::Lshr:
        Define(frame, instr.dst, Shift(instr, operand(0), operand(1)));
        return;
    case LirOpcode::Neg: {
        const Value &value = operand(0);
        if (value.shape.valueClass == ValueClass::Float) {
            // A float negates by its sign bit alone, as the back ends' software-float `Neg` does.
            const WideInteger sign = WideInteger::FromUnsigned(1, value.shape.width).ShiftedLeft(value.shape.width - 1);
            Define(frame, instr.dst, {value.bits.BitwiseXor(sign), value.shape});
            return;
        }
        Define(frame, instr.dst, {value.bits.Negated(), value.shape});
        return;
    }
    case LirOpcode::Not: {
        const Shape shape = ShapeFor(TypeRef::MakeBool());
        Define(frame, instr.dst, {WideInteger::FromUnsigned(operand(0).bits.IsZero() ? 1 : 0, shape.width), shape});
        return;
    }
    case LirOpcode::BitNot: {
        const Value &value = operand(0);
        // A bool stays canonical: bitwise NOT of one is logical NOT.
        const WideInteger result = value.shape.valueClass == ValueClass::Bool
                                     ? value.bits.BitwiseXor(WideInteger::FromUnsigned(1, value.shape.width))
                                     : value.bits.BitwiseNot();
        Define(frame, instr.dst, {result, value.shape});
        return;
    }
    case LirOpcode::CmpEq:
    case LirOpcode::CmpNe:
    case LirOpcode::CmpLt:
    case LirOpcode::CmpLe:
    case LirOpcode::CmpGt:
    case LirOpcode::CmpGe:
        Define(frame, instr.dst, Compare(instr, operand(0), operand(1)));
        return;
    case LirOpcode::Cast:
        Define(frame, instr.dst, Cast(instr, operand(0)));
        return;
    case LirOpcode::Call: {
        const LirFunc &callee = Find(instr.strArg);
        std::vector<Value> arguments;
        for (std::size_t index = 0; index < instr.srcs.size(); ++index) {
            arguments.push_back(operand(index));
        }
        const LirFunc *caller = current;
        std::optional<Value> result = Invoke(callee, arguments, depth + 1);
        current = caller;
        if (instr.dst != LirNoReg) {
            const Shape shape = ShapeFor(instr.type);
            if (!result || result->shape.width != shape.width) {
                Fail(LirFault::Malformed,
                     std::format("call of '{}' does not return a {}-bit value", callee.name, shape.width));
            }
            Define(frame, instr.dst, {result->bits, shape});
        }
        return;
    }
    case LirOpcode::Assert:
        if (operand(0).bits.IsZero()) {
            Fail(LirFault::AssertFailed,
                 std::format("assertion failed at line {}: {}", instr.source.Get().line, instr.strArg));
        }
        return;
    case LirOpcode::Panic:
        Fail(LirFault::Panic, std::format("panic at line {}: {}", instr.source.Get().line, instr.strArg));
    default:
        Fail(LirFault::Unsupported, std::format("opcode '{}'", LirOpcodeName(instr.op)));
    }
}

std::optional<Value> Machine::Invoke(const LirFunc &func, const std::vector<Value> &arguments,
                                     const std::uint32_t depth) {
    current = &func;
    if (depth > options.callDepthLimit) {
        Fail(LirFault::StepLimit, std::format("exceeded the call depth limit of {}", options.callDepthLimit));
    }
    if (func.blocks.empty() || func.isExtern || func.isAsm) {
        Fail(LirFault::Unsupported, "the function has no LIR body");
    }
    if (arguments.size() != func.params.size()) {
        Fail(LirFault::Malformed, std::format("{} arguments for {} parameters", arguments.size(), func.params.size()));
    }
    Frame frame;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const Shape shape = ShapeFor(func.params[index].type);
        if (arguments[index].shape.width != shape.width) {
            Fail(LirFault::Malformed, std::format("argument {} is {} bits for a {}-bit parameter", index,
                                                  arguments[index].shape.width, shape.width));
        }
        Define(frame, func.params[index].reg, {arguments[index].bits, shape});
    }

    const std::size_t memoryMark = memory.size();
    std::uint32_t block = 0;
    std::uint32_t previous = UINT32_MAX;
    while (true) {
        if (block >= func.blocks.size()) {
            Fail(LirFault::Malformed, std::format("branch to missing block {}", block));
        }
        const LirBlock &body = func.blocks[block];
        std::size_t index = 0;
        std::vector<std::pair<LirReg, Value>> incoming;
        for (; index < body.instrs.size() && body.instrs[index].op == LirOpcode::Phi; ++index) {
            Step();
            const LirInstr &phi = body.instrs[index];
            const auto edge = std::ranges::find(phi.phiPreds, previous, &std::pair<LirReg, std::uint32_t>::second);
            if (edge == phi.phiPreds.end()) {
                Fail(LirFault::Malformed, std::format("phi %{} has no value for block {}", phi.dst, previous));
            }
            incoming.emplace_back(phi.dst, Value{Read(frame, edge->first).bits, ShapeFor(phi.type)});
        }
        for (const auto &[reg, value] : incoming) {
            Define(frame, reg, value);
        }
        for (; index < body.instrs.size(); ++index) {
            Step();
            Execute(frame, body.instrs[index], depth);
        }
        if (!body.term) {
            Fail(LirFault::Malformed, std::format("block '{}' has no terminator", body.label));
        }
        Step();
        const LirTerminator &term = *body.term;
        previous = block;
        switch (term.kind) {
        case LirTermKind::Jump:
            block = term.trueTarget;
            break;
        case LirTermKind::Branch:
            block = Read(frame, term.cond).bits.IsZero() ? term.falseTarget : term.trueTarget;
            break;
        case LirTermKind::Switch: {
            const Value &selector = Read(frame, term.cond);
            block = term.defaultTarget;
            for (const LirSwitchCase &option : term.cases) {
                if (Literal(option.value, selector.shape) == selector.bits) {
                    block = option.target;
                    break;
                }
            }
            break;
        }
        case LirTermKind::Return: {
            memory.resize(memoryMark);
            if (!term.retVal) {
                return std::nullopt;
            }
            const Shape shape = ShapeFor(func.returnType);
            const Value &result = Read(frame, *term.retVal);
            if (result.shape.width != shape.width) {
                Fail(LirFault::Malformed,
                     std::format("returns a {}-bit value as '{}'", result.shape.width, func.returnType.ToString()));
            }
            return Value{result.bits, shape};
        }
        case LirTermKind::Unreachable:
            Fail(LirFault::Panic, std::format("reached the unreachable end of block '{}'", body.label));
        }
    }
}
} // namespace

std::string LirRunResult::Describe() const {
    std::string text = failure;
    for (const std::string &violation : violations) {
        text += (text.empty() ? "" : "\n") + violation;
    }
    return text.empty() ? "ok" : text;
}

std::uint32_t LirValueWidth(const TypeRef &type) {
    const auto shape = ShapeOf(type);
    return shape ? shape->width : 0;
}

LirInterpreter::LirInterpreter(const LirPackage &package, const LirInterpreterOptions options)
    : options(options) {
    for (const LirModule &module : package.modules) {
        for (const LirFunc &func : module.funcs) {
            // A body wins over a bodyless declaration of the same name, whichever module comes first.
            const auto [entry, added] = functions.try_emplace(func.name, &func);
            if (!added && entry->second->blocks.empty()) {
                entry->second = &func;
            }
        }
    }
}

LirRunResult LirInterpreter::Run(const std::string_view function, const std::span<const WideInteger> arguments) const {
    Machine machine(functions, options);
    LirRunResult result;
    try {
        const LirFunc &func = machine.Find(function);
        if (arguments.size() != func.params.size()) {
            machine.Fail(LirFault::Malformed,
                         std::format("{} arguments for {} parameters", arguments.size(), func.params.size()));
        }
        std::vector<Value> values;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const Shape shape = machine.ShapeFor(func.params[index].type);
            if (arguments[index].Truncated(shape.width) != arguments[index]) {
                machine.Fail(LirFault::Malformed, std::format("argument {} does not fit {} bits", index, shape.width));
            }
            values.push_back({arguments[index].ConvertedTruncating(shape.width), shape});
        }
        if (const auto value = machine.Invoke(func, values, 0)) {
            result.value = value->bits;
        }
    }
    catch (const Trap &trap) {
        result.fault = trap.fault;
        result.failure = trap.message;
    }
    result.steps = machine.steps;
    result.violations = std::move(machine.violations);
    return result;
}

LirRunResult RunLirFunction(const LirPackage &package, const std::string_view function,
                            const std::span<const WideInteger> arguments, const LirInterpreterOptions &options) {
    return LirInterpreter(package, options).Run(function, arguments);
}
} // namespace Rux::Testing

// Self-tests: hand-built LIR run through the interpreter and checked against WideInteger.

using namespace Rux;
using namespace Rux::Testing;

namespace {
/// Builds one function block by block, numbering registers in order, the way lowering does.
class FunctionBuilder {
public:
    FunctionBuilder(std::string name, const TypeRef &returnType) {
        func.name = std::move(name);
        func.returnType = returnType;
        Block();
    }

    LirReg Param(const TypeRef &type) {
        func.params.push_back({next, type, "p" + std::to_string(next)});
        return next++;
    }

    std::uint32_t Block() {
        func.blocks.push_back({"bb" + std::to_string(func.blocks.size()), {}, std::nullopt});
        return static_cast<std::uint32_t>(func.blocks.size() - 1);
    }

    void SetBlock(const std::uint32_t index) {
        current = index;
    }

    LirReg Emit(const LirOpcode op, const TypeRef &type, std::vector<LirReg> srcs, std::string strArg = {}) {
        LirInstr instr;
        instr.dst = op == LirOpcode::Store ? LirNoReg : next++;
        instr.op = op;
        instr.type = type;
        instr.srcs = std::move(srcs);
        instr.strArg = std::move(strArg);
        func.blocks[current].instrs.push_back(std::move(instr));
        return func.blocks[current].instrs.back().dst;
    }

    LirReg Const(const std::string &text, const TypeRef &type) {
        return Emit(LirOpcode::Const, type, {}, text);
    }

    LirReg Phi(const TypeRef &type, std::vector<std::pair<LirReg, std::uint32_t>> preds) {
        const LirReg dst = Emit(LirOpcode::Phi, type, {});
        func.blocks[current].instrs.back().phiPreds = std::move(preds);
        return dst;
    }

    void Phi(const LirReg phi, const LirReg value, const std::uint32_t block) {
        for (LirBlock &body : func.blocks) {
            for (LirInstr &instr : body.instrs) {
                if (instr.dst == phi) {
                    instr.phiPreds.emplace_back(value, block);
                }
            }
        }
    }

    void Jump(const std::uint32_t target) {
        Branch(LirNoReg, target, target).kind = LirTermKind::Jump;
    }

    LirTerminator &Branch(const LirReg cond, const std::uint32_t yes, const std::uint32_t no) {
        LirTerminator &term = func.blocks[current].term.emplace();
        term.kind = LirTermKind::Branch;
        term.cond = cond;
        term.trueTarget = yes;
        term.falseTarget = no;
        return term;
    }

    void Return(const LirReg value) {
        LirTerminator &term = func.blocks[current].term.emplace();
        term.kind = LirTermKind::Return;
        term.retVal = value;
        term.retType = func.returnType;
    }

    LirFunc func;

private:
    LirReg next = 0;
    std::uint32_t current = 0;
};

LirPackage PackageOf(std::vector<LirFunc> funcs) {
    LirPackage package;
    package.modules.push_back({});
    package.modules.back().funcs = std::move(funcs);
    return package;
}

TypeRef Kind(const TypeRef::Kind kind) {
    return TypeRef::MakePrimitive(kind);
}

/// `name(a, b) = a <op> b` at one type.
LirPackage BinaryFunction(const LirOpcode op, const TypeRef &type, const TypeRef &amountType) {
    FunctionBuilder builder("f", type);
    const LirReg a = builder.Param(type);
    const LirReg b = builder.Param(amountType);
    builder.Return(builder.Emit(op, type, {a, b}));
    return PackageOf({std::move(builder.func)});
}

WideInteger Words(std::initializer_list<std::uint64_t> words, const std::uint32_t width) {
    return WideInteger::FromWords(std::vector<std::uint64_t>(words), width);
}

LirRunResult Run(const LirPackage &package, std::initializer_list<WideInteger> arguments,
                 const LirInterpreterOptions &options = {}) {
    const std::vector<WideInteger> values(arguments);
    return RunLirFunction(package, "f", values, options);
}
} // namespace

TEST_CASE("interpreted wide add carries across every word and wide mul wraps") {
    const WideInteger a = Words({~0ULL, ~0ULL, 7, 0}, 256);
    const WideInteger b = Words({1, 0, ~0ULL, 3}, 256);
    const LirRunResult sum =
        Run(BinaryFunction(LirOpcode::Add, Kind(TypeRef::Kind::UInt256), Kind(TypeRef::Kind::UInt256)), {a, b});
    REQUIRE_MESSAGE(sum.Ok(), sum.Describe());
    CHECK_EQ(*sum.value, a.Added(b));
    CHECK_EQ(sum.value->Word64(3), 4); // the carry out of word 0 ripples through words 1 and 2 into word 3

    const WideInteger x = Words({0x123456789ABCDEF0ULL, ~0ULL, 0, 0, 0, 0, 0, 1ULL << 63}, 512);
    const WideInteger y = Words({~0ULL, 0xFEDCBA9876543210ULL, 5, 0, 0, 0, 0, 0}, 512);
    const LirRunResult product =
        Run(BinaryFunction(LirOpcode::Mul, Kind(TypeRef::Kind::UInt512), Kind(TypeRef::Kind::UInt512)), {x, y});
    REQUIRE_MESSAGE(product.Ok(), product.Describe());
    CHECK_EQ(*product.value, x.MultipliedWrapping(y));
}

TEST_CASE("interpreted shifts follow signedness and flag an amount at or above the width") {
    const TypeRef int256 = Kind(TypeRef::Kind::Int256);
    const TypeRef uint32 = Kind(TypeRef::Kind::UInt32);
    const WideInteger negative = WideInteger::FromUnsigned(12345, 256).Negated();
    const WideInteger amount = WideInteger::FromUnsigned(100, 32);
    const LirRunResult arithmetic = Run(BinaryFunction(LirOpcode::Shr, int256, uint32), {negative, amount});
    REQUIRE_MESSAGE(arithmetic.Ok(), arithmetic.Describe());
    CHECK_EQ(*arithmetic.value, negative.ShiftedRight(100, true));
    const LirRunResult logical = Run(BinaryFunction(LirOpcode::Lshr, int256, uint32), {negative, amount});
    CHECK_EQ(*logical.value, negative.ShiftedRight(100, false));
    const LirRunResult left = Run(BinaryFunction(LirOpcode::Shl, int256, uint32), {negative, amount});
    CHECK_EQ(*left.value, negative.ShiftedLeft(100));

    // 256 masks to 0 on the wide paths and 32 shifts a uint32 out entirely in a 64-bit register; both are recorded.
    const LirRunResult masked =
        Run(BinaryFunction(LirOpcode::Shl, int256, uint32), {negative, WideInteger::FromUnsigned(256, 32)});
    CHECK_EQ(*masked.value, negative);
    CHECK_EQ(masked.violations.size(), 1);
    const LirRunResult narrow = Run(BinaryFunction(LirOpcode::Shl, uint32, uint32),
                                    {WideInteger::FromUnsigned(1, 32), WideInteger::FromUnsigned(32, 32)});
    CHECK(narrow.value->IsZero());
    CHECK_FALSE(narrow.Ok());
}

TEST_CASE("interpreted compares and divisions follow the operand type's signedness") {
    const WideInteger minusOne = WideInteger::AllOnes(128);
    const WideInteger one = WideInteger::FromUnsigned(1, 128);
    // The result is a bool; the operands' type decides whether the comparison is signed.
    FunctionBuilder builder("f", TypeRef::MakeBool());
    const TypeRef int128 = Kind(TypeRef::Kind::Int128);
    const LirReg a = builder.Param(int128);
    const LirReg b = builder.Param(int128);
    builder.Return(builder.Emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {a, b}));
    const LirPackage signedLess = PackageOf({builder.func});
    CHECK_EQ(Run(signedLess, {minusOne, one}).value, WideInteger::FromUnsigned(1, 8));
    builder.func.params[0].type = builder.func.params[1].type = Kind(TypeRef::Kind::UInt128);
    CHECK_EQ(Run(PackageOf({builder.func}), {minusOne, one}).value, WideInteger::FromUnsigned(0, 8));

    const WideInteger minusSeven = WideInteger::FromUnsigned(7, 128).Negated();
    const WideInteger two = WideInteger::FromUnsigned(2, 128);
    CHECK_EQ(Run(BinaryFunction(LirOpcode::Div, int128, int128), {minusSeven, two}).value,
             WideInteger::FromUnsigned(3, 128).Negated());
    CHECK_EQ(Run(BinaryFunction(LirOpcode::Mod, int128, int128), {minusSeven, two}).value, minusOne);
    CHECK_EQ(Run(BinaryFunction(LirOpcode::Div, int128, int128), {one, WideInteger::Zero(128)}).fault,
             LirFault::DivideByZero);
    CHECK_EQ(
        Run(BinaryFunction(LirOpcode::Mod, int128, int128), {WideInteger::MinMagnitude(128, true), minusOne}).fault,
        LirFault::SignedOverflow);
}

TEST_CASE("interpreted casts extend by the source's signedness, truncate and normalize bools") {
    const auto cast = [](const TypeRef::Kind from, const TypeRef::Kind to, const WideInteger &value) {
        FunctionBuilder builder("f", Kind(to));
        const LirReg source = builder.Param(Kind(from));
        builder.Return(builder.Emit(LirOpcode::Cast, Kind(to), {source}, Kind(from).ToString()));
        return Run(PackageOf({std::move(builder.func)}), {value}).value.value_or(WideInteger::Zero(1));
    };
    const WideInteger minusTwo = WideInteger::FromUnsigned(2, 64).Negated();
    CHECK_EQ(cast(TypeRef::Kind::Int64, TypeRef::Kind::Int256, minusTwo), minusTwo.Extended(256, true));
    CHECK_EQ(cast(TypeRef::Kind::UInt64, TypeRef::Kind::Int256, minusTwo), minusTwo.Extended(256, false));
    CHECK_EQ(cast(TypeRef::Kind::Int64, TypeRef::Kind::UInt64, minusTwo), minusTwo);
    CHECK_EQ(cast(TypeRef::Kind::UInt256, TypeRef::Kind::UInt16, Words({0x1122334455667788ULL, 9, 9, 9}, 256)),
             WideInteger::FromUnsigned(0x7788, 16));
    CHECK_EQ(cast(TypeRef::Kind::UInt256, TypeRef::Kind::Bool128, Words({0, 0, 0, 1}, 256)),
             WideInteger::FromUnsigned(1, 128));
    CHECK_EQ(cast(TypeRef::Kind::Bool8, TypeRef::Kind::UInt512, WideInteger::FromUnsigned(1, 8)),
             WideInteger::FromUnsigned(1, 512));
}

TEST_CASE("an interpreted alloca reads back the same bytes as a float or as a uint") {
    const auto reinterpret = [](const TypeRef::Kind stored, const TypeRef::Kind loaded, const WideInteger &value) {
        FunctionBuilder builder("f", Kind(loaded));
        const LirReg source = builder.Param(Kind(stored));
        const LirReg slot = builder.Emit(LirOpcode::Alloca, Kind(TypeRef::Kind::UInt128), {});
        builder.Emit(LirOpcode::Store, Kind(stored), {source, slot});
        builder.Return(builder.Emit(LirOpcode::Load, Kind(loaded), {slot}));
        return Run(PackageOf({std::move(builder.func)}), {value}).value.value_or(WideInteger::Zero(1));
    };
    const WideInteger pattern = Words({0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL}, 128);
    CHECK_EQ(reinterpret(TypeRef::Kind::Float128, TypeRef::Kind::UInt128, pattern), pattern);
    CHECK_EQ(reinterpret(TypeRef::Kind::UInt128, TypeRef::Kind::Float128, pattern), pattern);
    CHECK_EQ(reinterpret(TypeRef::Kind::UInt64, TypeRef::Kind::Float64, pattern.Truncated(64)), pattern.Truncated(64));
    // float80 holds 80 value bits and stores zeros over the rest of its 16 bytes.
    CHECK_EQ(reinterpret(TypeRef::Kind::Float80, TypeRef::Kind::UInt128, pattern.Truncated(80)),
             pattern.Truncated(80).Extended(128, false));
    CHECK_EQ(reinterpret(TypeRef::Kind::UInt128, TypeRef::Kind::Float80, pattern), pattern.Truncated(80));
}

TEST_CASE("an interpreted loop carries its state through phis and the step limit stops a runaway one") {
    // f(n): for i below n, acc = acc * 3 + i, at uint128 so the product wraps.
    const TypeRef uint128 = Kind(TypeRef::Kind::UInt128);
    FunctionBuilder builder("f", uint128);
    const LirReg n = builder.Param(uint128);
    const LirReg zero = builder.Const("0", uint128);
    const std::uint32_t head = builder.Block();
    const std::uint32_t body = builder.Block();
    const std::uint32_t exit = builder.Block();
    builder.Jump(head);
    builder.SetBlock(head);
    const LirReg i = builder.Phi(uint128, {{zero, 0}});
    const LirReg acc = builder.Phi(uint128, {{zero, 0}});
    builder.Branch(builder.Emit(LirOpcode::CmpLt, TypeRef::MakeBool(), {i, n}), body, exit);
    builder.SetBlock(body);
    const LirReg tripled = builder.Emit(LirOpcode::Mul, uint128, {acc, builder.Const("3", uint128)});
    builder.Phi(acc, builder.Emit(LirOpcode::Add, uint128, {tripled, i}), body);
    builder.Phi(i, builder.Emit(LirOpcode::Add, uint128, {i, builder.Const("1", uint128)}), body);
    builder.Jump(head);
    builder.SetBlock(exit);
    builder.Return(acc);
    const LirPackage package = PackageOf({builder.func});

    WideInteger expected = WideInteger::Zero(128);
    for (std::uint64_t step = 0; step < 100; ++step) {
        expected =
            expected.MultipliedWrapping(WideInteger::FromUnsigned(3, 128)).Added(WideInteger::FromUnsigned(step, 128));
    }
    const LirRunResult result = Run(package, {WideInteger::FromUnsigned(100, 128)});
    REQUIRE_MESSAGE(result.Ok(), result.Describe());
    CHECK_EQ(*result.value, expected);

    LirInterpreterOptions options;
    options.stepLimit = 10'000;
    const LirRunResult runaway = Run(package, {WideInteger::AllOnes(128)}, options);
    CHECK_EQ(runaway.fault, LirFault::StepLimit);
    CHECK_FALSE(runaway.value.has_value());
}

TEST_CASE("interpreted calls resolve across modules and may recurse") {
    // fact(n) = n == 0 ? 1 : n * fact(n - 1) at uint256, called by f from another module.
    const TypeRef uint256 = Kind(TypeRef::Kind::UInt256);
    FunctionBuilder fact("fact", uint256);
    const LirReg n = fact.Param(uint256);
    const LirReg one = fact.Const("1", uint256);
    const std::uint32_t base = fact.Block();
    const std::uint32_t recurse = fact.Block();
    fact.Branch(fact.Emit(LirOpcode::CmpEq, TypeRef::MakeBool(), {n, fact.Const("0", uint256)}), base, recurse);
    fact.SetBlock(base);
    fact.Return(one);
    fact.SetBlock(recurse);
    const LirReg inner = fact.Emit(LirOpcode::Call, uint256, {fact.Emit(LirOpcode::Sub, uint256, {n, one})}, "fact");
    fact.Return(fact.Emit(LirOpcode::Mul, uint256, {n, inner}));

    FunctionBuilder entry("f", uint256);
    const LirReg argument = entry.Param(uint256);
    entry.Return(entry.Emit(LirOpcode::Call, uint256, {argument}, "fact"));
    LirPackage package = PackageOf({std::move(entry.func)});
    package.modules.push_back({});
    package.modules.back().funcs.push_back(std::move(fact.func));

    WideInteger expected = WideInteger::FromUnsigned(1, 256);
    for (std::uint64_t factor = 2; factor <= 50; ++factor) {
        expected = expected.MultipliedWrapping(WideInteger::FromUnsigned(factor, 256));
    }
    const LirRunResult result = Run(package, {WideInteger::FromUnsigned(50, 256)});
    REQUIRE_MESSAGE(result.Ok(), result.Describe());
    CHECK_EQ(*result.value, expected);
    CHECK_EQ(Run(package, {WideInteger::FromUnsigned(1000, 256)}).fault, LirFault::StepLimit); // the call depth
}

TEST_CASE("an opcode the interpreter does not model fails naming it") {
    const TypeRef pointer = TypeRef::MakePointer(Kind(TypeRef::Kind::UInt64));
    FunctionBuilder builder("f", pointer);
    const LirReg base = builder.Param(pointer);
    builder.Return(builder.Emit(LirOpcode::FieldPtr, pointer, {base}, "x"));
    const LirRunResult result = Run(PackageOf({std::move(builder.func)}), {WideInteger::Zero(64)});
    CHECK_EQ(result.fault, LirFault::Unsupported);
    CHECK_NE(result.failure.find("fieldptr"), std::string::npos);
}
