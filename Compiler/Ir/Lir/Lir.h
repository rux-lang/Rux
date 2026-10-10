#pragma once

#include "Target/AsmInstr.h"
#include "Target/CallingConvention.h"
#include "Types/CaseTypeForm.h"
#include "Types/DropGlue.h"
#include "Types/Type.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rux {
// Virtual register
using LirReg = std::uint32_t;
constexpr LirReg LirNoReg = ~LirReg{};

// Opcodes
enum class LirOpcode {
    // Literals / memory
    Const,  // %dst = const <type> <value>
    Alloca, // %dst = alloca <type>
    Load,   // %dst = load <type> %ptr  |  %dst = load <type> <name>
    Store,  // store <type> %val, %ptr
    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    // Bitwise / shift
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Lshr,
    // Unary
    Neg,
    Not,
    BitNot,
    // Comparisons (result: bool)
    CmpEq,
    CmpNe,
    CmpLt,
    CmpLe,
    CmpGt,
    CmpGe,
    // Type operations
    Cast, // %dst = cast %src : <from> to <to>
    // Calls
    Call,         // %dst = call <type> @<name>(%args...)
    CallIndirect, // %dst = call_ind <type> %callee(%args...)
    Assert,       // reports call-site context and traps when condition is false
    Panic,        // reports call-site context and always traps
    // Aggregate access (return pointer)
    FieldPtr, // %dst = fieldptr *<type> %base, <field>
    IndexPtr, // %dst = indexptr *<type> %base, %idx
    // SSA join
    Phi, // %dst = phi <type> [%val, bb], ...
    // Global address
    GlobalAddr,      // %dst = globaladdr <name> — address of a named global symbol
    ThreadLocalAddr, // %dst = threadlocaladdr <name> — this thread's copy of a thread-local variable
    StringAddr,      // %dst = stringaddr <value> — address of static literal storage
    // Atomic memory operations, each carrying its memory order. The value type is `type` throughout.
    AtomicLoad,     // %dst = atomic_load <type> %ptr
    AtomicStore,    // atomic_store <type> %val, %ptr
    AtomicExchange, // %dst = atomic_xchg <type> %ptr, %val
    AtomicCmpXchg,  // %dst = atomic_cmpxchg <type> %ptr, %expected, %desired — the previous value
    AtomicFetchAdd, // %dst = atomic_add <type> %ptr, %val — the previous value
    AtomicFetchSub, // %dst = atomic_sub <type> %ptr, %val
    AtomicFetchAnd, // %dst = atomic_and <type> %ptr, %val
    AtomicFetchOr,  // %dst = atomic_or <type> %ptr, %val
    AtomicFetchXor, // %dst = atomic_xor <type> %ptr, %val
    Fence,          // fence <order>
    SpinHint,       // spin_hint
    // Stack traces
    FramePointer,       // %dst = frame_pointer *uint64 — this function's frame record: the caller's frame and return
    WriteStandardError, // write_stderr %ptr, %length — writes bytes to standard error, ignoring any failure
};

/// The function every panic and failed assertion calls to print the frames that led to it, synthesized once for a
/// program that can fail that way.
inline constexpr std::string_view kBacktraceFunctionName = "__rux_backtrace";

/// The spelling of an opcode, as a dump writes it and as a diagnostic names it. One name serves both, so a back end
/// reporting an opcode it cannot lower yet names the same thing the LIR dump beside it does.
[[nodiscard]] constexpr std::string_view LirOpcodeName(const LirOpcode op) noexcept {
    switch (op) {
    case LirOpcode::Const:
        return "const";
    case LirOpcode::Alloca:
        return "alloca";
    case LirOpcode::Load:
        return "load";
    case LirOpcode::Store:
        return "store";
    case LirOpcode::Add:
        return "add";
    case LirOpcode::Sub:
        return "sub";
    case LirOpcode::Mul:
        return "mul";
    case LirOpcode::Div:
        return "div";
    case LirOpcode::Mod:
        return "mod";
    case LirOpcode::And:
        return "and";
    case LirOpcode::Or:
        return "or";
    case LirOpcode::Xor:
        return "xor";
    case LirOpcode::Shl:
        return "shl";
    case LirOpcode::Shr:
        return "shr";
    case LirOpcode::Lshr:
        return "lshr";
    case LirOpcode::Neg:
        return "neg";
    case LirOpcode::Not:
        return "not";
    case LirOpcode::BitNot:
        return "bitnot";
    case LirOpcode::CmpEq:
        return "cmpeq";
    case LirOpcode::CmpNe:
        return "cmpne";
    case LirOpcode::CmpLt:
        return "cmplt";
    case LirOpcode::CmpLe:
        return "cmple";
    case LirOpcode::CmpGt:
        return "cmpgt";
    case LirOpcode::CmpGe:
        return "cmpge";
    case LirOpcode::Cast:
        return "cast";
    case LirOpcode::Call:
        return "call";
    case LirOpcode::CallIndirect:
        return "call_ind";
    case LirOpcode::Assert:
        return "assert";
    case LirOpcode::Panic:
        return "panic";
    case LirOpcode::FieldPtr:
        return "fieldptr";
    case LirOpcode::IndexPtr:
        return "indexptr";
    case LirOpcode::Phi:
        return "phi";
    case LirOpcode::GlobalAddr:
        return "globaladdr";
    case LirOpcode::ThreadLocalAddr:
        return "threadlocaladdr";
    case LirOpcode::StringAddr:
        return "stringaddr";
    case LirOpcode::AtomicLoad:
        return "atomic_load";
    case LirOpcode::AtomicStore:
        return "atomic_store";
    case LirOpcode::AtomicExchange:
        return "atomic_xchg";
    case LirOpcode::AtomicCmpXchg:
        return "atomic_cmpxchg";
    case LirOpcode::AtomicFetchAdd:
        return "atomic_add";
    case LirOpcode::AtomicFetchSub:
        return "atomic_sub";
    case LirOpcode::AtomicFetchAnd:
        return "atomic_and";
    case LirOpcode::AtomicFetchOr:
        return "atomic_or";
    case LirOpcode::AtomicFetchXor:
        return "atomic_xor";
    case LirOpcode::Fence:
        return "fence";
    case LirOpcode::SpinHint:
        return "spin_hint";
    case LirOpcode::FramePointer:
        return "frame_pointer";
    case LirOpcode::WriteStandardError:
        return "write_stderr";
    }
    return "?";
}

// LIR Instruction
struct LirInstr {
    LirReg dst = LirNoReg; // result register (LirNoReg for Store)
    TypeRef type;          // result type (or value type for Store)
    LirOpcode op = LirOpcode::Const;
    std::vector<LirReg> srcs; // source registers
    std::string strArg;       // literal (Const), name (Load/Call), field (FieldPtr), from-type (Cast)
    std::vector<std::pair<LirReg, std::uint32_t>> phiPreds;  // Phi: (reg, block_index)
    CallingConvention callConv = CallingConvention::Default; // for Call instructions
    bool isCVariadic = false;                                // call to a C-style variadic extern or function value
    /// A store whose only purpose is that it happens. Zeroizing a secret writes memory nothing reads afterwards, which
    /// is exactly the shape dead-store elimination removes, so the write says so about itself.
    bool isVolatile = false;
    /// The memory order of an atomic instruction or fence, and the failure order of a compare-exchange, as
    /// `MemoryOrderKind` values: 0 relaxed, 1 acquire, 2 release, 3 acquire-release, 4 sequentially consistent.
    std::uint8_t memoryOrder = 0;
    std::uint8_t failureOrder = 0;
    /// Present only with isCVariadic. AArch64 platform variants need the declaration boundary to promote and/or place
    /// anonymous arguments correctly. It counts arguments only: a CallIndirect's callee in srcs[0] is not one of them.
    std::optional<std::uint32_t> cVariadicFixedParamCount;
    /// Optional source context used by instructions that report runtime failures.
    std::string sourceFile;
    std::string sourceFunction;
    std::uint32_t sourceLine = 0;
    std::uint32_t sourceColumn = 0;
};

// Terminators
enum class LirTermKind {
    Jump,
    Branch,
    Return,
    Switch,
    Unreachable,
};

/// The spelling of a terminator, under the same rule as LirOpcodeName.
[[nodiscard]] constexpr std::string_view LirTermKindName(const LirTermKind kind) noexcept {
    switch (kind) {
    case LirTermKind::Jump:
        return "jump";
    case LirTermKind::Branch:
        return "branch";
    case LirTermKind::Return:
        return "ret";
    case LirTermKind::Switch:
        return "switch";
    case LirTermKind::Unreachable:
        return "unreachable";
    }
    return "?";
}

struct LirSwitchCase {
    std::string value;
    std::uint32_t target = 0;
};

struct LirTerminator {
    LirTermKind kind = LirTermKind::Jump;
    LirReg cond = LirNoReg;
    std::uint32_t trueTarget = 0;  // Jump / Branch true target
    std::uint32_t falseTarget = 0; // Branch false target
    std::optional<LirReg> retVal;
    TypeRef retType;
    std::uint32_t defaultTarget = 0;
    std::vector<LirSwitchCase> cases;
};

// Basic block
struct LirBlock {
    std::string label;
    std::vector<LirInstr> instrs;
    std::optional<LirTerminator> term;
};

// Function
struct LirParam {
    LirReg reg;
    TypeRef type;
    std::string name;
};

struct LirFunc {
    std::string name;
    /// The name a stack trace prints, as the source spells it; the linker name when the source gave none.
    std::string displayName;
    std::string dll; // non-empty for extern declarations
    bool isPublic = false;
    /// Public and owned by the package being compiled: a library artifact exports it and keeps it reachable.
    bool isExported = false;
    bool isExtern = false;
    bool isNoReturn = false;
    bool isVariadic = false; // C-style variadic extern declaration
    CallingConvention callConv = CallingConvention::Default;
    std::vector<LirParam> params;
    TypeRef returnType;
    std::vector<LirBlock> blocks;  // empty for extern declarations
    bool isAsm = false;            // raw x86-64 assembly function
    std::vector<AsmInstr> asmBody; // instructions when isAsm
};

// Type declarations (passed through from HIR)
struct LirStructField {
    std::string name;
    TypeRef type;
};

struct LirStructDecl {
    std::string name;
    bool isPublic = false;
    std::vector<std::string> typeParams;
    std::vector<LirStructField> fields;
};

struct LirEnumVariant {
    std::string name;
    std::vector<TypeRef> fields;
    std::optional<std::string> discriminant;
};

struct LirEnumDecl {
    CaseTypeForm form = CaseTypeForm::Enumeration;
    std::string name;
    bool isPublic = false;
    std::vector<std::string> typeParams;
    TypeRef baseType;
    std::vector<LirEnumVariant> variants;

    [[nodiscard]] bool IsVariant() const noexcept {
        return form == CaseTypeForm::Variant;
    }
};

struct LirUnionField {
    std::string name;
    TypeRef type;
};

struct LirUnionDecl {
    std::string name;
    bool isPublic = false;
    std::vector<LirUnionField> fields;
};

/// One step from a constant's value into a part of it: the member of `aggregate` named `member`, which is a struct
/// field's name, or a tuple or array element's decimal index.
struct LirConstMember {
    TypeRef aggregate;
    std::string member;
};

/// One scalar of a constant's value, as a literal of `type`, at the place the members of `path` reach from the start of
/// the value. A scalar constant is one of these with an empty path.
struct LirConstScalar {
    std::vector<LirConstMember> path;
    TypeRef type;
    std::string literal;
};

struct LirConstDecl {
    std::string name;
    bool isPublic = false;
    /// Public and owned by the package being compiled: a library artifact exports it and keeps it reachable.
    bool isExported = false;
    TypeRef type;
    std::string value; // printed literal of the constant expression

    /// Constant sequence contents. T[..] publishes a {data, length} header; T[N] publishes the inline elements
    /// directly under `name`.
    TypeRef elementType;
    std::vector<std::string> elements;
    std::string text;
    bool isTextSlice = false;
    bool hasSequenceData = false;

    /// The contents of a constant that is not a sequence: every scalar its value folds to, which the code generators
    /// lay out in read-only data at the constant's size and alignment, zero wherever no scalar lies. `@C` addresses
    /// those bytes. `hasValueData` is false when some part of the value has no layout the code generators know, and the
    /// symbol is then a placeholder that `@C` does not address.
    std::vector<LirConstScalar> scalars;
    bool hasValueData = false;
};

/// A module variable: writable storage under `name`, starting as the bytes its scalars lay out at the type's size and
/// alignment, zero wherever no scalar lies. A value whose bytes are all zero is placed in zero-fill storage.
struct LirGlobal {
    std::string name;
    bool isPublic = false;
    /// Public and owned by the package being compiled: a library artifact exports it and keeps it reachable.
    bool isExported = false;
    TypeRef type;
    std::vector<LirConstScalar> scalars;
    /// One copy per thread, in the image's thread-local template rather than its ordinary data.
    bool isThreadLocal = false;
};

struct LirTypeAlias {
    std::string name;
    bool isPublic = false;
    TypeRef type;
};

struct LirExternVar {
    std::string name;
    bool isPublic = false;
    TypeRef type;
    /// Public and owned by the package being compiled: a library artifact keeps it reachable.
    bool isExported = false;
};

// Module / Package

// Vtable — a sequence of function-pointer entries emitted in .rodata
struct LirVtable {
    std::string label;                // e.g. __vtable__int64__Display
    std::vector<std::string> methods; // mangled method names in vtable order
};

struct LirModule {
    std::string name;
    std::vector<std::string> interfaceNames; // interface types (fat ptr = 16 bytes)
    std::vector<LirStructDecl> structs;
    std::vector<LirEnumDecl> enums;
    std::vector<LirUnionDecl> unions;
    std::vector<LirConstDecl> consts;
    std::vector<LirGlobal> globals;
    std::vector<LirTypeAlias> typeAliases;
    std::vector<LirExternVar> externVars;
    std::vector<LirFunc> funcs;
    std::vector<LirVtable> vtables;
};

struct LirPackage {
    std::vector<LirModule> modules;
    std::vector<DropGluePlan> dropGlues;
};
} // namespace Rux
