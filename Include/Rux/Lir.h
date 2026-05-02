/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#pragma once

#include "Rux/Hir.h"
#include "Rux/Type.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Rux {

// ── Virtual register ──────────────────────────────────────────────────────────

using LirReg = std::uint32_t;
constexpr LirReg LirNoReg = ~LirReg{};

// ── Opcodes ───────────────────────────────────────────────────────────────────

enum class LirOpcode {
    // Literals / memory
    Const,        // %dst = const <type> <value>
    Alloca,       // %dst = alloca <type>
    Load,         // %dst = load <type> %ptr  |  %dst = load <type> <name>
    Store,        // store <type> %val, %ptr
    // Arithmetic
    Add, Sub, Mul, Div, Mod, Pow,
    // Bitwise / shift
    And, Or, Xor, Shl, Shr,
    // Unary
    Neg, Not, BitNot,
    // Comparisons (result: bool)
    CmpEq, CmpNe, CmpLt, CmpLe, CmpGt, CmpGe,
    // Type operations
    Cast,         // %dst = cast %src : <from> to <to>
    // Calls
    Call,         // %dst = call <type> @<name>(%args...)
    CallIndirect, // %dst = call_ind <type> %callee(%args...)
    // Aggregate access (return pointer)
    FieldPtr,     // %dst = fieldptr *<type> %base, <field>
    IndexPtr,     // %dst = indexptr *<type> %base, %idx
    // SSA join
    Phi,          // %dst = phi <type> [%val, bb], ...
};

// ── LIR Instruction ───────────────────────────────────────────────────────────

struct LirInstr {
    LirReg  dst  = LirNoReg; // result register (LirNoReg for Store)
    TypeRef type;             // result type (or value type for Store)
    LirOpcode op  = LirOpcode::Const;
    std::vector<LirReg> srcs; // source registers
    std::string strArg;       // literal (Const), name (Load/Call), field (FieldPtr), from-type (Cast)
    std::vector<std::pair<LirReg, std::uint32_t>> phiPreds; // Phi: (reg, block_index)
    CallingConvention callConv = CallingConvention::Default; // for Call instructions
};

// ── Terminators ───────────────────────────────────────────────────────────────

enum class LirTermKind { Jump, Branch, Return, Switch };

struct LirSwitchCase {
    std::string   value;
    std::uint32_t target = 0;
};

struct LirTerminator {
    LirTermKind   kind         = LirTermKind::Jump;
    LirReg        cond         = LirNoReg;
    std::uint32_t trueTarget   = 0; // Jump / Branch true target
    std::uint32_t falseTarget  = 0; // Branch false target
    std::optional<LirReg> retVal;
    TypeRef       retType;
    std::uint32_t defaultTarget = 0;
    std::vector<LirSwitchCase> cases;
};

// ── Basic block ───────────────────────────────────────────────────────────────

struct LirBlock {
    std::string label;
    std::vector<LirInstr>        instrs;
    std::optional<LirTerminator> term;
};

// ── Function ──────────────────────────────────────────────────────────────────

struct LirParam {
    LirReg      reg;
    TypeRef     type;
    std::string name;
};

struct LirFunc {
    std::string           name;
    std::string           dll;     // non-empty for extern declarations
    bool                  isPublic = false;
    bool                  isExtern = false;
    CallingConvention     callConv = CallingConvention::Default;
    std::vector<LirParam> params;
    TypeRef               returnType;
    std::vector<LirBlock> blocks; // empty for extern declarations
};

// ── Type declarations (passed through from HIR) ───────────────────────────────

struct LirStructField { std::string name; TypeRef type; };
struct LirStructDecl  {
    std::string name;
    bool isPublic = false;
    std::vector<LirStructField> fields;
};

struct LirEnumVariant { std::string name; std::vector<TypeRef> fields; };
struct LirEnumDecl    {
    std::string name;
    bool isPublic = false;
    std::vector<LirEnumVariant> variants;
};

struct LirUnionField { std::string name; TypeRef type; };
struct LirUnionDecl  {
    std::string name;
    bool isPublic = false;
    std::vector<LirUnionField> fields;
};

struct LirConstDecl {
    std::string name;
    bool isPublic = false;
    TypeRef     type;
    std::string value; // printed literal of the constant expression
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

// ── Module / Package ──────────────────────────────────────────────────────────

struct LirModule {
    std::string                name;
    std::vector<LirStructDecl> structs;
    std::vector<LirEnumDecl>   enums;
    std::vector<LirUnionDecl>  unions;
    std::vector<LirConstDecl>  consts;
    std::vector<LirTypeAlias>  typeAliases;
    std::vector<LirExternVar>  externVars;
    std::vector<LirFunc>       funcs;
};

struct LirPackage {
    std::vector<LirModule> modules;
};

// ── Generator ─────────────────────────────────────────────────────────────────

// Lowers a typed HIR package into a flat, control-flow-explicit LIR.
// Each function body is decomposed into basic blocks of three-address
// instructions. Control flow (if/while/for/match) becomes explicit jumps
// and branches; local variables are represented as alloca/load/store triples.
class Lir {
public:
    explicit Lir(HirPackage package);

    [[nodiscard]] LirPackage Generate();

    // Write a human-readable dump of the LIR package to `path`.
    static bool Dump(const LirPackage& package, const std::filesystem::path& path);

private:
    HirPackage hir_;
};

} // namespace Rux
