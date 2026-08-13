// Package, module, checked-builder, and function setup for HIR-to-LIR lowering.

#include "Lowering/HirToLir/HirToLir.h"
#include "Lowering/HirToLir/HirToLirContext.h"

#include <cassert>
#include <format>
#include <utility>

namespace Rux::HirToLirDetail {

HirToLirContext::HirToLirContext(const TargetContext &target)
    : targetContext(target) {
}

LirPackage HirToLirContext::Run(const HirPackage &hir) {
    globalConsts.clear();
    for (const auto &mod : hir.modules) {
        for (const auto &iface : mod.interfaces) {
            interfacesByName[iface.name] = &iface;
        }
    }
    for (const auto &mod : hir.modules) {
        for (const auto &c : mod.consts) {
            globalConsts[c.name] = &c;
            globalConsts[mod.name + "::" + c.name] = &c;
        }
    }
    // Calling conventions are resolved package-wide: a call may target an
    // extern function declared in an imported module (e.g. C::printf), so
    // the map must span every module, not just the one being lowered.
    funcConvs.clear();
    funcNames.clear();
    externSymbols.clear();
    cVariadicFixedParamCounts.clear();
    for (const auto &mod : hir.modules) {
        for (const auto &ef : mod.externFuncs) {
            // Extern C functions default to the target's C ABI so arguments
            // land in the registers the shared library expects. Resolve
            // both an omitted convention and explicit `.C` here, against
            // the target OS and architecture rather than the host.
            funcConvs[ef.name] = ef.callConv == CallingConvention::Default
                                   ? PlatformCConvention(targetContext.os, targetContext.arch)
                                   : ResolveCConvention(ef.callConv, targetContext.os, targetContext.arch);
            funcNames.insert(ef.name);
            if (ef.isVariadic) {
                cVariadicFixedParamCounts[ef.name] = static_cast<std::uint32_t>(ef.params.size());
            }
            // The optional second `#Link` argument renames the imported symbol. Record it
            // package-wide, for the same reason funcConvs is: a call may
            // target an extern declared in another module, and every
            // reference has to reach the linker under the same name.
            if (!ef.symbolName.empty() && ef.symbolName != ef.name) {
                externSymbols[ef.name] = ef.symbolName;
            }
        }
        for (const auto &f : mod.funcs) {
            if (f.callConv != CallingConvention::Default) {
                funcConvs[f.name] = f.callConv;
            }
            funcNames.insert(f.name);
        }
    }
    LirPackage pkg;
    for (const auto &mod : hir.modules) {
        pkg.modules.push_back(LowerModule(mod));
    }
    return pkg;
}

// The name a function reaches the linker under: the second `#Link` argument
// override when one was given, otherwise the Rux name itself.
const std::string &HirToLirContext::SymbolFor(const std::string &name) const {
    const auto it = externSymbols.find(name);
    return it == externSymbols.end() ? name : it->second;
}

[[nodiscard]] std::optional<TypeRef> HirToLirContext::CVariadicPromotion(const TypeRef &type) {
    switch (type.kind) {
    case TypeRef::Kind::Float32:
        return TypeRef::MakeFloat64();
    case TypeRef::Kind::Bool8:
    case TypeRef::Kind::Bool16:
    case TypeRef::Kind::Char8:
    case TypeRef::Kind::Char16:
    case TypeRef::Kind::Int8:
    case TypeRef::Kind::Int16:
    case TypeRef::Kind::UInt8:
    case TypeRef::Kind::UInt16:
        return TypeRef::MakeInt32();
    default:
        return std::nullopt;
    }
}

// Preserve the declaration boundary on every target. Apple and FreeBSD
// AArch64 apply C's default argument promotions before their platform call
// layouts; keeping the promotion in LIR makes the value's ABI type explicit
// and lets code generation move it like any other converted value.
void HirToLirContext::SetCVariadicCallMetadata(LirInstr &call, const std::string &name, const HirCallExpr &expr) {
    const auto found = cVariadicFixedParamCounts.find(name);
    if (found == cVariadicFixedParamCounts.end()) {
        return;
    }
    call.isCVariadic = true;
    call.cVariadicFixedParamCount = found->second;

    const bool promotesAArch64 = targetContext.arch == Target::Arch::AArch64 &&
                                 (targetContext.os == Target::OS::MacOS || targetContext.os == Target::OS::FreeBSD);
    if (!promotesAArch64) {
        return;
    }
    for (std::size_t i = found->second; i < call.srcs.size() && i < expr.args.size(); ++i) {
        if (const auto promoted = CVariadicPromotion(expr.args[i]->type)) {
            call.srcs[i] = EmitCast(call.srcs[i], expr.args[i]->type, *promoted);
        }
    }
}

// Block / register allocation
LirReg HirToLirContext::NewReg() {
    return builder->AllocateRegister();
}

[[noreturn]] void HirToLirContext::BuilderFailure() {
    assert(false && "checked LIR builder rejected lowering output");
    std::unreachable();
}

void HirToLirContext::Require(const bool accepted) {
    if (!accepted) {
        BuilderFailure();
    }
}

[[nodiscard]] LirOpcode HirToLirContext::RequireOpcode(const std::optional<LirOpcode> opcode) {
    if (!opcode) {
        BuilderFailure();
    }
    return *opcode;
}

[[nodiscard]] std::uint32_t HirToLirContext::NewBlock(std::string label) const {
    return builder->CreateBlock(std::move(label));
}

void HirToLirContext::SetBlock(const std::uint32_t idx) {
    Require(builder->SelectBlock(idx));
}

[[nodiscard]] bool HirToLirContext::IsTerminated() const {
    return builder->IsTerminated();
}

// Instruction emission
void HirToLirContext::Emit(LirInstr i) const {
    Require(builder->Insert(std::move(i)));
}

void HirToLirContext::Terminate(LirTerminator t) const {
    Require(builder->Terminate(std::move(t)));
}

void HirToLirContext::Jump(std::uint32_t target) const {
    Terminate(LirTerminator{
        .kind = LirTermKind::Jump, .trueTarget = target, .retVal = std::nullopt, .retType = {}, .cases = {}});
}

void HirToLirContext::Branch(const LirReg cond, const std::uint32_t trueTarget, std::uint32_t falseTarget) const {
    Terminate(LirTerminator{.kind = LirTermKind::Branch,
                            .cond = cond,
                            .trueTarget = trueTarget,
                            .falseTarget = falseTarget,
                            .retVal = std::nullopt,
                            .retType = {},
                            .cases = {}});
}

void HirToLirContext::Return(const std::optional<LirReg> val, TypeRef type) const {
    LirTerminator t;
    t.kind = LirTermKind::Return;
    t.retVal = val;
    t.retType = std::move(type);
    Terminate(std::move(t));
}

void HirToLirContext::Unreachable() const {
    LirTerminator t;
    t.kind = LirTermKind::Unreachable;
    Terminate(std::move(t));
}

// Instruction builders

LirReg HirToLirContext::EmitConst(std::string value, TypeRef type) {
    LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::Const;
    i.type = std::move(type);
    i.strArg = std::move(value);
    Emit(std::move(i));
    return r;
}

LirReg HirToLirContext::EmitAlloca(TypeRef type) {
    LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::Alloca;
    i.type = std::move(type);
    Emit(std::move(i));
    return r;
}

LirReg HirToLirContext::EmitAlloca(TypeRef type, std::uint64_t count) {
    const LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::Alloca;
    i.type = std::move(type);
    i.strArg = std::to_string(count);
    Emit(std::move(i));
    return r;
}

LirReg HirToLirContext::EmitLoad(LirReg ptr, TypeRef type) {
    const LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::Load;
    i.type = std::move(type);
    i.srcs = {ptr};
    Emit(std::move(i));
    return r;
}

LirReg HirToLirContext::EmitNamedLoad(std::string name, TypeRef type) {
    LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::Load;
    i.type = std::move(type);
    i.strArg = std::move(name);
    Emit(std::move(i));
    return r;
}

void HirToLirContext::EmitStore(LirReg val, LirReg ptr, TypeRef type) const {
    LirInstr i;
    i.dst = LirNoReg;
    i.op = LirOpcode::Store;
    i.type = std::move(type);
    i.srcs = {val, ptr};
    Emit(std::move(i));
}

LirReg HirToLirContext::EmitBinary(const LirOpcode op, LirReg l, LirReg r, TypeRef type) {
    const LirReg dst = NewReg();
    LirInstr i;
    i.dst = dst;
    i.op = op;
    i.type = std::move(type);
    i.srcs = {l, r};
    Emit(std::move(i));
    return dst;
}

LirReg HirToLirContext::EmitUnary(LirOpcode op, LirReg src, const TypeRef &type) {
    const LirReg dst = NewReg();
    LirInstr i;
    i.dst = dst;
    i.op = op;
    i.type = type;
    i.srcs = {src};
    Emit(std::move(i));
    return dst;
}

LirReg HirToLirContext::EmitCast(LirReg src, const TypeRef &fromType, TypeRef toType) {
    LirReg dst = NewReg();
    LirInstr i;
    i.dst = dst;
    i.op = LirOpcode::Cast;
    i.type = std::move(toType);
    i.srcs = {src};
    i.strArg = fromType.ToString();
    Emit(std::move(i));
    return dst;
}

// The types that live in a register and are held to a width: everything a
// comparison can widen one side of without changing what is being asked.
bool HirToLirContext::IsScalar(const TypeRef &t) {
    return t.IsNumeric() || t.IsBool() || t.kind == TypeRef::Kind::Char8 || t.kind == TypeRef::Kind::Char16 ||
           t.kind == TypeRef::Kind::Char32;
}

bool HirToLirContext::IsComparison(const TokenKind op) {
    using TK = TokenKind;
    return op == TK::Equal || op == TK::BangEqual || op == TK::Less || op == TK::LessEqual || op == TK::Greater ||
           op == TK::GreaterEqual;
}

LirReg HirToLirContext::EmitCastIfNeeded(LirReg src, const TypeRef &fromType, const TypeRef &toType) {
    if (src == LirNoReg || fromType.IsUnknown() || toType.IsUnknown() || fromType == toType) {
        return src;
    }
    return EmitCast(src, fromType, toType);
}

LirReg HirToLirContext::EmitFieldPtr(LirReg base, std::string field, const TypeRef &elemType) {
    LirReg ptr = NewReg();
    LirInstr i;
    i.dst = ptr;
    i.op = LirOpcode::FieldPtr;
    i.type = TypeRef::MakePointer(elemType);
    i.srcs = {base};
    i.strArg = std::move(field);
    Emit(std::move(i));
    return ptr;
}

LirReg HirToLirContext::EmitIndexPtr(LirReg base, LirReg idx, const TypeRef &elemType) {
    LirReg ptr = NewReg();
    LirInstr i;
    i.dst = ptr;
    i.op = LirOpcode::IndexPtr;
    i.type = TypeRef::MakePointer(elemType);
    i.srcs = {base, idx};
    Emit(std::move(i));
    return ptr;
}

// `pointee` names what the symbol holds, so that a later FieldPtr can find
// the layout. Opaque is right for a function address, which is never
// dereferenced.
LirReg HirToLirContext::EmitGlobalAddr(std::string label, TypeRef pointee) {
    LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::GlobalAddr;
    i.type = TypeRef::MakePointer(std::move(pointee));
    i.strArg = std::move(label);
    Emit(std::move(i));
    return r;
}

LirReg HirToLirContext::EmitStringAddr(std::string value, const TypeRef &elemType) {
    LirReg r = NewReg();
    LirInstr i;
    i.dst = r;
    i.op = LirOpcode::StringAddr;
    i.type = TypeRef::MakePointer(elemType);
    i.strArg = std::move(value);
    Emit(std::move(i));
    return r;
}

[[nodiscard]] bool HirToLirContext::IsInterfaceType(const TypeRef &t) const {
    return t.kind == TypeRef::Kind::Named && interfacesByName.contains(t.name);
}

bool HirToLirContext::IsSliceType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<");
}

bool HirToLirContext::IsArrayType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Array;
}

bool HirToLirContext::IsAggregateEnumType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Named && !type.inner.empty() && type.inner[0].kind == TypeRef::Kind::Array &&
           type.SizeInBytes().value_or(0) > 8;
}

bool HirToLirContext::IsStringSliceLiteral(const HirLiteralExpr &e) {
    return e.type.kind == TypeRef::Kind::Named &&
           (e.type.name == "Slice<char8>" || e.type.name == "Slice<char16>" || e.type.name == "Slice<char32>");
}

TypeRef HirToLirContext::StringSliceElementType(const HirLiteralExpr &e) {
    if (e.type.kind == TypeRef::Kind::Named) {
        if (e.type.name == "Slice<char16>") {
            return TypeRef::MakeChar16();
        }
        if (e.type.name == "Slice<char32>") {
            return TypeRef::MakeChar32();
        }
    }
    return TypeRef::MakeChar8();
}

// Module lowering
LirModule HirToLirContext::LowerModule(const HirModule &mod) {
    // funcConvs is populated package-wide in Run() before any module is
    // lowered, so cross-module (imported) calls resolve correctly.
    LirModule lm;
    lm.name = mod.name;
    for (const auto &iface : mod.interfaces) {
        lm.interfaceNames.push_back(iface.name);
    }
    for (const auto &s : mod.structs) {
        LirStructDecl sd;
        sd.name = s.name;
        sd.isPublic = s.isPublic;
        sd.typeParams = s.typeParams;
        for (const auto &f : s.fields) {
            sd.fields.push_back({f.name, f.type});
        }
        lm.structs.push_back(std::move(sd));
    }
    for (const auto &e : mod.enums) {
        LirEnumDecl ed;
        ed.name = e.name;
        ed.isPublic = e.isPublic;
        ed.typeParams = e.typeParams;
        ed.baseType = e.baseType;
        for (const auto &v : e.variants) {
            ed.variants.push_back({v.name, v.fields, v.discriminant});
        }
        lm.enums.push_back(std::move(ed));
    }
    for (const auto &u : mod.unions) {
        LirUnionDecl ud;
        ud.name = u.name;
        ud.isPublic = u.isPublic;
        for (const auto &f : u.fields) {
            ud.fields.push_back({f.name, f.type});
        }
        lm.unions.push_back(std::move(ud));
    }
    for (const auto &c : mod.consts) {
        globalConsts[c.name] = &c;
        LirConstDecl cd;
        cd.name = c.name;
        cd.isPublic = c.isPublic;
        cd.type = c.type;
        cd.value = PrintConstExpr(*c.value);
        CollectConstContents(c, cd);
        lm.consts.push_back(std::move(cd));
    }
    for (const auto &ta : mod.typeAliases) {
        lm.typeAliases.push_back({ta.name, ta.isPublic, ta.type});
    }
    for (const auto &ev : mod.externVars) {
        lm.externVars.push_back({ev.name, ev.isPublic, ev.type});
    }
    for (const auto &ef : mod.externFuncs) {
        LirFunc lf;
        lf.name = SymbolFor(ef.name);
        lf.dll = ef.dll;
        lf.isPublic = ef.isPublic;
        lf.isExtern = true;
        lf.isNoReturn = ef.isNoReturn;
        lf.isVariadic = ef.isVariadic;
        lf.callConv = funcConvs.at(ef.name);
        lf.returnType = ef.returnType;
        LirReg pr = 0;
        for (const auto &p : ef.params) {
            lf.params.push_back({pr++, p.type, p.name});
        }
        lm.funcs.push_back(std::move(lf));
    }
    for (const auto &f : mod.funcs) {
        lm.funcs.push_back(LowerFunc(f));
    }
    for (const auto &impl : mod.impls) {
        assert(impl.methods.size() == impl.methodLinkerNames.size());
        for (std::size_t i = 0; i < impl.methods.size(); ++i) {
            lm.funcs.push_back(LowerFunc(impl.methods[i], impl.methodLinkerNames[i]));
        }
        if (!impl.vtableLabel.empty()) {
            LirVtable vt;
            vt.label = impl.vtableLabel;
            vt.methods = impl.vtableEntries;
            lm.vtables.push_back(std::move(vt));
        }
    }
    return lm;
}

// Render a simple constant expression to a printable string.
std::string HirToLirContext::PrintConstExpr(const HirExpr &e) {
    if (auto *lit = dynamic_cast<const HirLiteralExpr *>(&e)) {
        return lit->value;
    }
    if (auto *v = dynamic_cast<const HirVarExpr *>(&e)) {
        return v->name;
    }
    if (auto *b = dynamic_cast<const HirBinaryExpr *>(&e)) {
        return PrintConstExpr(*b->left) + " op " + PrintConstExpr(*b->right);
    }
    return "<const>";
}

// The literal an array element spells out, with a leading minus folded in
// and a named constant resolved to the literal it stands for. Anything else
// is not a constant the backend can lay out; the semantic analyzer rejects
// those before we get here.
std::optional<std::string> HirToLirContext::PrintConstElement(const HirExpr &e) const {
    if (auto *lit = dynamic_cast<const HirLiteralExpr *>(&e)) {
        return lit->value;
    }
    if (auto *u = dynamic_cast<const HirUnaryExpr *>(&e); u && u->op == TokenKind::Minus) {
        if (const auto inner = PrintConstElement(*u->operand)) {
            return inner->starts_with('-') ? inner->substr(1) : "-" + *inner;
        }
    }
    if (auto *v = dynamic_cast<const HirVarExpr *>(&e)) {
        if (const auto it = globalConsts.find(v->name); it != globalConsts.end()) {
            return PrintConstElement(*it->second->value);
        }
    }
    return std::nullopt;
}

// Records constant slice/array contents for direct read-only emission.
void HirToLirContext::CollectConstContents(const HirConst &c, LirConstDecl &cd) const {
    if (!IsSliceType(c.type) && !IsArrayType(c.type)) {
        return;
    }
    if (auto *lit = dynamic_cast<const HirLiteralExpr *>(c.value.get()); lit && IsStringSliceLiteral(*lit)) {
        cd.isTextSlice = true;
        cd.hasSequenceData = true;
        cd.text = lit->value;
        cd.elementType = StringSliceElementType(*lit);
        return;
    }
    const HirExpr *value = c.value.get();
    if (const auto *view = dynamic_cast<const HirArrayToSliceExpr *>(value)) {
        value = view->value.get();
    }
    if (auto *arr = dynamic_cast<const HirArrayExpr *>(value)) {
        std::vector<std::string> elements;
        for (const auto &element : arr->elements) {
            const auto printed = PrintConstElement(*element);
            if (!printed) {
                return;
            }
            elements.push_back(*printed);
        }
        cd.elementType =
            arr->elementType.IsUnknown() && !arr->elements.empty() ? arr->elements.front()->type : arr->elementType;
        cd.elements = std::move(elements);
        cd.hasSequenceData = true;
    }
}

// Function lowering
LirFunc HirToLirContext::LowerFunc(const HirFunc &hf, const std::string_view nameOverride) {
    locals.clear();
    localConsts.clear();
    enumPayloadSlots.clear();
    LirFunc lf;
    lf.name = nameOverride.empty() ? hf.name : std::string(nameOverride);
    lf.isPublic = hf.isPublic;
    lf.isExtern = false;
    lf.isNoReturn = hf.isNoReturn;
    lf.callConv = hf.callConv;
    lf.returnType = hf.returnType;
    // An asm function is an opaque blob of raw x86-64: no params to spill,
    // no basic blocks, no automatic prologue/epilogue. Its instructions are
    // carried verbatim to the code generator, which encodes them directly.
    if (hf.isAsm) {
        lf.isAsm = true;
        lf.asmBody = hf.asmBody;
        for (const auto &[name, type, isVariadic] : hf.params) {
            lf.params.push_back({LirNoReg, type, name});
        }
        return lf;
    }
    CheckedLirBuilder functionBuilder(lf);
    builder = &functionBuilder;
    SetBlock(NewBlock("entry"));
    for (const auto &[name, type, isVariadic] : hf.params) {
        const LirReg pr = builder->DefineParameter();
        if (isVariadic) {
            locals[name] = pr;
            lf.params.push_back({pr, TypeRef::MakePointer(type), name});
        }
        else if (IsInterfaceType(type)) {
            // Interface values are 16-byte fat ptrs; callers pass their
            // address. pr holds that address directly — no extra
            // alloca.
            locals[name] = pr;
            lf.params.push_back({pr, TypeRef::MakePointer(type), name});
        }
        else if (IsSliceType(type)) {
            // Slice values are 16-byte {data, length} structs; callers
            // pass a pointer. pr holds that pointer directly — FieldPtr
            // handles the indirection. Register with Pointer<type> so
            // ResolveFieldOffset can compute field offsets.
            locals[name] = pr;
            lf.params.push_back({pr, TypeRef::MakePointer(type), name});
        }
        else {
            const LirReg slot = EmitAlloca(type);
            EmitStore(pr, slot, type);
            locals[name] = slot;
            lf.params.push_back({pr, type, name});
        }
    }
    if (hf.body) {
        LowerBlock(*hf.body);
        if (!IsTerminated()) {
            Return(std::nullopt, TypeRef::MakeOpaque());
        }
    }
    builder = nullptr;
    return lf;
}

// Block / statement lowering
void HirToLirContext::LowerBlock(const HirBlock &block) {
    for (const auto &stmt : block.stmts) {
        if (IsTerminated()) {
            break;
        }
        LowerStmt(*stmt);
    }
}

} // namespace Rux::HirToLirDetail

namespace Rux {

// Lir public API
HirToLirLowering::HirToLirLowering(HirPackage package, TargetContext target)
    : hir_(std::move(package))
    , target_(target) {
}

LirPackage HirToLirLowering::Generate() {
    HirToLirDetail::HirToLirContext lowering(target_);
    return lowering.Run(hir_);
}
} // namespace Rux
