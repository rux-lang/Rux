#pragma once

#include "Diagnostics/Diagnostics.h"
#include "Ir/Hir/Hir.h"
#include "Ir/Lir/Lir.h"
#include "Lowering/HirToLir/CheckedLirBuilder.h"
#include "Target/Target.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Rux::HirToLirDetail {

// Private state for lowering one HIR package. Function-local state is reset by
// LowerFunc and is shared only by the focused statement, expression, and
// aggregate lowering implementations.
class HirToLirContext {
public:
    HirToLirContext(const TargetContext &target, std::vector<Diagnostic> &outputDiagnostics);

    [[nodiscard]] LirPackage Run(const HirPackage &hir);

private:
    struct LocalConstValue {
        const HirExpr *value = nullptr;
        TypeRef type;
    };

    struct LabelTargets {
        std::uint32_t breakTarget;
        std::uint32_t continueTarget;
    };

    std::unordered_map<std::string, const HirInterface *> interfacesByName;
    CheckedLirBuilder *builder = nullptr;
    std::unordered_map<std::string, LirReg> locals;
    std::unordered_map<std::string, const HirConst *> globalConsts;
    std::unordered_map<std::string, LocalConstValue> localConsts;
    std::unordered_map<LirReg, std::vector<LirReg>> enumPayloadSlots;
    std::uint32_t breakTarget = 0;
    std::uint32_t continueTarget = 0;
    std::unordered_map<std::string, LabelTargets> labelTargets;
    TargetContext targetContext;
    std::unordered_map<std::string, CallingConvention> funcConvs;
    std::unordered_set<std::string> funcNames;
    std::unordered_map<std::string, std::uint32_t> cVariadicFixedParamCounts;
    std::unordered_map<std::string, std::string> externSymbols;
    std::vector<Diagnostic> &diagnostics;
    std::string currentFunction;

    [[nodiscard]] const std::string &SymbolFor(const std::string &name) const;
    [[nodiscard]] static std::optional<TypeRef> CVariadicPromotion(const TypeRef &type);
    void SetCVariadicCallMetadata(LirInstr &call, const std::string &name, const HirCallExpr &expr);

    [[nodiscard]] LirReg NewReg();
    void BuilderFailure(std::string detail) const;
    [[nodiscard]] LirOpcode RequireOpcode(std::optional<LirOpcode> opcode);
    [[nodiscard]] std::uint32_t NewBlock(std::string label = "") const;
    void SetBlock(std::uint32_t idx);
    [[nodiscard]] bool IsTerminated() const;
    void Emit(LirInstr instruction) const;
    void Terminate(LirTerminator terminator) const;
    void Jump(std::uint32_t target) const;
    void Branch(LirReg cond, std::uint32_t trueTarget, std::uint32_t falseTarget) const;
    void Return(std::optional<LirReg> value, TypeRef type) const;
    void Unreachable() const;

    [[nodiscard]] LirReg EmitConst(std::string value, TypeRef type);
    [[nodiscard]] LirReg EmitAlloca(TypeRef type);
    [[nodiscard]] LirReg EmitAlloca(TypeRef type, std::uint64_t count);
    [[nodiscard]] LirReg EmitLoad(LirReg ptr, TypeRef type);
    [[nodiscard]] LirReg EmitNamedLoad(std::string name, TypeRef type);
    void EmitStore(LirReg value, LirReg ptr, TypeRef type) const;
    [[nodiscard]] LirReg EmitBinary(LirOpcode op, LirReg left, LirReg right, TypeRef type);
    [[nodiscard]] LirReg EmitUnary(LirOpcode op, LirReg source, const TypeRef &type);
    [[nodiscard]] LirReg EmitCast(LirReg source, const TypeRef &fromType, TypeRef toType);
    [[nodiscard]] static bool IsScalar(const TypeRef &type);
    [[nodiscard]] static bool IsComparison(TokenKind op);
    [[nodiscard]] LirReg EmitCastIfNeeded(LirReg source, const TypeRef &fromType, const TypeRef &toType);
    [[nodiscard]] LirReg EmitFieldPtr(LirReg base, std::string field, const TypeRef &elementType);
    [[nodiscard]] LirReg EmitIndexPtr(LirReg base, LirReg index, const TypeRef &elementType);
    [[nodiscard]] LirReg EmitGlobalAddr(std::string label, TypeRef pointee = TypeRef::MakeOpaque());
    [[nodiscard]] LirReg EmitStringAddr(std::string value, const TypeRef &elementType);

    [[nodiscard]] bool IsInterfaceType(const TypeRef &type) const;
    [[nodiscard]] static bool IsSliceType(const TypeRef &type);
    [[nodiscard]] static bool IsArrayType(const TypeRef &type);
    [[nodiscard]] static bool IsAggregateEnumType(const TypeRef &type);
    [[nodiscard]] static bool IsStringSliceLiteral(const HirLiteralExpr &expression);
    [[nodiscard]] static TypeRef StringSliceElementType(const HirLiteralExpr &expression);
    [[nodiscard]] static TypeRef SliceElementTypeFromType(const TypeRef &type);

    [[nodiscard]] LirModule LowerModule(const HirModule &module);
    [[nodiscard]] static std::string PrintConstExpr(const HirExpr &expression);
    [[nodiscard]] std::optional<std::string> PrintConstElement(const HirExpr &expression) const;
    void CollectConstContents(const HirConst &constant, LirConstDecl &declaration) const;
    [[nodiscard]] LirFunc LowerFunc(const HirFunc &function, std::string_view nameOverride = "");

    void LowerBlock(const HirBlock &block);
    void LowerStmt(const HirStmt &statement);
    void LowerIf(const HirIfStmt &statement);
    void LowerWhile(const HirWhileStmt &statement);
    void LowerDoWhile(const HirDoWhileStmt &statement);
    void LowerLoop(const HirLoopStmt &statement);
    void LowerFor(const HirForStmt &statement);
    void LowerMatch(const HirMatchStmt &statement);

    void StoreEnumConstructIntoSlot(const HirEnumConstructExpr &expression, LirReg slot);
    void BindLetPattern(const HirPattern &pattern, LirReg subjectPtr, const TypeRef &subjectType);

    LirReg LowerPattern(const HirPattern &pattern, LirReg subjectValue, const TypeRef &subjectType,
                        const std::vector<LirReg> *enumPayload = nullptr, LirReg subjectSlot = LirNoReg);
    LirReg LowerExpr(const HirExpr &expression);
    LirReg LowerPostfix(const HirPostfixExpr &expression);
    LirReg LowerUnary(const HirUnaryExpr &expression);
    LirReg LowerBinary(const HirBinaryExpr &expression);
    LirReg LowerAssign(const HirAssignExpr &expression);
    LirReg LowerInterfaceCall(const HirInterfaceCallExpr &expression);
    LirReg LowerCall(const HirCallExpr &expression);
    LirReg LowerSliceDataPtr(const HirExpr &object, const TypeRef &elementType);
    LirReg LowerRangeIndex(const HirIndexExpr &expression);
    LirReg LowerLValue(const HirExpr &expression);

    void CopySliceValue(LirReg sourceSlot, LirReg destinationSlot, const TypeRef &sliceType);
    void StoreTernaryInit(const HirTernaryExpr &expression, LirReg slot, const TypeRef &type);
    void StoreExprIntoSlot(const HirExpr &expression, LirReg slot, const TypeRef &type);
    LirReg LowerTernary(const HirTernaryExpr &expression);
    void StoreMatchInit(const HirMatchExpr &expression, LirReg slot, const TypeRef &type);
    LirReg LowerMatchExpr(const HirMatchExpr &expression);
    void StoreCoerceToInterface(const HirCoerceToInterfaceExpr &expression, LirReg slot);
    LirReg LowerCoerceToInterface(const HirCoerceToInterfaceExpr &expression);
    void StoreArrayToSlice(const HirArrayToSliceExpr &expression, LirReg slot);
    LirReg LowerArrayToSlice(const HirArrayToSliceExpr &expression);
    LirReg LowerEnumConstruct(const HirEnumConstructExpr &expression);
    void StoreRangeInit(const HirRangeExpr &expression, LirReg slot);
    LirReg LowerRange(const HirRangeExpr &expression);
    LirReg LowerStructInit(const HirStructInitExpr &expression);
    void StoreStructInit(const HirStructInitExpr &expression, LirReg slot);
    LirReg LowerArray(const HirArrayExpr &expression);
    LirReg LowerTuple(const HirTupleExpr &expression);
    void StoreTupleInit(const HirTupleExpr &expression, LirReg slot);
    LirReg LowerStringLiteralSlice(const HirLiteralExpr &expression);
    void StoreStringLiteralSlice(const HirLiteralExpr &expression, LirReg slot);
    void StoreArrayInit(const HirArrayExpr &expression, LirReg slot);
};

} // namespace Rux::HirToLirDetail
