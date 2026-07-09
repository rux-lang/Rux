// LIR lowering: HirPackage -> LirPackage (control-flow-explicit, SSA-ish).

#include "Lowering/HirToLir/HirToLir.h"

#include "Ir/Hir/Passes/PassManager.h"

#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux {
static LirOpcode BinaryOpcode(TokenKind op) {
    using TK = TokenKind;
    switch (op) {
    case TK::Plus:
        return LirOpcode::Add;
    case TK::Minus:
        return LirOpcode::Sub;
    case TK::Star:
        return LirOpcode::Mul;
    case TK::Slash:
        return LirOpcode::Div;
    case TK::Percent:
        return LirOpcode::Mod;
    case TK::StarStar:
        return LirOpcode::Pow;
    case TK::Amp:
        return LirOpcode::And;
    case TK::Pipe:
        return LirOpcode::Or;
    case TK::Caret:
        return LirOpcode::Xor;
    case TK::LessLess:
        return LirOpcode::Shl;
    case TK::GreaterGreater:
        return LirOpcode::Shr;
    case TK::AmpAmp:
        return LirOpcode::And;
    case TK::PipePipe:
        return LirOpcode::Or;
    case TK::Equal:
        return LirOpcode::CmpEq;
    case TK::BangEqual:
        return LirOpcode::CmpNe;
    case TK::Less:
        return LirOpcode::CmpLt;
    case TK::LessEqual:
        return LirOpcode::CmpLe;
    case TK::Greater:
        return LirOpcode::CmpGt;
    case TK::GreaterEqual:
        return LirOpcode::CmpGe;
    default:
        return LirOpcode::Add;
    }
}

static LirOpcode CompoundOpcode(TokenKind op) {
    using TK = TokenKind;
    switch (op) {
    case TK::PlusAssign:
        return LirOpcode::Add;
    case TK::MinusAssign:
        return LirOpcode::Sub;
    case TK::StarAssign:
        return LirOpcode::Mul;
    case TK::SlashAssign:
        return LirOpcode::Div;
    case TK::PercentAssign:
        return LirOpcode::Mod;
    case TK::AmpAssign:
        return LirOpcode::And;
    case TK::PipeAssign:
        return LirOpcode::Or;
    case TK::CaretAssign:
        return LirOpcode::Xor;
    case TK::LessLessAssign:
        return LirOpcode::Shl;
    case TK::GreaterGreaterAssign:
        return LirOpcode::Shr;
    default:
        return LirOpcode::Add;
    }
}

// Lowering
class LirLowering {
public:
    LirPackage Run(const HirPackage &hir) {
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
        for (const auto &mod : hir.modules) {
            for (const auto &ef : mod.externFuncs) {
                // Extern C functions default to the platform C ABI (SysV on
                // Linux/BSD/macOS, Win64 on Windows) so arguments land in the
                // registers the shared library expects.
                funcConvs[ef.name] = ef.callConv == CallingConvention::Default ? PlatformCConvention() : ef.callConv;
                funcNames.insert(ef.name);
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

private:
    std::unordered_map<std::string, const HirInterface *> interfacesByName;
    LirReg nextReg = 0;
    LirFunc *fn = nullptr;                          // function being built (valid only inside LowerFunc)
    std::uint32_t cur = 0;                          // current basic-block index into fn_->blocks
    std::unordered_map<std::string, LirReg> locals; // name → alloca register
    std::unordered_map<std::string, const HirConst *> globalConsts;

    struct LocalConstValue {
        const HirExpr *value = nullptr;
        TypeRef type;
    };

    std::unordered_map<std::string, LocalConstValue> localConsts;
    std::unordered_map<LirReg, std::vector<LirReg>> enumPayloadSlots;
    std::uint32_t breakTarget = 0;
    std::uint32_t continueTarget = 0;

    struct LabelTargets {
        std::uint32_t breakTarget, continueTarget;
    };

    std::unordered_map<std::string, LabelTargets> labelTargets;
    std::unordered_map<std::string, CallingConvention> funcConvs; // name → calling convention
    std::unordered_set<std::string> funcNames;                    // every function symbol (for address-of)

    // Block / register allocation
    LirReg NewReg() {
        return nextReg++;
    }

    [[nodiscard]] std::uint32_t NewBlock(std::string label = "") const {
        if (label.empty()) {
            label = std::format("bb{}", fn->blocks.size());
        }
        fn->blocks.push_back({std::move(label), {}, std::nullopt});
        return static_cast<std::uint32_t>(fn->blocks.size() - 1);
    }

    void SetBlock(const std::uint32_t idx) {
        cur = idx;
    }

    [[nodiscard]] bool IsTerminated() const {
        return fn->blocks[cur].term.has_value();
    }

    // Instruction emission
    void Emit(LirInstr i) const {
        fn->blocks[cur].instrs.push_back(std::move(i));
    }

    void Terminate(LirTerminator t) const {
        if (!fn->blocks[cur].term.has_value()) {
            fn->blocks[cur].term = std::move(t);
        }
    }

    void Jump(std::uint32_t target) const {
        Terminate(LirTerminator{
            .kind = LirTermKind::Jump, .trueTarget = target, .retVal = std::nullopt, .retType = {}, .cases = {}});
    }

    void Branch(const LirReg cond, const std::uint32_t trueTarget, std::uint32_t falseTarget) const {
        Terminate(LirTerminator{.kind = LirTermKind::Branch,
                                .cond = cond,
                                .trueTarget = trueTarget,
                                .falseTarget = falseTarget,
                                .retVal = std::nullopt,
                                .retType = {},
                                .cases = {}});
    }

    void Return(const std::optional<LirReg> val, TypeRef type) const {
        LirTerminator t;
        t.kind = LirTermKind::Return;
        t.retVal = val;
        t.retType = std::move(type);
        Terminate(std::move(t));
    }

    // Instruction builders

    LirReg EmitConst(std::string value, TypeRef type) {
        LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::Const;
        i.type = std::move(type);
        i.strArg = std::move(value);
        Emit(std::move(i));
        return r;
    }

    LirReg EmitAlloca(TypeRef type) {
        LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::Alloca;
        i.type = std::move(type);
        Emit(std::move(i));
        return r;
    }

    LirReg EmitAlloca(TypeRef type, std::uint64_t count) {
        LirReg r = EmitAlloca(std::move(type));
        fn->blocks[cur].instrs.back().strArg = std::to_string(count);
        return r;
    }

    LirReg EmitLoad(LirReg ptr, TypeRef type) {
        const LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::Load;
        i.type = std::move(type);
        i.srcs = {ptr};
        Emit(std::move(i));
        return r;
    }

    LirReg EmitNamedLoad(std::string name, TypeRef type) {
        LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::Load;
        i.type = std::move(type);
        i.strArg = std::move(name);
        Emit(std::move(i));
        return r;
    }

    void EmitStore(LirReg val, LirReg ptr, TypeRef type) const {
        LirInstr i;
        i.dst = LirNoReg;
        i.op = LirOpcode::Store;
        i.type = std::move(type);
        i.srcs = {val, ptr};
        Emit(std::move(i));
    }

    LirReg EmitBinary(const LirOpcode op, LirReg l, LirReg r, TypeRef type) {
        const LirReg dst = NewReg();
        LirInstr i;
        i.dst = dst;
        i.op = op;
        i.type = std::move(type);
        i.srcs = {l, r};
        Emit(std::move(i));
        return dst;
    }

    LirReg EmitUnary(LirOpcode op, LirReg src, const TypeRef &type) {
        const LirReg dst = NewReg();
        LirInstr i;
        i.dst = dst;
        i.op = op;
        i.type = type;
        i.srcs = {src};
        Emit(std::move(i));
        return dst;
    }

    LirReg EmitCast(LirReg src, const TypeRef &fromType, TypeRef toType) {
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

    LirReg EmitCastIfNeeded(LirReg src, const TypeRef &fromType, const TypeRef &toType) {
        if (src == LirNoReg || fromType.IsUnknown() || toType.IsUnknown() || fromType == toType) {
            return src;
        }
        return EmitCast(src, fromType, toType);
    }

    LirReg EmitFieldPtr(LirReg base, std::string field, const TypeRef &elemType) {
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

    LirReg EmitIndexPtr(LirReg base, LirReg idx, const TypeRef &elemType) {
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
    LirReg EmitGlobalAddr(std::string label, TypeRef pointee = TypeRef::MakeOpaque()) {
        LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::GlobalAddr;
        i.type = TypeRef::MakePointer(std::move(pointee));
        i.strArg = std::move(label);
        Emit(std::move(i));
        return r;
    }

    LirReg EmitStringAddr(std::string value, const TypeRef &elemType) {
        LirReg r = NewReg();
        LirInstr i;
        i.dst = r;
        i.op = LirOpcode::StringAddr;
        i.type = TypeRef::MakePointer(elemType);
        i.strArg = std::move(value);
        Emit(std::move(i));
        return r;
    }

    [[nodiscard]] bool IsInterfaceType(const TypeRef &t) const {
        return t.kind == TypeRef::Kind::Named && interfacesByName.contains(t.name);
    }

    static bool IsSliceType(const TypeRef &type) {
        return type.kind == TypeRef::Kind::Slice ||
               (type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<"));
    }

    static bool IsStringSliceLiteral(const HirLiteralExpr &e) {
        return e.type.kind == TypeRef::Kind::Named &&
               (e.type.name == "Slice<char8>" || e.type.name == "Slice<char16>" || e.type.name == "Slice<char32>");
    }

    static TypeRef StringSliceElementType(const HirLiteralExpr &e) {
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
    LirModule LowerModule(const HirModule &mod) {
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
            lf.name = ef.name;
            lf.dll = ef.dll;
            lf.isPublic = ef.isPublic;
            lf.isExtern = true;
            lf.callConv = ef.callConv;
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
            for (const auto &m : impl.methods) {
                std::string mangledName = impl.typeName + "::" + m.name;
                lm.funcs.push_back(LowerFunc(m, mangledName));
            }
            if (impl.interfaceName) {
                auto ifaceIt = interfacesByName.find(*impl.interfaceName);
                if (ifaceIt != interfacesByName.end()) {
                    LirVtable vt;
                    vt.label = "__vtable__" + impl.typeName + "__" + *impl.interfaceName;
                    for (const auto &m : ifaceIt->second->methods) {
                        vt.methods.push_back(impl.typeName + "::" + m.name);
                    }
                    lm.vtables.push_back(std::move(vt));
                }
            }
        }
        return lm;
    }

    // Render a simple constant expression to a printable string.
    static std::string PrintConstExpr(const HirExpr &e) {
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
    std::optional<std::string> PrintConstElement(const HirExpr &e) const {
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

    // Records the contents of a slice-typed constant so the backend can emit it
    // as data rather than as an expression evaluated at each use.
    void CollectConstContents(const HirConst &c, LirConstDecl &cd) const {
        if (!IsSliceType(c.type)) {
            return;
        }
        if (auto *lit = dynamic_cast<const HirLiteralExpr *>(c.value.get()); lit && IsStringSliceLiteral(*lit)) {
            cd.isTextSlice = true;
            cd.text = lit->value;
            cd.elementType = StringSliceElementType(*lit);
            return;
        }
        if (auto *arr = dynamic_cast<const HirSliceExpr *>(c.value.get())) {
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
        }
    }

    // Function lowering
    LirFunc LowerFunc(const HirFunc &hf, const std::string_view nameOverride = "") {
        nextReg = 0;
        locals.clear();
        localConsts.clear();
        enumPayloadSlots.clear();
        LirFunc lf;
        lf.name = nameOverride.empty() ? hf.name : std::string(nameOverride);
        lf.isPublic = hf.isPublic;
        lf.isExtern = false;
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
        fn = &lf;
        cur = NewBlock("entry");
        for (const auto &[name, type, isVariadic] : hf.params) {
            const LirReg pr = NewReg();
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
        return lf;
    }

    // Block / statement lowering
    void LowerBlock(const HirBlock &block) {
        for (const auto &stmt : block.stmts) {
            if (IsTerminated()) {
                break;
            }
            LowerStmt(*stmt);
        }
    }

    void StoreEnumConstructIntoSlot(const HirEnumConstructExpr &e, const LirReg slot) {
        LirReg tag = EmitConst(e.discriminant, TypeRef::MakeInt64());
        EmitStore(tag, slot, e.type);

        auto &payloadSlots = enumPayloadSlots[slot];
        payloadSlots.clear();
        payloadSlots.reserve(e.payloads.size());
        for (const auto &payloadExpr : e.payloads) {
            const LirReg payloadSlot = EmitAlloca(payloadExpr->type);
            const LirReg payload = LowerExpr(*payloadExpr);
            EmitStore(payload, payloadSlot, payloadExpr->type);
            payloadSlots.push_back(payloadSlot);
        }
    }

    void LowerStmt(const HirStmt &stmt) {
        if (auto *s = dynamic_cast<const HirLetStmt *>(&stmt)) {
            LirReg slot = EmitAlloca(s->type);
            if (!s->pattern) {
                locals[s->name] = slot;
            }
            if (!s->init && s->stackBufferLength != 0) {
                const LirReg data = EmitAlloca(s->stackBufferElementType, s->stackBufferLength);
                const LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(s->stackBufferElementType));
                EmitStore(data, dataField, TypeRef::MakePointer(s->stackBufferElementType));
                const LirReg len = EmitConst(std::to_string(s->stackBufferLength), TypeRef::MakeUInt64());
                const LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
                EmitStore(len, lenField, TypeRef::MakeUInt64());
            }
            if (s->init) {
                StoreExprIntoSlot(*s->init, slot, s->type);
            }
            if (s->pattern) {
                BindLetPattern(*s->pattern, slot, s->type);
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirExprStmt *>(&stmt)) {
            LowerExpr(*s->expr);
            return;
        }

        if (auto *s = dynamic_cast<const HirReturnStmt *>(&stmt)) {
            if (s->value) {
                LirReg val = LowerExpr(**s->value);
                TypeRef retType = fn ? fn->returnType : (*s->value)->type;
                if (val != LirNoReg && !retType.IsUnknown() && (*s->value)->type != retType) {
                    LirReg casted = NewReg();
                    LirInstr cast;
                    cast.dst = casted;
                    cast.op = LirOpcode::Cast;
                    cast.type = retType;
                    cast.srcs = {val};
                    cast.strArg = (*s->value)->type.ToString();
                    Emit(std::move(cast));
                    val = casted;
                }
                Return({val}, retType);
            }
            else {
                Return(std::nullopt, TypeRef::MakeOpaque());
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirBreakStmt *>(&stmt)) {
            if (!s->label.empty()) {
                Jump(labelTargets.at(s->label).breakTarget);
            }
            else {
                Jump(breakTarget);
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirContinueStmt *>(&stmt)) {
            if (!s->label.empty()) {
                Jump(labelTargets.at(s->label).continueTarget);
            }
            else {
                Jump(continueTarget);
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirIfStmt *>(&stmt)) {
            LowerIf(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirWhileStmt *>(&stmt)) {
            LowerWhile(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirDoWhileStmt *>(&stmt)) {
            LowerDoWhile(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirLoopStmt *>(&stmt)) {
            LowerLoop(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirForStmt *>(&stmt)) {
            LowerFor(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirMatchStmt *>(&stmt)) {
            LowerMatch(*s);
            return;
        }

        if (auto *s = dynamic_cast<const HirLocalDecl *>(&stmt)) {
            if (s->hasConstant) {
                localConsts[s->constantName] = {s->constantValue.get(), s->constantType};
            }
            return;
        }
    }

    // Control-flow lowering
    void LowerIf(const HirIfStmt &s) {
        std::uint32_t mergeBlock = NewBlock("if.merge");

        // Pre-allocate blocks for each else-if condition so we know their
        // indices before emitting the branch for the preceding condition.
        std::vector<std::uint32_t> elifCondBlocks;
        elifCondBlocks.reserve(s.elseIfs.size());
        for (std::size_t i = 0; i < s.elseIfs.size(); ++i) {
            elifCondBlocks.push_back(NewBlock(std::format("if.elif{}", i)));
        }
        std::uint32_t elseBlock = s.elseBlock ? NewBlock("if.else") : mergeBlock;
        // Main condition
        const LirReg cond0 = LowerExpr(*s.condition);
        const std::uint32_t thenBb0 = NewBlock("if.then");
        const std::uint32_t fall0 = s.elseIfs.empty() ? elseBlock : elifCondBlocks[0];
        Branch(cond0, thenBb0, fall0);
        SetBlock(thenBb0);
        LowerBlock(s.thenBlock);
        if (!IsTerminated()) {
            Jump(mergeBlock);
        }
        // Else-if chain
        for (std::size_t i = 0; i < s.elseIfs.size(); ++i) {
            SetBlock(elifCondBlocks[i]);
            const LirReg elifCond = LowerExpr(*s.elseIfs[i].condition);
            const std::uint32_t elifThen = NewBlock(std::format("if.elif.then{}", i));
            const std::uint32_t nextFall = (i + 1 < s.elseIfs.size()) ? elifCondBlocks[i + 1] : elseBlock;
            Branch(elifCond, elifThen, nextFall);
            SetBlock(elifThen);
            LowerBlock(s.elseIfs[i].block);
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
        }
        // Else block
        if (s.elseBlock) {
            SetBlock(elseBlock);
            LowerBlock(*s.elseBlock);
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
        }
        SetBlock(mergeBlock);
    }

    void LowerWhile(const HirWhileStmt &s) {
        std::uint32_t condBlock = NewBlock("while.cond");
        std::uint32_t bodyBlock = NewBlock("while.body");
        std::uint32_t afterBlock = NewBlock("while.after");
        if (!IsTerminated()) {
            Jump(condBlock);
        }
        SetBlock(condBlock);
        const LirReg cond = LowerExpr(*s.condition);
        Branch(cond, bodyBlock, afterBlock);
        const std::uint32_t savedBreak = breakTarget;
        const std::uint32_t savedContinue = continueTarget;
        breakTarget = afterBlock;
        continueTarget = condBlock;
        if (!s.label.empty()) {
            labelTargets[s.label] = {afterBlock, condBlock};
        }
        SetBlock(bodyBlock);
        LowerBlock(s.body);
        if (!s.label.empty()) {
            labelTargets.erase(s.label);
        }
        if (!IsTerminated()) {
            Jump(condBlock);
        }
        breakTarget = savedBreak;
        continueTarget = savedContinue;
        SetBlock(afterBlock);
    }

    void LowerDoWhile(const HirDoWhileStmt &s) {
        std::uint32_t bodyBlock = NewBlock("do.body");
        std::uint32_t condBlock = NewBlock("do.cond");
        std::uint32_t afterBlock = NewBlock("do.after");
        if (!IsTerminated()) {
            Jump(bodyBlock);
        }
        SetBlock(bodyBlock);
        const std::uint32_t savedBreak = breakTarget;
        const std::uint32_t savedContinue = continueTarget;
        breakTarget = afterBlock;
        continueTarget = condBlock;
        if (!s.label.empty()) {
            labelTargets[s.label] = {afterBlock, condBlock};
        }
        LowerBlock(s.body);
        if (!s.label.empty()) {
            labelTargets.erase(s.label);
        }
        breakTarget = savedBreak;
        continueTarget = savedContinue;
        if (!IsTerminated()) {
            Jump(condBlock);
        }
        SetBlock(condBlock);
        const LirReg cond = LowerExpr(*s.condition);
        Branch(cond, bodyBlock, afterBlock);
        SetBlock(afterBlock);
    }

    void LowerLoop(const HirLoopStmt &s) {
        std::uint32_t bodyBlock = NewBlock("loop.body");
        std::uint32_t afterBlock = NewBlock("loop.after");
        if (!IsTerminated()) {
            Jump(bodyBlock);
        }
        SetBlock(bodyBlock);
        const std::uint32_t savedBreak = breakTarget;
        const std::uint32_t savedContinue = continueTarget;
        breakTarget = afterBlock;
        continueTarget = bodyBlock;
        if (!s.label.empty()) {
            labelTargets[s.label] = {afterBlock, bodyBlock};
        }
        LowerBlock(s.body);
        if (!s.label.empty()) {
            labelTargets.erase(s.label);
        }
        breakTarget = savedBreak;
        continueTarget = savedContinue;
        if (!IsTerminated()) {
            Jump(bodyBlock);
        }
        SetBlock(afterBlock);
    }

    void LowerFor(const HirForStmt &s) {
        const bool isRange = s.iterable->type.IsRange();
        const TypeRef elemType = (isRange && !s.iterable->type.inner.empty()) ? s.iterable->type.inner[0] : s.varType;

        // When the loop reuses a variable already in scope, its storage is the
        // outer variable's slot, so the loop's mutations persist afterwards.
        // Otherwise the loop gets a fresh slot. In both cases the name is bound
        // to `slot` only *after* the iterable is lowered below, so that a
        // self-referential bound like `for k in k..7` reads the pre-loop value
        // of the outer `k` rather than the induction slot mid-initialization.
        LirReg slot = s.reusesOuterVar ? locals.at(s.variable) : EmitAlloca(s.varType);

        if (isRange) {
            // Get a slot holding the range struct
            LirReg iterSlot;
            std::optional<bool> literalInclusive;
            if (auto *re = dynamic_cast<const HirRangeExpr *>(s.iterable.get())) {
                iterSlot = LowerRange(*re);
                literalInclusive = re->inclusive;
            }
            else {
                iterSlot = LowerLValue(*s.iterable);
            }

            // The iterable has been lowered in the enclosing scope; now bind the
            // loop variable name to its induction slot for the loop body.
            locals[s.variable] = slot;

            // i = range.lo
            LirReg loPtr = EmitFieldPtr(iterSlot, "lo", elemType);
            LirReg loVal = EmitLoad(loPtr, elemType);
            EmitStore(loVal, slot, elemType);

            LirReg hiPtr = EmitFieldPtr(iterSlot, "hi", elemType);
            LirReg inclPtr = EmitFieldPtr(iterSlot, "inclusive", TypeRef::MakeBool());

            std::uint32_t condBlock = NewBlock("for.cond");
            std::uint32_t bodyBlock = NewBlock("for.body");
            std::uint32_t stepBlock = NewBlock("for.step");
            std::uint32_t afterBlock = NewBlock("for.after");

            if (!IsTerminated()) {
                Jump(condBlock);
            }
            SetBlock(condBlock);
            LirReg iVal = EmitLoad(slot, elemType);
            LirReg hiCondVal = EmitLoad(hiPtr, elemType);
            LirReg cond;
            if (literalInclusive.has_value()) {
                cond = EmitBinary(*literalInclusive ? LirOpcode::CmpLe : LirOpcode::CmpLt, iVal, hiCondVal,
                                  TypeRef::MakeBool());
            }
            else {
                // Keep the Range<T> contract explicit: continue while
                // i < hi, or while inclusive is true and i == hi.
                LirReg beforeHi = EmitBinary(LirOpcode::CmpLt, iVal, hiCondVal, TypeRef::MakeBool());
                LirReg atHi = EmitBinary(LirOpcode::CmpEq, iVal, hiCondVal, TypeRef::MakeBool());
                LirReg inclBool = EmitLoad(inclPtr, TypeRef::MakeBool());
                LirReg inclusiveAtHi = EmitBinary(LirOpcode::And, inclBool, atHi, TypeRef::MakeBool());
                cond = EmitBinary(LirOpcode::Or, beforeHi, inclusiveAtHi, TypeRef::MakeBool());
            }
            Branch(cond, bodyBlock, afterBlock);

            std::uint32_t savedBreak = breakTarget;
            std::uint32_t savedContinue = continueTarget;
            breakTarget = afterBlock;
            continueTarget = stepBlock;
            if (!s.label.empty()) {
                labelTargets[s.label] = {afterBlock, stepBlock};
            }

            SetBlock(bodyBlock);
            LowerBlock(s.body);

            if (!IsTerminated()) {
                Jump(stepBlock);
            }
            SetBlock(stepBlock);
            LirReg iCur = EmitLoad(slot, elemType);
            LirReg one = EmitConst("1", elemType);
            LirReg iNext = EmitBinary(LirOpcode::Add, iCur, one, elemType);
            EmitStore(iNext, slot, elemType);
            if (!IsTerminated()) {
                Jump(condBlock);
            }

            if (!s.label.empty()) {
                labelTargets.erase(s.label);
            }
            breakTarget = savedBreak;
            continueTarget = savedContinue;
            SetBlock(afterBlock);
            return;
        }

        if (IsSliceType(s.iterable->type)) {
            const TypeRef dataType = TypeRef::MakePointer(elemType);
            LirReg iterSlot = LowerLValue(*s.iterable);

            // The iterable has been lowered in the enclosing scope; now bind the
            // loop variable name to its induction slot for the loop body.
            locals[s.variable] = slot;

            LirReg dataFieldPtr = EmitFieldPtr(iterSlot, "data", dataType);
            LirReg dataPtr = EmitLoad(dataFieldPtr, dataType);
            LirReg lenFieldPtr = EmitFieldPtr(iterSlot, "length", TypeRef::MakeUInt64());
            LirReg length = EmitLoad(lenFieldPtr, TypeRef::MakeUInt64());

            LirReg idxSlot = EmitAlloca(TypeRef::MakeUInt64());
            LirReg zero = EmitConst("0", TypeRef::MakeUInt64());
            EmitStore(zero, idxSlot, TypeRef::MakeUInt64());

            std::uint32_t condBlock = NewBlock("for.cond");
            std::uint32_t bodyBlock = NewBlock("for.body");
            std::uint32_t stepBlock = NewBlock("for.step");
            std::uint32_t afterBlock = NewBlock("for.after");

            if (!IsTerminated()) {
                Jump(condBlock);
            }
            SetBlock(condBlock);
            LirReg idx = EmitLoad(idxSlot, TypeRef::MakeUInt64());
            LirReg cond = EmitBinary(LirOpcode::CmpLt, idx, length, TypeRef::MakeBool());
            Branch(cond, bodyBlock, afterBlock);

            std::uint32_t savedBreak = breakTarget;
            std::uint32_t savedContinue = continueTarget;
            breakTarget = afterBlock;
            continueTarget = stepBlock;
            if (!s.label.empty()) {
                labelTargets[s.label] = {afterBlock, stepBlock};
            }

            SetBlock(bodyBlock);
            LirReg elemPtr = EmitIndexPtr(dataPtr, idx, elemType);
            LirReg elemVal = EmitLoad(elemPtr, elemType);
            EmitStore(elemVal, slot, elemType);
            LowerBlock(s.body);

            if (!IsTerminated()) {
                Jump(stepBlock);
            }
            SetBlock(stepBlock);
            LirReg idxCur = EmitLoad(idxSlot, TypeRef::MakeUInt64());
            LirReg one = EmitConst("1", TypeRef::MakeUInt64());
            LirReg idxNext = EmitBinary(LirOpcode::Add, idxCur, one, TypeRef::MakeUInt64());
            EmitStore(idxNext, idxSlot, TypeRef::MakeUInt64());
            if (!IsTerminated()) {
                Jump(condBlock);
            }

            if (!s.label.empty()) {
                labelTargets.erase(s.label);
            }
            breakTarget = savedBreak;
            continueTarget = savedContinue;
            SetBlock(afterBlock);
            return;
        }
    }

    void LowerMatch(const HirMatchStmt &s) {
        const LirReg subjectVal = LowerExpr(*s.subject);
        const std::vector<LirReg> *subjectPayload = nullptr;
        if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(s.subject.get())) {
            if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
                if (const auto payloadIt = enumPayloadSlots.find(localIt->second);
                    payloadIt != enumPayloadSlots.end()) {
                    subjectPayload = &payloadIt->second;
                }
            }
        }
        const std::uint32_t mergeBlock = NewBlock("match.merge");
        if (s.arms.empty()) {
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
            SetBlock(mergeBlock);
            return;
        }
        for (std::size_t i = 0; i < s.arms.size(); ++i) {
            const auto &arm = s.arms[i];
            const bool isLast = (i + 1 == s.arms.size());
            std::uint32_t bodyBlock = NewBlock(std::format("match.arm{}", i));
            std::uint32_t nextBlock = isLast ? mergeBlock : NewBlock(std::format("match.next{}", i));
            LirReg matched = LowerPattern(*arm.pattern, subjectVal, s.subject->type, subjectPayload);
            Branch(matched, bodyBlock, nextBlock);
            SetBlock(bodyBlock);
            LowerExpr(*arm.body);
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
            if (!isLast) {
                SetBlock(nextBlock);
            }
        }
        SetBlock(mergeBlock);
    }

    // Pattern lowering
    // Returns a bool register: 1 if the pattern matches `subjectVal`.
    // Side-effects: binds pattern variables into locals
    void BindLetPattern(const HirPattern &pat, LirReg subjectPtr, const TypeRef &subjectType) {
        if (dynamic_cast<const HirWildcardPattern *>(&pat)) {
            return;
        }

        if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat)) {
            const TypeRef bindType = p->type.IsUnknown() ? subjectType : p->type;
            LirReg bindSlot = EmitAlloca(bindType);
            locals[p->name] = bindSlot;
            LirReg val = EmitLoad(subjectPtr, bindType);
            EmitStore(val, bindSlot, bindType);
            return;
        }

        if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
            for (std::size_t i = 0; i < p->elements.size(); ++i) {
                TypeRef elemType = TypeRef::MakeUnknown();
                if (subjectType.kind == TypeRef::Kind::Tuple && i < subjectType.inner.size()) {
                    elemType = subjectType.inner[i];
                }
                LirReg elemPtr = EmitFieldPtr(subjectPtr, std::to_string(i), elemType);
                BindLetPattern(*p->elements[i], elemPtr, elemType);
            }
        }
    }

    LirReg LowerPattern(const HirPattern &pat, LirReg subjectVal, const TypeRef &subjectType,
                        const std::vector<LirReg> *enumPayload = nullptr) {
        if (dynamic_cast<const HirWildcardPattern *>(&pat)) {
            return EmitConst("1", TypeRef::MakeBool());
        }
        if (auto *p = dynamic_cast<const HirLiteralPattern *>(&pat)) {
            LirReg lit = EmitConst(p->value, p->type);
            return EmitBinary(LirOpcode::CmpEq, subjectVal, lit, TypeRef::MakeBool());
        }
        if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat)) {
            LirReg bindSlot = EmitAlloca(p->type);
            locals[p->name] = bindSlot;
            EmitStore(subjectVal, bindSlot, p->type);
            return EmitConst("1", TypeRef::MakeBool());
        }
        if (auto *p = dynamic_cast<const HirRangePattern *>(&pat)) {
            LirReg lo = LirNoReg, hi = LirNoReg;
            if (auto *lit = dynamic_cast<const HirLiteralPattern *>(p->lo.get())) {
                lo = EmitConst(lit->value, subjectType);
            }
            else {
                lo = EmitConst("0", subjectType);
            }
            if (auto *lit = dynamic_cast<const HirLiteralPattern *>(p->hi.get())) {
                hi = EmitConst(lit->value, subjectType);
            }
            else {
                hi = EmitConst("0", subjectType);
            }
            const LirReg cmpLo = EmitBinary(LirOpcode::CmpLe, lo, subjectVal, TypeRef::MakeBool());
            const LirOpcode hiOp = p->inclusive ? LirOpcode::CmpLe : LirOpcode::CmpLt;
            const LirReg cmpHi = EmitBinary(hiOp, subjectVal, hi, TypeRef::MakeBool());
            return EmitBinary(LirOpcode::And, cmpLo, cmpHi, TypeRef::MakeBool());
        }

        // Enum, struct, tuple patterns: lower payload bindings, then emit a
        // placeholder true. Full structural matching requires runtime
        // support beyond what this IR stage provides.
        if (auto *p = dynamic_cast<const HirEnumPattern *>(&pat)) {
            LirReg tagValue = subjectVal;
            if (!p->unitDiscriminants.empty() || p->discriminant) {
                LirReg mask = EmitConst("4294967295", TypeRef::MakeInt64());
                tagValue = EmitBinary(LirOpcode::And, subjectVal, mask, TypeRef::MakeInt64());
            }
            for (std::size_t i = 0; i < p->args.size(); ++i) {
                const auto &arg = p->args[i];
                if (auto *bp = dynamic_cast<const HirBindingPattern *>(arg.get())) {
                    const TypeRef bindType = bp->type.IsUnknown() ? subjectType : bp->type;
                    const LirReg bindSlot = EmitAlloca(bindType);
                    locals[bp->name] = bindSlot;
                    LirReg payload = LirNoReg;
                    const std::size_t payloadIndex = i < p->argIndices.size() ? p->argIndices[i] : i;
                    if (enumPayload && payloadIndex < enumPayload->size()) {
                        payload = EmitLoad((*enumPayload)[payloadIndex], bindType);
                    }
                    else {
                        LirReg shift = EmitConst("32", TypeRef::MakeInt64());
                        payload = EmitBinary(LirOpcode::Shr, subjectVal, shift, TypeRef::MakeInt64());
                    }
                    EmitStore(payload, bindSlot, bindType);
                }
            }
            if (p->hasPayload) {
                if (p->discriminant) {
                    LirReg lit = EmitConst(*p->discriminant, TypeRef::MakeInt64());
                    return EmitBinary(LirOpcode::CmpEq, tagValue, lit, TypeRef::MakeBool());
                }
                return EmitConst("1", TypeRef::MakeBool());
            }
            if (p->discriminant) {
                LirReg lit = EmitConst(*p->discriminant, TypeRef::MakeInt64());
                return EmitBinary(LirOpcode::CmpEq, tagValue, lit, TypeRef::MakeBool());
            }
            return EmitConst("1", TypeRef::MakeBool());
        }

        if (auto *p = dynamic_cast<const HirStructPattern *>(&pat)) {
            for (const auto &f : p->fields) {
                if (auto *bp = dynamic_cast<const HirBindingPattern *>(f.pattern.get())) {
                    LirReg bindSlot = EmitAlloca(bp->type);
                    locals[bp->name] = bindSlot;
                }
            }
            return EmitConst("1", TypeRef::MakeBool());
        }

        if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
            for (const auto &elem : p->elements) {
                if (auto *bp = dynamic_cast<const HirBindingPattern *>(elem.get())) {
                    LirReg bindSlot = EmitAlloca(bp->type);
                    locals[bp->name] = bindSlot;
                }
            }
            return EmitConst("1", TypeRef::MakeBool());
        }

        if (auto *p = dynamic_cast<const HirGuardedPattern *>(&pat)) {
            LirReg inner = LowerPattern(*p->inner, subjectVal, subjectType);
            LirReg guard = LowerExpr(*p->guard);
            return EmitBinary(LirOpcode::And, inner, guard, TypeRef::MakeBool());
        }

        return EmitConst("1", TypeRef::MakeBool()); // wildcard fallback
    }

    // Expression lowering
    // Returns the register holding the expression's value.
    // For void expressions the return value is LirNoReg.

    LirReg LowerExpr(const HirExpr &expr) {
        if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr)) {
            if (IsStringSliceLiteral(*e)) {
                return LowerStringLiteralSlice(*e);
            }
            return EmitConst(e->value, e->type);
        }
        if (auto *e = dynamic_cast<const HirVarExpr *>(&expr)) {
            if (const auto it = localConsts.find(e->name); it != localConsts.end()) {
                const TypeRef &constType = it->second.type.IsUnknown() ? e->type : it->second.type;
                LirReg value = LowerExpr(*it->second.value);
                return EmitCastIfNeeded(value, it->second.value->type, constType);
            }
            if (const auto it = globalConsts.find(e->name); it != globalConsts.end()) {
                LirReg value = LowerExpr(*it->second->value);
                return EmitCastIfNeeded(value, it->second->value->type, it->second->type);
            }
            auto it = locals.find(e->name);
            if (it != locals.end()) {
                if (IsInterfaceType(e->type)) {
                    return it->second; // fat-ptr address lives in the slot
                }
                return EmitLoad(it->second, e->type);
            }
            // A function referenced by name (not called) evaluates to its
            // address, i.e. a function pointer.
            if (funcNames.contains(e->name)) {
                return EmitGlobalAddr(e->name);
            }
            return EmitNamedLoad(e->name, e->type);
        }
        if (dynamic_cast<const HirSelfExpr *>(&expr)) {
            auto it = locals.find("self");
            if (it != locals.end()) {
                if (IsInterfaceType(expr.type)) {
                    return it->second;
                }
                return EmitLoad(it->second, expr.type);
            }
            return EmitNamedLoad("self", expr.type);
        }
        if (auto *e = dynamic_cast<const HirPathExpr *>(&expr)) {
            std::string path;
            for (std::size_t i = 0; i < e->segments.size(); ++i) {
                if (i) {
                    path += "::";
                }
                path += e->segments[i];
            }
            if (const auto it = globalConsts.find(path); it != globalConsts.end()) {
                LirReg value = LowerExpr(*it->second->value);
                return EmitCastIfNeeded(value, it->second->value->type, it->second->type);
            }
            // Module-qualified function referenced by name → its address. The
            // binary symbol is the final path segment (e.g. Math::Add → "Add").
            if (funcNames.contains(e->segments.back())) {
                return EmitGlobalAddr(e->segments.back());
            }
            return EmitNamedLoad(path, e->type);
        }
        if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr)) {
            return LowerUnary(*e);
        }
        if (auto *e = dynamic_cast<const HirPostfixExpr *>(&expr)) {
            return LowerPostfix(*e);
        }
        if (auto *e = dynamic_cast<const HirBinaryExpr *>(&expr)) {
            return LowerBinary(*e);
        }
        if (auto *e = dynamic_cast<const HirAssignExpr *>(&expr)) {
            return LowerAssign(*e);
        }
        if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr)) {
            return LowerTernary(*e);
        }
        if (auto *e = dynamic_cast<const HirMatchExpr *>(&expr)) {
            return LowerMatchExpr(*e);
        }
        if (auto *e = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
            return LowerEnumConstruct(*e);
        }
        if (auto *e = dynamic_cast<const HirCallExpr *>(&expr)) {
            return LowerCall(*e);
        }
        if (auto *e = dynamic_cast<const HirCoerceToInterfaceExpr *>(&expr)) {
            return LowerCoerceToInterface(*e);
        }
        if (auto *e = dynamic_cast<const HirInterfaceCallExpr *>(&expr)) {
            return LowerInterfaceCall(*e);
        }
        if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr)) {
            LirReg idx = LowerExpr(*e->index);
            LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
            LirReg ptr = EmitIndexPtr(sliceBase, idx, e->type);
            if (IsInterfaceType(e->type)) {
                return ptr;
            }
            return EmitLoad(ptr, e->type);
        }
        if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr)) {
            LirReg base =
                e->object->type.kind == TypeRef::Kind::Pointer ? LowerExpr(*e->object) : LowerLValue(*e->object);
            LirReg ptr = EmitFieldPtr(base, e->field, e->type);
            return EmitLoad(ptr, e->type);
        }
        if (auto *e = dynamic_cast<const HirStructInitExpr *>(&expr)) {
            return LowerStructInit(*e);
        }
        if (auto *e = dynamic_cast<const HirSliceExpr *>(&expr)) {
            return LowerSlice(*e);
        }
        if (auto *e = dynamic_cast<const HirTupleExpr *>(&expr)) {
            return LowerTuple(*e);
        }
        if (auto *e = dynamic_cast<const HirCastExpr *>(&expr)) {
            return EmitCast(LowerExpr(*e->operand), e->operand->type, e->type);
        }
        if (auto *e = dynamic_cast<const HirIsExpr *>(&expr)) {
            // Should only be reached for interface types (rejected by
            // sema). Return false as a safe fallback.
            LowerExpr(*e->operand);
            return EmitConst("false", TypeRef::MakeBool());
        }
        if (auto *e = dynamic_cast<const HirBlockExpr *>(&expr)) {
            LowerBlock(e->block);
            return EmitConst("0", e->type);
        }
        if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
            return LowerRange(*e);
        }
        return EmitConst("0", expr.type);
    }

    LirReg LowerPostfix(const HirPostfixExpr &e) {
        const LirReg ptr = LowerLValue(*e.operand);
        const LirReg old_val = EmitLoad(ptr, e.type);
        LirReg delta = EmitConst("1", e.type);
        if (e.type.kind == TypeRef::Kind::Pointer && !e.type.inner.empty()) {
            const auto elemSize = e.type.inner[0].SizeInBytes();
            if (elemSize && *elemSize > 1) {
                const LirReg sz = EmitConst(std::to_string(*elemSize), e.type);
                delta = EmitBinary(LirOpcode::Mul, delta, sz, e.type);
            }
        }
        const LirOpcode op = (e.op == TokenKind::PlusPlus) ? LirOpcode::Add : LirOpcode::Sub;
        const LirReg new_val = EmitBinary(op, old_val, delta, e.type);
        EmitStore(new_val, ptr, e.type);
        return old_val;
    }

    LirReg LowerUnary(const HirUnaryExpr &e) {
        using TK = TokenKind;
        switch (e.op) {
        case TK::Minus:
            return EmitUnary(LirOpcode::Neg, LowerExpr(*e.operand), e.type);
        case TK::Bang:
            return EmitUnary(LirOpcode::Not, LowerExpr(*e.operand), e.type);
        case TK::Tilde:
            return EmitUnary(LirOpcode::BitNot, LowerExpr(*e.operand), e.type);
        case TK::Star: {
            // Dereference: the operand evaluates to a pointer; load through
            // it.
            LirReg ptr = LowerExpr(*e.operand);
            return EmitLoad(ptr, e.type);
        }
        case TK::Amp: {
            // Address-of: return the alloca slot for named locals,
            // otherwise materialize a temporary.
            if (auto *v = dynamic_cast<const HirVarExpr *>(e.operand.get())) {
                if (const auto it = locals.find(v->name); it != locals.end()) {
                    return it->second;
                }
            }
            // Non-addressable: evaluate into a temp slot.
            const LirReg val = LowerExpr(*e.operand);
            const LirReg slot = EmitAlloca(e.operand->type);
            EmitStore(val, slot, e.operand->type);
            return slot;
        }
        case TK::PlusPlus:
        case TK::MinusMinus: {
            const LirReg ptr = LowerLValue(*e.operand);
            const LirReg old_val = EmitLoad(ptr, e.type);
            LirReg delta = EmitConst("1", e.type);
            if (e.type.kind == TypeRef::Kind::Pointer && !e.type.inner.empty()) {
                const auto elemSize = e.type.inner[0].SizeInBytes();
                if (elemSize && *elemSize > 1) {
                    const LirReg sz = EmitConst(std::to_string(*elemSize), e.type);
                    delta = EmitBinary(LirOpcode::Mul, delta, sz, e.type);
                }
            }
            const LirOpcode aop = (e.op == TK::PlusPlus) ? LirOpcode::Add : LirOpcode::Sub;
            const LirReg new_val = EmitBinary(aop, old_val, delta, e.type);
            EmitStore(new_val, ptr, e.type);
            return new_val;
        }
        default:
            return EmitUnary(LirOpcode::Not, LowerExpr(*e.operand), e.type);
        }
    }

    LirReg LowerBinary(const HirBinaryExpr &e) {
        using TK = TokenKind;
        // Short-circuit operators: branch to avoid evaluating the
        // right-hand side.
        if (e.op == TK::AmpAmp || e.op == TK::PipePipe) {
            LirReg lhs = LowerExpr(*e.left);
            std::uint32_t rhsBlock = NewBlock(e.op == TK::AmpAmp ? "land.rhs" : "lor.rhs");
            std::uint32_t shortBlock = NewBlock(e.op == TK::AmpAmp ? "land.short" : "lor.short");
            std::uint32_t mergeBlock = NewBlock(e.op == TK::AmpAmp ? "land.merge" : "lor.merge");
            if (e.op == TK::AmpAmp) {
                Branch(lhs, rhsBlock, shortBlock); // false → skip rhs
            }
            else {
                Branch(lhs, shortBlock, rhsBlock); // true  → skip rhs
            }
            // Short-circuit path: result is the known constant.
            SetBlock(shortBlock);
            LirReg shortVal = EmitConst(e.op == TK::AmpAmp ? "false" : "true", TypeRef::MakeBool());
            Jump(mergeBlock);
            std::uint32_t shortBlockIdx = shortBlock;
            // Right-hand side path.
            SetBlock(rhsBlock);
            LirReg rhs = LowerExpr(*e.right);
            Jump(mergeBlock);
            std::uint32_t rhsBlockIdx = rhsBlock;
            // Join with a phi.
            SetBlock(mergeBlock);
            LirReg result = NewReg();
            LirInstr phi;
            phi.dst = result;
            phi.op = LirOpcode::Phi;
            phi.type = TypeRef::MakeBool();
            phi.phiPreds = {{shortVal, shortBlockIdx}, {rhs, rhsBlockIdx}};
            Emit(std::move(phi));
            return result;
        }
        const LirReg lhs = LowerExpr(*e.left);
        const LirReg rhs = LowerExpr(*e.right);

        // Scale integer operand by element size for pointer arithmetic.
        if ((e.op == TK::Plus || e.op == TK::Minus) && e.type.kind == TypeRef::Kind::Pointer && !e.type.inner.empty()) {
            const auto elemSize = e.type.inner[0].SizeInBytes();
            if (elemSize && *elemSize > 1) {
                if (e.left->type.kind == TypeRef::Kind::Pointer) {
                    // ptr + int or ptr - int: scale the right (integer)
                    // operand
                    const LirReg sz = EmitConst(std::to_string(*elemSize), e.right->type);
                    const LirReg scaled = EmitBinary(LirOpcode::Mul, rhs, sz, e.right->type);
                    return EmitBinary(BinaryOpcode(e.op), lhs, scaled, e.type);
                }
                else {
                    // int + ptr: scale the left (integer) operand
                    const LirReg sz = EmitConst(std::to_string(*elemSize), e.left->type);
                    const LirReg scaled = EmitBinary(LirOpcode::Mul, lhs, sz, e.left->type);
                    return EmitBinary(BinaryOpcode(e.op), scaled, rhs, e.type);
                }
            }
        }

        return EmitBinary(BinaryOpcode(e.op), lhs, rhs, e.type);
    }

    LirReg LowerAssign(const HirAssignExpr &e) {
        if (e.op == TokenKind::Assign && (IsInterfaceType(e.type) || IsSliceType(e.type))) {
            const LirReg ptr = LowerLValue(*e.target);
            StoreExprIntoSlot(*e.value, ptr, e.type);
            return ptr;
        }

        LirReg val = LowerExpr(*e.value);
        if (e.op != TokenKind::Assign) {
            // Compound assignment: load current value, compute, then store.
            const LirReg current = LowerExpr(*e.target);
            if (e.type.kind == TypeRef::Kind::Pointer && !e.type.inner.empty()) {
                const auto elemSize = e.type.inner[0].SizeInBytes();
                if (elemSize && *elemSize > 1) {
                    const LirReg sz = EmitConst(std::to_string(*elemSize), e.type);
                    val = EmitBinary(LirOpcode::Mul, val, sz, e.type);
                }
            }
            val = EmitBinary(CompoundOpcode(e.op), current, val, e.type);
        }
        else {
            val = EmitCastIfNeeded(val, e.value->type, e.type);
        }
        const LirReg ptr = LowerLValue(*e.target);
        EmitStore(val, ptr, e.type);
        return val;
    }

    static TypeRef SliceElementTypeFromType(const TypeRef &type) {
        if (type.kind == TypeRef::Kind::Slice && !type.inner.empty()) {
            return type.inner[0];
        }
        if (type.kind == TypeRef::Kind::Named) {
            if (type.name == "Slice<char16>") {
                return TypeRef::MakeChar16();
            }
            if (type.name == "Slice<char32>") {
                return TypeRef::MakeChar32();
            }
            constexpr std::string_view prefix = "Slice<";
            if (type.name.starts_with(prefix) && type.name.ends_with(">")) {
                const std::string elemName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
                if (elemName == "bool" || elemName == "bool8") {
                    return TypeRef::MakeBool();
                }
                if (elemName == "char8") {
                    return TypeRef::MakeChar8();
                }
                if (elemName == "char16") {
                    return TypeRef::MakeChar16();
                }
                if (elemName == "char32" || elemName == "char") {
                    return TypeRef::MakeChar();
                }
                if (elemName == "int8") {
                    return TypeRef::MakeInt8();
                }
                if (elemName == "int16") {
                    return TypeRef::MakeInt16();
                }
                if (elemName == "int32") {
                    return TypeRef::MakeInt32();
                }
                if (elemName == "int64") {
                    return TypeRef::MakeInt64();
                }
                if (elemName == "int") {
                    return TypeRef::MakeInt();
                }
                if (elemName == "uint8") {
                    return TypeRef::MakeUInt8();
                }
                if (elemName == "uint16") {
                    return TypeRef::MakeUInt16();
                }
                if (elemName == "uint32") {
                    return TypeRef::MakeUInt32();
                }
                if (elemName == "uint64") {
                    return TypeRef::MakeUInt64();
                }
                if (elemName == "uint") {
                    return TypeRef::MakeUInt();
                }
                if (elemName == "float32") {
                    return TypeRef::MakeFloat32();
                }
                if (elemName == "float64") {
                    return TypeRef::MakeFloat64();
                }
                return TypeRef::MakeNamed(elemName);
            }
        }
        return TypeRef::MakeChar8();
    }

    void CopySliceValue(LirReg srcSlot, LirReg dstSlot, const TypeRef &sliceType) {
        const TypeRef elemType = SliceElementTypeFromType(sliceType);
        const TypeRef dataType = TypeRef::MakePointer(elemType);

        const LirReg srcDataPtr = EmitFieldPtr(srcSlot, "data", dataType);
        const LirReg data = EmitLoad(srcDataPtr, dataType);
        const LirReg dstDataPtr = EmitFieldPtr(dstSlot, "data", dataType);
        EmitStore(data, dstDataPtr, dataType);

        const LirReg srcLenPtr = EmitFieldPtr(srcSlot, "length", TypeRef::MakeUInt64());
        const LirReg len = EmitLoad(srcLenPtr, TypeRef::MakeUInt64());
        const LirReg dstLenPtr = EmitFieldPtr(dstSlot, "length", TypeRef::MakeUInt64());
        EmitStore(len, dstLenPtr, TypeRef::MakeUInt64());
    }

    void StoreTernaryInit(const HirTernaryExpr &e, LirReg slot, const TypeRef &type) {
        LirReg cond = LowerExpr(*e.condition);
        const std::uint32_t thenBlock = NewBlock("ternary.store.then");
        const std::uint32_t elseBlock = NewBlock("ternary.store.else");
        const std::uint32_t mergeBlock = NewBlock("ternary.store.merge");
        Branch(cond, thenBlock, elseBlock);

        SetBlock(thenBlock);
        StoreExprIntoSlot(*e.thenExpr, slot, type);
        Jump(mergeBlock);

        SetBlock(elseBlock);
        StoreExprIntoSlot(*e.elseExpr, slot, type);
        Jump(mergeBlock);

        SetBlock(mergeBlock);
    }

    void StoreExprIntoSlot(const HirExpr &expr, LirReg slot, const TypeRef &type) {
        if (auto *init = dynamic_cast<const HirStructInitExpr *>(&expr)) {
            StoreStructInit(*init, slot);
            return;
        }
        if (auto *initSliceExpr = dynamic_cast<const HirSliceExpr *>(&expr)) {
            StoreSliceInit(*initSliceExpr, slot);
            return;
        }
        if (auto *initTupleExpr = dynamic_cast<const HirTupleExpr *>(&expr)) {
            StoreTupleInit(*initTupleExpr, slot);
            return;
        }
        if (auto *initRangeExpr = dynamic_cast<const HirRangeExpr *>(&expr)) {
            StoreRangeInit(*initRangeExpr, slot);
            return;
        }
        if (auto *initTernaryExpr = dynamic_cast<const HirTernaryExpr *>(&expr)) {
            StoreTernaryInit(*initTernaryExpr, slot, type);
            return;
        }
        if (auto *initMatchExpr = dynamic_cast<const HirMatchExpr *>(&expr)) {
            StoreMatchInit(*initMatchExpr, slot, type);
            return;
        }
        if (auto *initEnumExpr = dynamic_cast<const HirEnumConstructExpr *>(&expr)) {
            StoreEnumConstructIntoSlot(*initEnumExpr, slot);
            return;
        }
        if (auto *initLitExpr = dynamic_cast<const HirLiteralExpr *>(&expr);
            initLitExpr && IsStringSliceLiteral(*initLitExpr)) {
            StoreStringLiteralSlice(*initLitExpr, slot);
            return;
        }
        if (IsSliceType(type)) {
            const LirReg src = LowerLValue(expr);
            CopySliceValue(src, slot, type);
            return;
        }

        if (auto *coerce = dynamic_cast<const HirCoerceToInterfaceExpr *>(&expr)) {
            StoreCoerceToInterface(*coerce, slot);
            return;
        }

        if (IsInterfaceType(type)) {
            // Copy the 16-byte fat pointer {data, vtable} field by field.
            const LirReg srcBase = LowerExpr(expr); // returns fat-ptr address
            const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());
            LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
            LirReg srcData = EmitIndexPtr(srcBase, i0, TypeRef::MakeUInt64());
            LirReg dataVal = EmitLoad(srcData, ptrType);
            LirReg dstData = EmitIndexPtr(slot, i0, TypeRef::MakeUInt64());
            EmitStore(dataVal, dstData, ptrType);
            LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
            LirReg srcVtbl = EmitIndexPtr(srcBase, i1, TypeRef::MakeUInt64());
            LirReg vtblVal = EmitLoad(srcVtbl, ptrType);
            LirReg dstVtbl = EmitIndexPtr(slot, i1, TypeRef::MakeUInt64());
            EmitStore(vtblVal, dstVtbl, ptrType);
            return;
        }

        const LirReg val = LowerExpr(expr);
        EmitStore(EmitCastIfNeeded(val, expr.type, type), slot, type);
    }

    LirReg LowerTernary(const HirTernaryExpr &e) {
        LirReg cond = LowerExpr(*e.condition);
        const std::uint32_t thenBlock = NewBlock("ternary.then");
        const std::uint32_t elseBlock = NewBlock("ternary.else");
        const std::uint32_t mergeBlock = NewBlock("ternary.merge");
        Branch(cond, thenBlock, elseBlock);
        SetBlock(thenBlock);
        LirReg thenVal = LowerExpr(*e.thenExpr);
        Jump(mergeBlock);
        std::uint32_t thenIdx = thenBlock;
        SetBlock(elseBlock);
        LirReg elseVal = LowerExpr(*e.elseExpr);
        Jump(mergeBlock);
        std::uint32_t elseIdx = elseBlock;
        SetBlock(mergeBlock);
        LirReg result = NewReg();
        LirInstr phi;
        phi.dst = result;
        phi.op = LirOpcode::Phi;
        phi.type = e.type;
        phi.phiPreds = {{thenVal, thenIdx}, {elseVal, elseIdx}};
        Emit(std::move(phi));
        return result;
    }

    void StoreMatchInit(const HirMatchExpr &e, LirReg slot, const TypeRef &type) {
        const LirReg subjectVal = LowerExpr(*e.subject);
        const std::vector<LirReg> *subjectPayload = nullptr;
        if (auto *subjectVar = dynamic_cast<const HirVarExpr *>(e.subject.get())) {
            if (const auto localIt = locals.find(subjectVar->name); localIt != locals.end()) {
                if (const auto payloadIt = enumPayloadSlots.find(localIt->second);
                    payloadIt != enumPayloadSlots.end()) {
                    subjectPayload = &payloadIt->second;
                }
            }
        }
        const std::uint32_t mergeBlock = NewBlock("match.expr.store.merge");
        if (e.arms.empty()) {
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
            SetBlock(mergeBlock);
            return;
        }

        for (std::size_t i = 0; i < e.arms.size(); ++i) {
            const auto &arm = e.arms[i];
            const bool isLast = (i + 1 == e.arms.size());
            const std::uint32_t bodyBlock = NewBlock(std::format("match.expr.store.arm{}", i));
            const std::uint32_t nextBlock = isLast ? mergeBlock : NewBlock(std::format("match.expr.store.next{}", i));
            const LirReg matched = LowerPattern(*arm.pattern, subjectVal, e.subject->type, subjectPayload);
            Branch(matched, bodyBlock, nextBlock);
            SetBlock(bodyBlock);
            StoreExprIntoSlot(*arm.body, slot, type);
            if (!IsTerminated()) {
                Jump(mergeBlock);
            }
            if (!isLast) {
                SetBlock(nextBlock);
            }
        }

        SetBlock(mergeBlock);
    }

    LirReg LowerMatchExpr(const HirMatchExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreMatchInit(e, slot, e.type);
        return EmitLoad(slot, e.type);
    }

    // Fill an existing 16-byte fat-pointer slot with {&concrete, &vtable}.
    void StoreCoerceToInterface(const HirCoerceToInterfaceExpr &e, LirReg slot) {
        LirReg val = LowerExpr(*e.value);
        LirReg concreteSlot = EmitAlloca(e.value->type);
        EmitStore(val, concreteSlot, e.value->type);

        const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());
        LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
        LirReg dataField = EmitIndexPtr(slot, i0, TypeRef::MakeUInt64());
        EmitStore(concreteSlot, dataField, ptrType);

        LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
        LirReg vtblField = EmitIndexPtr(slot, i1, TypeRef::MakeUInt64());
        if (!e.vtableLabel.empty()) {
            LirReg vtblAddr = EmitGlobalAddr(e.vtableLabel);
            EmitStore(vtblAddr, vtblField, ptrType);
        }
        else {
            LirReg zero = EmitConst("0", TypeRef::MakeUInt64());
            EmitStore(zero, vtblField, ptrType);
        }
    }

    // Wrap a concrete value into a {data_ptr, vtable_ptr} fat pointer.
    // Returns the alloca slot whose data region IS the 16-byte fat pointer.
    LirReg LowerCoerceToInterface(const HirCoerceToInterfaceExpr &e) {
        LirReg slot = EmitAlloca(e.type); // Named("X") → 16-byte data region
        StoreCoerceToInterface(e, slot);
        return slot;
    }

    // Call a method through an interface fat pointer via vtable dispatch.
    LirReg LowerInterfaceCall(const HirInterfaceCallExpr &e) {
        const TypeRef ptrType = TypeRef::MakePointer(TypeRef::MakeOpaque());

        // fat_ptr_addr = the 8-byte value that IS the fat ptr address
        LirReg fatPtrAddr = LowerExpr(*e.fatPtrExpr);

        // Load data_ptr = fat_ptr[0]
        LirReg i0 = EmitConst("0", TypeRef::MakeUInt64());
        LirReg dataField = EmitIndexPtr(fatPtrAddr, i0, TypeRef::MakeUInt64());
        LirReg dataPtr = EmitLoad(dataField, ptrType);

        // Load vtbl_ptr = fat_ptr[1]
        LirReg i1 = EmitConst("1", TypeRef::MakeUInt64());
        LirReg vtblField = EmitIndexPtr(fatPtrAddr, i1, TypeRef::MakeUInt64());
        LirReg vtblPtr = EmitLoad(vtblField, ptrType);

        // Load fn_ptr = vtbl_ptr[methodIdx]
        LirReg midx = EmitConst(std::to_string(e.methodIdx), TypeRef::MakeUInt64());
        LirReg fnSlot = EmitIndexPtr(vtblPtr, midx, TypeRef::MakeUInt64());
        LirReg fnPtr = EmitLoad(fnSlot, ptrType);

        // CallIndirect(fn_ptr, data_ptr, args...)
        const LirReg dst = e.type.IsOpaque() ? LirNoReg : NewReg();
        LirInstr ci;
        ci.dst = dst;
        ci.type = e.type;
        ci.op = LirOpcode::CallIndirect;
        ci.callConv = CallingConvention::Default;
        ci.srcs = {fnPtr, dataPtr};
        for (const auto &arg : e.args) {
            ci.srcs.push_back(LowerExpr(*arg));
        }
        Emit(std::move(ci));
        return dst;
    }

    LirReg LowerEnumConstruct(const HirEnumConstructExpr &e) {
        if (e.payloads.empty()) {
            return EmitConst(e.discriminant, TypeRef::MakeInt64());
        }
        LirReg payload = LowerExpr(*e.payloads[0]);
        if (e.payloads[0]->type.kind != TypeRef::Kind::Int64 && e.payloads[0]->type.kind != TypeRef::Kind::Int) {
            payload = EmitCast(payload, e.payloads[0]->type, TypeRef::MakeInt64());
        }
        LirReg shift = EmitConst("32", TypeRef::MakeInt64());
        LirReg shifted = EmitBinary(LirOpcode::Shl, payload, shift, TypeRef::MakeInt64());
        LirReg tag = EmitConst(e.discriminant, TypeRef::MakeInt64());
        return EmitBinary(LirOpcode::Or, shifted, tag, TypeRef::MakeInt64());
    }

    LirReg LowerCall(const HirCallExpr &e) {
        std::vector<LirReg> argRegs;
        argRegs.reserve(e.args.size());
        for (const auto &arg : e.args) {
            // Slice types are 16-byte {data, length} structs. The callee
            // expects a POINTER to the struct (not the 16-byte value, which
            // wouldn't fit in a single ABI register), so take the lvalue.
            if (IsSliceType(arg->type)) {
                argRegs.push_back(LowerLValue(*arg));
            }
            else {
                argRegs.push_back(LowerExpr(*arg));
            }
        }
        const LirReg dst = e.type.IsOpaque() ? LirNoReg : NewReg();
        LirInstr ci;
        ci.dst = dst;
        ci.type = e.type;
        ci.srcs = std::move(argRegs);
        if (auto *v = dynamic_cast<const HirVarExpr *>(e.callee.get()); v && !locals.contains(v->name)) {
            // Direct call to a named function. A same-named local would be a
            // function-pointer variable and is handled by the indirect path.
            ci.op = LirOpcode::Call;
            ci.strArg = v->name;
            auto it = funcConvs.find(v->name);
            if (it != funcConvs.end()) {
                ci.callConv = it->second;
            }
        }
        else if (auto *p = dynamic_cast<const HirPathExpr *>(e.callee.get())) {
            ci.op = LirOpcode::Call;
            // Module qualifiers are compile-time only; the binary symbol is
            // just the final segment (e.g. Math::Add → "Add").
            ci.strArg = p->segments.back();
            auto it = funcConvs.find(ci.strArg);
            if (it != funcConvs.end()) {
                ci.callConv = it->second;
            }
        }
        else {
            // Function pointer / indirect call: evaluate callee first.
            ci.op = LirOpcode::CallIndirect;
            LirReg fp = LowerExpr(*e.callee);
            ci.srcs.insert(ci.srcs.begin(), fp);
        }
        Emit(std::move(ci));
        return dst;
    }

    void StoreRangeInit(const HirRangeExpr &e, LirReg slot) {
        const TypeRef elemType = e.type.inner.empty() ? TypeRef::MakeInt64() : e.type.inner[0];
        // Endpoints may be narrower than the range element type (e.g. a uint32
        // bound in a Range<int>). Widen them to the element type so the store
        // writes the full field; otherwise the unwritten high bits are garbage.
        if (e.lo) {
            const LirReg loVal = EmitCastIfNeeded(LowerExpr(*e.lo), e.lo->type, elemType);
            const LirReg loPtr = EmitFieldPtr(slot, "lo", elemType);
            EmitStore(loVal, loPtr, elemType);
        }
        if (e.hi) {
            const LirReg hiVal = EmitCastIfNeeded(LowerExpr(*e.hi), e.hi->type, elemType);
            const LirReg hiPtr = EmitFieldPtr(slot, "hi", elemType);
            EmitStore(hiVal, hiPtr, elemType);
        }
        const LirReg inclVal = EmitConst(e.inclusive ? "1" : "0", TypeRef::MakeBool());
        const LirReg inclPtr = EmitFieldPtr(slot, "inclusive", TypeRef::MakeBool());
        EmitStore(inclVal, inclPtr, TypeRef::MakeBool());
    }

    LirReg LowerRange(const HirRangeExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreRangeInit(e, slot);
        return slot;
    }

    LirReg LowerStructInit(const HirStructInitExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreStructInit(e, slot);
        return EmitLoad(slot, e.type);
    }

    void StoreStructInit(const HirStructInitExpr &e, LirReg slot) {
        for (const auto &f : e.fields) {
            const LirReg ptr = EmitFieldPtr(slot, f.name, f.value->type);
            StoreExprIntoSlot(*f.value, ptr, f.value->type);
        }
    }

    LirReg LowerSlice(const HirSliceExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreSliceInit(e, slot);
        return slot;
    }

    LirReg LowerTuple(const HirTupleExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreTupleInit(e, slot);
        return slot;
    }

    void StoreTupleInit(const HirTupleExpr &e, LirReg slot) {
        for (std::size_t i = 0; i < e.elements.size(); ++i) {
            const LirReg ptr = EmitFieldPtr(slot, std::to_string(i), e.elements[i]->type);
            StoreExprIntoSlot(*e.elements[i], ptr, e.elements[i]->type);
        }
    }

    LirReg LowerStringLiteralSlice(const HirLiteralExpr &e) {
        const LirReg slot = EmitAlloca(e.type);
        StoreStringLiteralSlice(e, slot);
        return slot;
    }

    void StoreStringLiteralSlice(const HirLiteralExpr &e, LirReg slot) {
        const TypeRef elemType = StringSliceElementType(e);
        const LirReg data = EmitStringAddr(e.value, elemType);
        LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
        EmitStore(data, dataField, TypeRef::MakePointer(elemType));
        LirReg len = EmitConst(std::to_string(e.value.size()), TypeRef::MakeUInt64());
        LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
        EmitStore(len, lenField, TypeRef::MakeUInt64());
    }

    void StoreSliceInit(const HirSliceExpr &e, LirReg slot) {
        TypeRef elemType = e.elementType;
        if (elemType.IsUnknown() && !e.elements.empty()) {
            elemType = e.elements.front()->type;
        }
        LirReg data = EmitAlloca(elemType);
        fn->blocks[cur].instrs.back().strArg = std::to_string(e.elements.size());
        for (std::size_t i = 0; i < e.elements.size(); ++i) {
            LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
            LirReg ptr = EmitIndexPtr(data, idx, elemType);
            StoreExprIntoSlot(*e.elements[i], ptr, elemType);
        }
        LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
        EmitStore(data, dataField, TypeRef::MakePointer(elemType));
        LirReg len = EmitConst(std::to_string(e.elements.size()), TypeRef::MakeUInt64());
        LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
        EmitStore(len, lenField, TypeRef::MakeUInt64());
    }

    LirReg LowerSliceDataPtr(const HirExpr &object, const TypeRef &elemType) {
        if (!IsSliceType(object.type)) {
            return LowerExpr(object);
        }
        LirReg slicePtr = LowerLValue(object);
        LirReg dataField = EmitFieldPtr(slicePtr, "data", TypeRef::MakePointer(elemType));
        return EmitLoad(dataField, TypeRef::MakePointer(elemType));
    }

    // Returns the pointer register for an lvalue expression.
    LirReg LowerLValue(const HirExpr &expr) {
        if (auto *e = dynamic_cast<const HirVarExpr *>(&expr)) {
            auto it = locals.find(e->name);
            if (it != locals.end()) {
                return it->second;
            }
            // A slice-typed constant lives in read-only data under its own
            // name, so indexing or reading .length works off that address. A
            // local const has no symbol, so its initializer is materialized.
            if (const auto constIt = localConsts.find(e->name); constIt != localConsts.end()) {
                return LowerLValue(*constIt->second.value);
            }
            if (const auto constIt = globalConsts.find(e->name); constIt != globalConsts.end()) {
                if (IsSliceType(constIt->second->type)) {
                    return EmitGlobalAddr(e->name, constIt->second->type);
                }
                return LowerLValue(*constIt->second->value);
            }
            // Global variable address.
            LirReg ptr = NewReg();
            LirInstr i;
            i.dst = ptr;
            i.op = LirOpcode::Load;
            i.type = TypeRef::MakePointer(e->type);
            i.strArg = "&" + e->name;
            Emit(std::move(i));
            return ptr;
        }
        if (auto *e = dynamic_cast<const HirSliceExpr *>(&expr)) {
            LirReg slot = EmitAlloca(e->type);
            StoreSliceInit(*e, slot);
            return slot;
        }
        if (auto *e = dynamic_cast<const HirTupleExpr *>(&expr)) {
            LirReg slot = EmitAlloca(e->type);
            StoreTupleInit(*e, slot);
            return slot;
        }
        if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
            return LowerRange(*e);
        }
        if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr)) {
            LirReg slot = EmitAlloca(e->type);
            StoreTernaryInit(*e, slot, e->type);
            return slot;
        }
        if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr)) {
            LirReg base =
                e->object->type.kind == TypeRef::Kind::Pointer ? LowerExpr(*e->object) : LowerLValue(*e->object);
            return EmitFieldPtr(base, e->field, e->type);
        }

        if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr)) {
            LirReg idx = LowerExpr(*e->index);
            LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
            return EmitIndexPtr(sliceBase, idx, e->type);
        }
        if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr)) {
            if (e->op == TokenKind::Star) {
                return LowerExpr(*e->operand); // pointer dereference
            }
        }
        if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr)) {
            // String literals: return the alloca slot directly instead of
            // spilling through the 16-byte fallback (which would misread
            // the alloca vreg's 8-byte pointer slot as the slice value).
            if (IsStringSliceLiteral(*e)) {
                return LowerStringLiteralSlice(*e);
            }
            LirReg slot = EmitAlloca(expr.type);
            EmitStore(EmitConst(e->value, e->type), slot, expr.type);
            return slot;
        }
        // Non-addressable fallback: spill to a temp slot.
        LirReg val = LowerExpr(expr);
        LirReg slot = EmitAlloca(expr.type);
        EmitStore(val, slot, expr.type);
        return slot;
    }
};

// Lir public API
HirToLirLowering::HirToLirLowering(HirPackage package)
    : hir_(std::move(package)) {
}

LirPackage HirToLirLowering::Generate() {
    HirPassManager::Run(hir_);

    LirLowering lowering;
    return lowering.Run(hir_);
}
} // namespace Rux
