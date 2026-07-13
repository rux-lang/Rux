#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Semantic/Type.h"
#include "Target/AsmInstr.h"
#include "Target/CallingConvention.h"

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
    Pow,
    // Bitwise / shift
    And,
    Or,
    Xor,
    Shl,
    Shr,
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
    GlobalAddr, // %dst = globaladdr <name> — address of a named global symbol
    StringAddr, // %dst = stringaddr <value> — address of static literal storage
};

// LIR Instruction
struct LirInstr {
    LirReg dst = LirNoReg; // result register (LirNoReg for Store)
    TypeRef type;          // result type (or value type for Store)
    LirOpcode op = LirOpcode::Const;
    std::vector<LirReg> srcs; // source registers
    std::string strArg;       // literal (Const), name (Load/Call), field (FieldPtr), from-type (Cast)
    std::vector<std::pair<LirReg, std::uint32_t>> phiPreds;  // Phi: (reg, block_index)
    CallingConvention callConv = CallingConvention::Default; // for Call instructions
    // Optional source context used by instructions that report runtime failures.
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
    std::string dll; // non-empty for extern declarations
    bool isPublic = false;
    bool isExtern = false;
    bool isNoReturn = false;
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
    std::string name;
    bool isPublic = false;
    TypeRef baseType;
    std::vector<LirEnumVariant> variants;
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

struct LirConstDecl {
    std::string name;
    bool isPublic = false;
    TypeRef type;
    std::string value; // printed literal of the constant expression

    // A constant of slice type is emitted as read-only data plus a
    // {data, length} header published under `name`. Its contents come either
    // from an array literal (elements) or from a string literal (text, holding
    // the already decoded bytes); exactly one is set.
    TypeRef elementType;
    std::vector<std::string> elements;
    std::string text;
    bool isTextSlice = false;
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
    std::vector<LirTypeAlias> typeAliases;
    std::vector<LirExternVar> externVars;
    std::vector<LirFunc> funcs;
    std::vector<LirVtable> vtables;
};

struct LirPackage {
    std::vector<LirModule> modules;
};

} // namespace Rux
