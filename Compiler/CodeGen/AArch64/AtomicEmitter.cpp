// Atomic memory operations and fences on AArch64.
// AArch64 reorders ordinary memory freely, so ordering is carried by the instructions themselves: LDAR loads with
// acquire, STLR stores with release, and both are sequentially consistent together, which is the whole of what a
// sequentially consistent load or store needs. A read-modify-write is an exclusive-access loop: a load-exclusive, the
// new value, and a store-exclusive that fails, and is retried, if any other agent wrote the location in between. The
// acquire and release forms of the exclusive pair carry the operation's order. Large System Extensions instructions
// would make the loop a single instruction, but they are optional in ARMv8.0 and no target feature selects them yet,
// so the loop is what every processor runs.
// Registers: the address is in x10, the operand in x11, a compare-exchange's desired value in x12, the old value lands
// in x9, the new one is built in x13, and w14 receives the store-exclusive status. Every branch distance is fixed,
// because each loop is emitted whole and has no variable-length part.

#include "CodeGen/AArch64/Encoder.h"
#include "CodeGen/AArch64/FunctionEmitter.h"

namespace Rux {
namespace {
constexpr unsigned kOld = 9;
constexpr unsigned kAddress = 10;
constexpr unsigned kOperand = 11;
constexpr unsigned kDesired = 12;
constexpr unsigned kNew = 13;
constexpr unsigned kStatus = 14;

constexpr std::uint8_t kRelaxed = 0;
constexpr std::uint8_t kAcquire = 1;
constexpr std::uint8_t kRelease = 2;
constexpr std::uint8_t kAcquireRelease = 3;
constexpr std::uint8_t kSequentiallyConsistent = 4;

[[nodiscard]] bool Acquires(const std::uint8_t order) {
    return order == kAcquire || order == kAcquireRelease || order == kSequentiallyConsistent;
}

[[nodiscard]] bool Releases(const std::uint8_t order) {
    return order == kRelease || order == kAcquireRelease || order == kSequentiallyConsistent;
}

// The size field of a load/store-exclusive or ordered instruction: 0 for a byte up to 3 for a doubleword.
[[nodiscard]] std::uint32_t SizeField(const int size) {
    switch (size) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 2;
    default:
        return 3;
    }
}

// size 001000 o2 L o1 Rs o0 Rt2 Rn Rt, with the unused register fields all ones.
[[nodiscard]] std::uint32_t ExclusiveClass(const int size, const bool o2, const bool load, const unsigned rs,
                                           const bool o0, const unsigned rn, const unsigned rt) {
    return SizeField(size) << 30U | 0x08000000U | (o2 ? 1U : 0U) << 23U | (load ? 1U : 0U) << 22U | (rs & 31U) << 16U |
           (o0 ? 1U : 0U) << 15U | 31U << 10U | (rn & 31U) << 5U | (rt & 31U);
}

// LDXR / LDAXR (with their byte and halfword forms): a load-exclusive, with acquire when asked.
[[nodiscard]] std::uint32_t LoadExclusive(const int size, const bool acquire, const unsigned rn, const unsigned rt) {
    return ExclusiveClass(size, false, true, 31, acquire, rn, rt);
}

// STXR / STLXR: a store-exclusive whose status lands in `rs`, with release when asked.
[[nodiscard]] std::uint32_t StoreExclusive(const int size, const bool release, const unsigned rs, const unsigned rn,
                                           const unsigned rt) {
    return ExclusiveClass(size, false, false, rs, release, rn, rt);
}

// LDAR: a load with acquire.
[[nodiscard]] std::uint32_t LoadAcquire(const int size, const unsigned rn, const unsigned rt) {
    return ExclusiveClass(size, true, true, 31, true, rn, rt);
}

// STLR: a store with release.
[[nodiscard]] std::uint32_t StoreRelease(const int size, const unsigned rn, const unsigned rt) {
    return ExclusiveClass(size, true, false, 31, true, rn, rt);
}

constexpr std::uint32_t kClrex = 0xD5033F5F;

// The register a value of `size` bytes is computed in: W for up to four bytes, X for eight.
[[nodiscard]] A64Reg Sized(const unsigned code, const int size) {
    return size == 8 ? A64::Xn(code) : A64::Wn(code);
}
} // namespace

// Extends the old value in x9 as the type reads it, after an exclusive or ordered load zero-extended it.
void AArch64FunctionEmitter::ExtendOldValue(const TypeRef &type, const int size) {
    if (!type.IsSigned()) {
        return;
    }
    switch (size) {
    case 1:
        Must(encoder.Sxtb(A64::Xn(kOld), A64::Wn(kOld)), "an atomic result");
        break;
    case 2:
        Must(encoder.Sxth(A64::Xn(kOld), A64::Wn(kOld)), "an atomic result");
        break;
    case 4:
        Must(encoder.Sxtw(A64::Xn(kOld), A64::Wn(kOld)), "an atomic result");
        break;
    default:
        break;
    }
}

bool AArch64FunctionEmitter::EmitAtomic(const LirInstr &instruction) {
    const TypeRef &type = instruction.type;
    switch (instruction.op) {
    case LirOpcode::Fence:
        // An acquire fence only needs earlier loads ordered; every other fence orders loads and stores.
        Must(encoder.Dmb(instruction.memoryOrder == kAcquire ? A64Barrier::Ishld : A64Barrier::Ish), "a fence");
        return true;
    case LirOpcode::SpinHint:
        Must(encoder.Hint(1), "a spin hint"); // YIELD
        return true;
    case LirOpcode::AtomicLoad: {
        const int size = RuntimeSize(type);
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[0], A64::Xn(kAddress));
        if (instruction.memoryOrder == kRelaxed) {
            hooks.LoadScalar(A64::Xn(kOld), address, 0, static_cast<unsigned>(size), type.IsSigned());
        }
        else {
            encoder.Word(LoadAcquire(size, address.code, kOld));
            ExtendOldValue(type, size);
        }
        hooks.StoreToSlot(A64::Xn(kOld), instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicStore: {
        const int size = RuntimeSize(type);
        const A64Reg value = hooks.ReadRawOperand(instruction.srcs[0], static_cast<unsigned>(size), A64::Xn(kOperand));
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[1], A64::Xn(kAddress));
        if (instruction.memoryOrder == kRelaxed) {
            hooks.StoreScalar(value, address, 0, static_cast<unsigned>(size));
        }
        else {
            encoder.Word(StoreRelease(size, address.code, value.code));
        }
        return true;
    }
    case LirOpcode::AtomicExchange:
    case LirOpcode::AtomicFetchAdd:
    case LirOpcode::AtomicFetchSub:
    case LirOpcode::AtomicFetchAnd:
    case LirOpcode::AtomicFetchOr:
    case LirOpcode::AtomicFetchXor: {
        const int size = RuntimeSize(type);
        const A64Reg value = hooks.ReadRawOperand(instruction.srcs[1], static_cast<unsigned>(size), A64::Xn(kOperand));
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[0], A64::Xn(kAddress));
        const bool acquire = Acquires(instruction.memoryOrder);
        const bool release = Releases(instruction.memoryOrder);
        const A64Reg old = Sized(kOld, size);
        const A64Reg next = Sized(kNew, size);
        const A64Reg operand = Sized(value.code, size);
        // retry: ldxr old, [address]; next = old op operand; stxr status, next, [address]; cbnz status, retry
        encoder.Word(LoadExclusive(size, acquire, address.code, kOld));
        unsigned stored = value.code;
        switch (instruction.op) {
        case LirOpcode::AtomicFetchAdd:
            Must(encoder.Add(next, old, operand), "an atomic addition");
            stored = kNew;
            break;
        case LirOpcode::AtomicFetchSub:
            Must(encoder.Sub(next, old, operand), "an atomic subtraction");
            stored = kNew;
            break;
        case LirOpcode::AtomicFetchAnd:
            Must(encoder.And(next, old, operand), "an atomic and");
            stored = kNew;
            break;
        case LirOpcode::AtomicFetchOr:
            Must(encoder.Orr(next, old, operand), "an atomic or");
            stored = kNew;
            break;
        case LirOpcode::AtomicFetchXor:
            Must(encoder.Eor(next, old, operand), "an atomic exclusive or");
            stored = kNew;
            break;
        default:
            break;
        }
        encoder.Word(StoreExclusive(size, release, kStatus, address.code, stored));
        const std::int64_t back = stored == kNew ? -12 : -8;
        Must(encoder.Cbnz(A64::Wn(kStatus), back), "an atomic retry");
        ExtendOldValue(type, size);
        hooks.StoreToSlot(A64::Xn(kOld), instruction.dst, type);
        return true;
    }
    case LirOpcode::AtomicCmpXchg: {
        const int size = RuntimeSize(type);
        // The old value comes back zero-extended, so the expected value is compared zero-extended at the same width.
        const A64Reg expected =
            hooks.ReadRawOperand(instruction.srcs[1], static_cast<unsigned>(size), A64::Xn(kOperand));
        const A64Reg desired =
            hooks.ReadRawOperand(instruction.srcs[2], static_cast<unsigned>(size), A64::Xn(kDesired));
        const A64Reg address = hooks.ReadPointerOperand(instruction.srcs[0], A64::Xn(kAddress));
        if (size == 1) {
            Must(encoder.Uxtb(A64::Wn(kNew), A64::Wn(expected.code)), "a compare-exchange operand");
        }
        else if (size == 2) {
            Must(encoder.Uxth(A64::Wn(kNew), A64::Wn(expected.code)), "a compare-exchange operand");
        }
        else {
            Must(encoder.Mov(Sized(kNew, size), Sized(expected.code, size)), "a compare-exchange operand");
        }
        const bool acquire = Acquires(instruction.memoryOrder) || Acquires(instruction.failureOrder);
        const bool release = Releases(instruction.memoryOrder);
        // retry: ldxr old; cmp old, expected; b.ne fail; stxr status, desired; cbnz status, retry; b done;
        // fail: clrex; done:
        encoder.Word(LoadExclusive(size, acquire, address.code, kOld));
        Must(encoder.Cmp(Sized(kOld, size), Sized(kNew, size)), "a compare-exchange comparison");
        Must(encoder.BCond(A64Condition::Ne, 16), "a compare-exchange mismatch");
        encoder.Word(StoreExclusive(size, release, kStatus, address.code, desired.code));
        Must(encoder.Cbnz(A64::Wn(kStatus), -16), "a compare-exchange retry");
        Must(encoder.B(8), "a compare-exchange success");
        encoder.Word(kClrex);
        ExtendOldValue(type, size);
        hooks.StoreToSlot(A64::Xn(kOld), instruction.dst, type);
        return true;
    }
    default:
        return false;
    }
}
} // namespace Rux
