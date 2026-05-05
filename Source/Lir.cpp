/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Lir.h"

#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace Rux
{
    // Internal helpers
    static std::string_view OpcodeStr(LirOpcode op)
    {
        switch (op)
        {
        case LirOpcode::Const: return "const";
        case LirOpcode::Alloca: return "alloca";
        case LirOpcode::Load: return "load";
        case LirOpcode::Store: return "store";
        case LirOpcode::Add: return "add";
        case LirOpcode::Sub: return "sub";
        case LirOpcode::Mul: return "mul";
        case LirOpcode::Div: return "div";
        case LirOpcode::Mod: return "mod";
        case LirOpcode::Pow: return "pow";
        case LirOpcode::And: return "and";
        case LirOpcode::Or: return "or";
        case LirOpcode::Xor: return "xor";
        case LirOpcode::Shl: return "shl";
        case LirOpcode::Shr: return "shr";
        case LirOpcode::Neg: return "neg";
        case LirOpcode::Not: return "not";
        case LirOpcode::BitNot: return "bitnot";
        case LirOpcode::CmpEq: return "cmpeq";
        case LirOpcode::CmpNe: return "cmpne";
        case LirOpcode::CmpLt: return "cmplt";
        case LirOpcode::CmpLe: return "cmple";
        case LirOpcode::CmpGt: return "cmpgt";
        case LirOpcode::CmpGe: return "cmpge";
        case LirOpcode::Cast: return "cast";
        case LirOpcode::Call: return "call";
        case LirOpcode::CallIndirect: return "call_ind";
        case LirOpcode::FieldPtr: return "fieldptr";
        case LirOpcode::IndexPtr: return "indexptr";
        case LirOpcode::Phi: return "phi";
        default: return "?";
        }
    }

    static LirOpcode BinaryOpcode(TokenKind op)
    {
        using TK = TokenKind;
        switch (op)
        {
        case TK::Plus: return LirOpcode::Add;
        case TK::Minus: return LirOpcode::Sub;
        case TK::Star: return LirOpcode::Mul;
        case TK::Slash: return LirOpcode::Div;
        case TK::Percent: return LirOpcode::Mod;
        case TK::StarStar: return LirOpcode::Pow;
        case TK::Amp: return LirOpcode::And;
        case TK::Pipe: return LirOpcode::Or;
        case TK::Caret: return LirOpcode::Xor;
        case TK::LessLess: return LirOpcode::Shl;
        case TK::GreaterGreater: return LirOpcode::Shr;
        case TK::AmpAmp: return LirOpcode::And;
        case TK::PipePipe: return LirOpcode::Or;
        case TK::Equal: return LirOpcode::CmpEq;
        case TK::BangEqual: return LirOpcode::CmpNe;
        case TK::Less: return LirOpcode::CmpLt;
        case TK::LessEqual: return LirOpcode::CmpLe;
        case TK::Greater: return LirOpcode::CmpGt;
        case TK::GreaterEqual: return LirOpcode::CmpGe;
        default: return LirOpcode::Add;
        }
    }

    static LirOpcode CompoundOpcode(TokenKind op)
    {
        using TK = TokenKind;
        switch (op)
        {
        case TK::PlusAssign: return LirOpcode::Add;
        case TK::MinusAssign: return LirOpcode::Sub;
        case TK::StarAssign: return LirOpcode::Mul;
        case TK::SlashAssign: return LirOpcode::Div;
        case TK::PercentAssign: return LirOpcode::Mod;
        case TK::AmpAssign: return LirOpcode::And;
        case TK::PipeAssign: return LirOpcode::Or;
        case TK::CaretAssign: return LirOpcode::Xor;
        case TK::LessLessAssign: return LirOpcode::Shl;
        case TK::GreaterGreaterAssign: return LirOpcode::Shr;
        default: return LirOpcode::Add;
        }
    }

    // Lowering
    class LirLowering
    {
    public:
        LirPackage Run(const HirPackage& hir)
        {
            LirPackage pkg;
            for (const auto& mod : hir.modules)
                pkg.modules.push_back(LowerModule(mod));
            return pkg;
        }

    private:
        LirReg nextReg = 0;
        LirFunc* fn = nullptr; // function being built (valid only inside LowerFunc)
        std::uint32_t cur = 0; // current basic-block index into fn_->blocks
        std::unordered_map<std::string, LirReg> locals; // name → alloca register
        std::uint32_t breakTarget = 0;
        std::uint32_t continueTarget = 0;
        std::unordered_map<std::string, CallingConvention> funcConvs; // name → calling convention

        // Block / register allocation
        LirReg NewReg()
        {
            return nextReg++;
        }

        [[nodiscard]] std::uint32_t NewBlock(std::string label = "") const
        {
            if (label.empty())
                label = std::format("bb{}", fn->blocks.size());
            fn->blocks.push_back({std::move(label), {}, std::nullopt});
            return static_cast<std::uint32_t>(fn->blocks.size() - 1);
        }

        void SetBlock(const std::uint32_t idx)
        {
            cur = idx;
        }

        [[nodiscard]] bool IsTerminated() const
        {
            return fn->blocks[cur].term.has_value();
        }

        // Instruction emission
        void Emit(LirInstr i) const
        {
            fn->blocks[cur].instrs.push_back(std::move(i));
        }

        void Terminate(LirTerminator t) const
        {
            if (!fn->blocks[cur].term.has_value())
                fn->blocks[cur].term = std::move(t);
        }

        void Jump(std::uint32_t target) const
        {
            Terminate(LirTerminator{.kind = LirTermKind::Jump, .trueTarget = target});
        }

        void Branch(const LirReg cond, const std::uint32_t trueTarget, std::uint32_t falseTarget) const
        {
            Terminate(LirTerminator{
                .kind = LirTermKind::Branch,
                .cond = cond,
                .trueTarget = trueTarget,
                .falseTarget = falseTarget,
            });
        }

        void Return(const std::optional<LirReg> val, TypeRef type) const
        {
            LirTerminator t;
            t.kind = LirTermKind::Return;
            t.retVal = val;
            t.retType = std::move(type);
            Terminate(std::move(t));
        }

        // Instruction builders

        LirReg EmitConst(std::string value, TypeRef type)
        {
            LirReg r = NewReg();
            LirInstr i;
            i.dst = r;
            i.op = LirOpcode::Const;
            i.type = std::move(type);
            i.strArg = std::move(value);
            Emit(std::move(i));
            return r;
        }

        LirReg EmitAlloca(TypeRef type)
        {
            LirReg r = NewReg();
            LirInstr i;
            i.dst = r;
            i.op = LirOpcode::Alloca;
            i.type = std::move(type);
            Emit(std::move(i));
            return r;
        }

        LirReg EmitLoad(LirReg ptr, TypeRef type)
        {
            const LirReg r = NewReg();
            LirInstr i;
            i.dst = r;
            i.op = LirOpcode::Load;
            i.type = std::move(type);
            i.srcs = {ptr};
            Emit(std::move(i));
            return r;
        }

        LirReg EmitNamedLoad(std::string name, TypeRef type)
        {
            LirReg r = NewReg();
            LirInstr i;
            i.dst = r;
            i.op = LirOpcode::Load;
            i.type = std::move(type);
            i.strArg = std::move(name);
            Emit(std::move(i));
            return r;
        }

        void EmitStore(LirReg val, LirReg ptr, TypeRef type) const
        {
            LirInstr i;
            i.dst = LirNoReg;
            i.op = LirOpcode::Store;
            i.type = std::move(type);
            i.srcs = {val, ptr};
            Emit(std::move(i));
        }

        LirReg EmitBinary(const LirOpcode op, LirReg l, LirReg r, TypeRef type)
        {
            const LirReg dst = NewReg();
            LirInstr i;
            i.dst = dst;
            i.op = op;
            i.type = std::move(type);
            i.srcs = {l, r};
            Emit(std::move(i));
            return dst;
        }

        LirReg EmitUnary(LirOpcode op, LirReg src, const TypeRef& type)
        {
            const LirReg dst = NewReg();
            LirInstr i;
            i.dst = dst;
            i.op = op;
            i.type = type;
            i.srcs = {src};
            Emit(std::move(i));
            return dst;
        }

        LirReg EmitFieldPtr(LirReg base, std::string field, const TypeRef& elemType)
        {
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

        LirReg EmitIndexPtr(LirReg base, LirReg idx, const TypeRef& elemType)
        {
            LirReg ptr = NewReg();
            LirInstr i;
            i.dst = ptr;
            i.op = LirOpcode::IndexPtr;
            i.type = TypeRef::MakePointer(elemType);
            i.srcs = {base, idx};
            Emit(std::move(i));
            return ptr;
        }

        static bool IsSliceType(const TypeRef& type)
        {
            return type.kind == TypeRef::Kind::Slice ||
                (type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<"));
        }

        static bool IsStringSliceLiteral(const HirLiteralExpr& e)
        {
            return e.type.kind == TypeRef::Kind::Named &&
            (e.type.name == "Slice<char8>" ||
                e.type.name == "Slice<char16>" ||
                e.type.name == "Slice<char32>");
        }

        static TypeRef StringSliceElementType(const HirLiteralExpr& e)
        {
            if (e.type.kind == TypeRef::Kind::Named)
            {
                if (e.type.name == "Slice<char16>") return TypeRef::MakeChar16();
                if (e.type.name == "Slice<char32>") return TypeRef::MakeChar32();
            }
            return TypeRef::MakeChar8();
        }

        // Module lowering
        LirModule LowerModule(const HirModule& mod)
        {
            // Build calling-convention map before lowering functions
            funcConvs.clear();
            for (const auto& ef : mod.externFuncs)
                if (ef.callConv != CallingConvention::Default)
                    funcConvs[ef.name] = ef.callConv;
            for (const auto& f : mod.funcs)
                if (f.callConv != CallingConvention::Default)
                    funcConvs[f.name] = f.callConv;

            LirModule lm;
            lm.name = mod.name;
            for (const auto& s : mod.structs)
            {
                LirStructDecl sd;
                sd.name = s.name;
                sd.isPublic = s.isPublic;
                sd.typeParams = s.typeParams;
                for (const auto& f : s.fields)
                    sd.fields.push_back({f.name, f.type});
                lm.structs.push_back(std::move(sd));
            }
            for (const auto& e : mod.enums)
            {
                LirEnumDecl ed;
                ed.name = e.name;
                ed.isPublic = e.isPublic;
                for (const auto& v : e.variants)
                    ed.variants.push_back({v.name, v.fields});
                lm.enums.push_back(std::move(ed));
            }
            for (const auto& u : mod.unions)
            {
                LirUnionDecl ud;
                ud.name = u.name;
                ud.isPublic = u.isPublic;
                for (const auto& f : u.fields)
                    ud.fields.push_back({f.name, f.type});
                lm.unions.push_back(std::move(ud));
            }
            for (const auto& c : mod.consts)
            {
                LirConstDecl cd;
                cd.name = c.name;
                cd.isPublic = c.isPublic;
                cd.type = c.type;
                cd.value = PrintConstExpr(*c.value);
                lm.consts.push_back(std::move(cd));
            }
            for (const auto& ta : mod.typeAliases)
                lm.typeAliases.push_back({ta.name, ta.isPublic, ta.type});
            for (const auto& ev : mod.externVars)
                lm.externVars.push_back({ev.name, ev.isPublic, ev.type});
            for (const auto& ef : mod.externFuncs)
            {
                LirFunc lf;
                lf.name = ef.name;
                lf.dll = ef.dll;
                lf.isPublic = ef.isPublic;
                lf.isExtern = true;
                lf.callConv = ef.callConv;
                lf.returnType = ef.returnType;
                LirReg pr = 0;
                for (const auto& p : ef.params)
                    lf.params.push_back({pr++, p.type, p.name});
                lm.funcs.push_back(std::move(lf));
            }
            for (const auto& f : mod.funcs)
                lm.funcs.push_back(LowerFunc(f));
            for (const auto& impl : mod.impls)
            {
                for (const auto& m : impl.methods)
                {
                    std::string mangledName = impl.typeName + "::" + m.name;
                    lm.funcs.push_back(LowerFunc(m, mangledName));
                }
            }
            return lm;
        }

        // Render a simple constant expression to a printable string.
        static std::string PrintConstExpr(const HirExpr& e)
        {
            if (auto* lit = dynamic_cast<const HirLiteralExpr*>(&e))
                return lit->value;
            if (auto* v = dynamic_cast<const HirVarExpr*>(&e))
                return v->name;
            if (auto* b = dynamic_cast<const HirBinaryExpr*>(&e))
                return PrintConstExpr(*b->left) + " op " + PrintConstExpr(*b->right);
            return "<const>";
        }

        // Function lowering
        LirFunc LowerFunc(const HirFunc& hf, std::string_view nameOverride = "")
        {
            nextReg = 0;
            locals.clear();
            LirFunc lf;
            lf.name = nameOverride.empty() ? hf.name : std::string(nameOverride);
            lf.isPublic = hf.isPublic;
            lf.isExtern = false;
            lf.callConv = hf.callConv;
            lf.returnType = hf.returnType;
            fn = &lf;
            cur = NewBlock("entry");
            for (const auto& p : hf.params)
            {
                const LirReg pr = NewReg();
                const LirReg slot = EmitAlloca(p.type);
                EmitStore(pr, slot, p.type);
                locals[p.name] = slot;
                lf.params.push_back({pr, p.type, p.name});
            }
            if (hf.body)
            {
                LowerBlock(*hf.body);
                if (!IsTerminated())
                    Return(std::nullopt, TypeRef::MakeOpaque());
            }
            return lf;
        }

        // Block / statement lowering
        void LowerBlock(const HirBlock& block)
        {
            for (const auto& stmt : block.stmts)
            {
                if (IsTerminated()) break;
                LowerStmt(*stmt);
            }
        }

        void LowerStmt(const HirStmt& stmt)
        {
            if (auto* s = dynamic_cast<const HirLetStmt*>(&stmt))
            {
                LirReg slot = EmitAlloca(s->type);
                locals[s->name] = slot;
                if (s->init)
                {
                    if (auto* init = dynamic_cast<const HirStructInitExpr*>(s->init.get()))
                    {
                        StoreStructInit(*init, slot);
                    }
                    else if (auto* initSliceExpr = dynamic_cast<const HirSliceExpr*>(s->init.get()))
                    {
                        StoreSliceInit(*initSliceExpr, slot);
                    }
                    else if (auto* initLitExpr = dynamic_cast<const HirLiteralExpr*>(s->init.get());
                        initLitExpr && IsStringSliceLiteral(*initLitExpr))
                    {
                        StoreStringLiteralSlice(*initLitExpr, slot);
                    }
                    else
                    {
                        const LirReg val = LowerExpr(*s->init);
                        EmitStore(val, slot, s->type);
                    }
                }
                return;
            }

            if (auto* s = dynamic_cast<const HirExprStmt*>(&stmt))
            {
                LowerExpr(*s->expr);
                return;
            }

            if (auto* s = dynamic_cast<const HirReturnStmt*>(&stmt))
            {
                if (s->value)
                {
                    LirReg val = LowerExpr(**s->value);
                    Return({val}, (*s->value)->type);
                }
                else
                {
                    Return(std::nullopt, TypeRef::MakeOpaque());
                }
                return;
            }

            if (dynamic_cast<const HirBreakStmt*>(&stmt))
            {
                Jump(breakTarget);
                return;
            }

            if (dynamic_cast<const HirContinueStmt*>(&stmt))
            {
                Jump(continueTarget);
                return;
            }

            if (auto* s = dynamic_cast<const HirIfStmt*>(&stmt))
            {
                LowerIf(*s);
                return;
            }

            if (auto* s = dynamic_cast<const HirWhileStmt*>(&stmt))
            {
                LowerWhile(*s);
                return;
            }

            if (auto* s = dynamic_cast<const HirForStmt*>(&stmt))
            {
                LowerFor(*s);
                return;
            }

            if (auto* s = dynamic_cast<const HirMatchStmt*>(&stmt))
            {
                LowerMatch(*s);
                return;
            }

            // HirLocalDecl — placeholder, nothing to emit
        }

        // Control-flow lowering
        void LowerIf(const HirIfStmt& s)
        {
            std::uint32_t mergeBlock = NewBlock("if.merge");

            // Pre-allocate blocks for each else-if condition so we know their
            // indices before emitting the branch for the preceding condition.
            std::vector<std::uint32_t> elifCondBlocks;
            elifCondBlocks.reserve(s.elseIfs.size());
            for (std::size_t i = 0; i < s.elseIfs.size(); ++i)
                elifCondBlocks.push_back(NewBlock(std::format("if.elif{}", i)));
            std::uint32_t elseBlock =
                s.elseBlock ? NewBlock("if.else") : mergeBlock;
            // Main condition
            const LirReg cond0 = LowerExpr(*s.condition);
            const std::uint32_t thenBb0 = NewBlock("if.then");
            const std::uint32_t fall0 = s.elseIfs.empty() ? elseBlock : elifCondBlocks[0];
            Branch(cond0, thenBb0, fall0);
            SetBlock(thenBb0);
            LowerBlock(s.thenBlock);
            if (!IsTerminated()) Jump(mergeBlock);
            // Else-if chain
            for (std::size_t i = 0; i < s.elseIfs.size(); ++i)
            {
                SetBlock(elifCondBlocks[i]);
                const LirReg elifCond = LowerExpr(*s.elseIfs[i].condition);
                const std::uint32_t elifThen = NewBlock(std::format("if.elif.then{}", i));
                const std::uint32_t nextFall = (i + 1 < s.elseIfs.size())
                                                   ? elifCondBlocks[i + 1]
                                                   : elseBlock;
                Branch(elifCond, elifThen, nextFall);
                SetBlock(elifThen);
                LowerBlock(s.elseIfs[i].block);
                if (!IsTerminated()) Jump(mergeBlock);
            }
            // Else block
            if (s.elseBlock)
            {
                SetBlock(elseBlock);
                LowerBlock(*s.elseBlock);
                if (!IsTerminated()) Jump(mergeBlock);
            }
            SetBlock(mergeBlock);
        }

        void LowerWhile(const HirWhileStmt& s)
        {
            std::uint32_t condBlock = NewBlock("while.cond");
            std::uint32_t bodyBlock = NewBlock("while.body");
            std::uint32_t afterBlock = NewBlock("while.after");
            if (!IsTerminated()) Jump(condBlock);
            SetBlock(condBlock);
            const LirReg cond = LowerExpr(*s.condition);
            Branch(cond, bodyBlock, afterBlock);
            const std::uint32_t savedBreak = breakTarget;
            const std::uint32_t savedContinue = continueTarget;
            breakTarget = afterBlock;
            continueTarget = condBlock;
            SetBlock(bodyBlock);
            LowerBlock(s.body);
            if (!IsTerminated()) Jump(condBlock);
            breakTarget = savedBreak;
            continueTarget = savedContinue;
            SetBlock(afterBlock);
        }

        void LowerFor(const HirForStmt& s)
        {
            std::uint32_t condBlock = NewBlock("for.cond");
            std::uint32_t bodyBlock = NewBlock("for.body");
            std::uint32_t afterBlock = NewBlock("for.after");
            // Allocate loop variable slot
            LirReg slot = EmitAlloca(s.varType);
            locals[s.variable] = slot;
            // Evaluate and store iterable
            LirReg iterVal = LowerExpr(*s.iterable);
            LirReg iterSlot = EmitAlloca(s.iterable->type);
            EmitStore(iterVal, iterSlot, s.iterable->type);
            if (!IsTerminated()) Jump(condBlock);
            // Condition: check iterator has remaining elements
            SetBlock(condBlock);
            LirReg hasNext = NewReg();
            {
                LirInstr ci;
                ci.dst = hasNext;
                ci.op = LirOpcode::Call;
                ci.type = TypeRef::MakeBool();
                ci.strArg = "@iter.has_next";
                ci.srcs = {iterSlot};
                Emit(std::move(ci));
            }
            Branch(hasNext, bodyBlock, afterBlock);
            std::uint32_t savedBreak = breakTarget;
            std::uint32_t savedContinue = continueTarget;
            breakTarget = afterBlock;
            continueTarget = condBlock;

            // Body: advance iterator, bind loop variable, then lower body
            SetBlock(bodyBlock);
            LirReg nextVal = NewReg();
            {
                LirInstr ni;
                ni.dst = nextVal;
                ni.op = LirOpcode::Call;
                ni.type = s.varType;
                ni.strArg = "@iter.next";
                ni.srcs = {iterSlot};
                Emit(std::move(ni));
            }
            EmitStore(nextVal, slot, s.varType);
            LowerBlock(s.body);
            if (!IsTerminated()) Jump(condBlock);
            breakTarget = savedBreak;
            continueTarget = savedContinue;
            SetBlock(afterBlock);
        }

        void LowerMatch(const HirMatchStmt& s)
        {
            const LirReg subjectVal = LowerExpr(*s.subject);
            const std::uint32_t mergeBlock = NewBlock("match.merge");
            if (s.arms.empty())
            {
                if (!IsTerminated()) Jump(mergeBlock);
                SetBlock(mergeBlock);
                return;
            }
            for (std::size_t i = 0; i < s.arms.size(); ++i)
            {
                const auto& arm = s.arms[i];
                const bool isLast = (i + 1 == s.arms.size());
                std::uint32_t bodyBlock = NewBlock(std::format("match.arm{}", i));
                std::uint32_t nextBlock =
                    isLast ? mergeBlock : NewBlock(std::format("match.next{}", i));
                LirReg matched = LowerPattern(*arm.pattern, subjectVal, s.subject->type);
                Branch(matched, bodyBlock, nextBlock);
                SetBlock(bodyBlock);
                LowerExpr(*arm.body);
                if (!IsTerminated()) Jump(mergeBlock);
                if (!isLast) SetBlock(nextBlock);
            }
            SetBlock(mergeBlock);
        }

        // Pattern lowering
        // Returns a bool register: 1 if the pattern matches `subjectVal`.
        // Side-effects: binds pattern variables into locals
        LirReg LowerPattern(const HirPattern& pat, LirReg subjectVal,
                            const TypeRef& subjectType)
        {
            if (dynamic_cast<const HirWildcardPattern*>(&pat))
                return EmitConst("1", TypeRef::MakeBool());
            if (auto* p = dynamic_cast<const HirLiteralPattern*>(&pat))
            {
                LirReg lit = EmitConst(p->value, p->type);
                return EmitBinary(LirOpcode::CmpEq, subjectVal, lit, TypeRef::MakeBool());
            }
            if (auto* p = dynamic_cast<const HirBindingPattern*>(&pat))
            {
                LirReg bindSlot = EmitAlloca(p->type);
                locals[p->name] = bindSlot;
                EmitStore(subjectVal, bindSlot, p->type);
                return EmitConst("1", TypeRef::MakeBool());
            }
            if (auto* p = dynamic_cast<const HirRangePattern*>(&pat))
            {
                LirReg lo = LirNoReg, hi = LirNoReg;
                if (auto* lit = dynamic_cast<const HirLiteralPattern*>(p->lo.get()))
                    lo = EmitConst(lit->value, subjectType);
                else
                    lo = EmitConst("0", subjectType);
                if (auto* lit = dynamic_cast<const HirLiteralPattern*>(p->hi.get()))
                    hi = EmitConst(lit->value, subjectType);
                else
                    hi = EmitConst("0", subjectType);
                const LirReg cmpLo = EmitBinary(LirOpcode::CmpLe, lo, subjectVal, TypeRef::MakeBool());
                const LirOpcode hiOp = p->inclusive ? LirOpcode::CmpLe : LirOpcode::CmpLt;
                const LirReg cmpHi = EmitBinary(hiOp, subjectVal, hi, TypeRef::MakeBool());
                return EmitBinary(LirOpcode::And, cmpLo, cmpHi, TypeRef::MakeBool());
            }

            // Enum, struct, tuple patterns: lower payload bindings, then emit a
            // placeholder true. Full structural matching requires runtime support
            // beyond what this IR stage provides.
            if (auto* p = dynamic_cast<const HirEnumPattern*>(&pat))
            {
                for (const auto& arg : p->args)
                {
                    if (auto* bp = dynamic_cast<const HirBindingPattern*>(arg.get()))
                    {
                        const LirReg bindSlot = EmitAlloca(bp->type);
                        locals[bp->name] = bindSlot;
                    }
                }
                return EmitConst("1", TypeRef::MakeBool());
            }

            if (auto* p = dynamic_cast<const HirStructPattern*>(&pat))
            {
                for (const auto& f : p->fields)
                {
                    if (auto* bp = dynamic_cast<const HirBindingPattern*>(f.pattern.get()))
                    {
                        LirReg bindSlot = EmitAlloca(bp->type);
                        locals[bp->name] = bindSlot;
                    }
                }
                return EmitConst("1", TypeRef::MakeBool());
            }

            if (auto* p = dynamic_cast<const HirTuplePattern*>(&pat))
            {
                for (const auto& elem : p->elements)
                {
                    if (auto* bp = dynamic_cast<const HirBindingPattern*>(elem.get()))
                    {
                        LirReg bindSlot = EmitAlloca(bp->type);
                        locals[bp->name] = bindSlot;
                    }
                }
                return EmitConst("1", TypeRef::MakeBool());
            }

            if (auto* p = dynamic_cast<const HirGuardedPattern*>(&pat))
            {
                LirReg inner = LowerPattern(*p->inner, subjectVal, subjectType);
                LirReg guard = LowerExpr(*p->guard);
                return EmitBinary(LirOpcode::And, inner, guard, TypeRef::MakeBool());
            }

            return EmitConst("1", TypeRef::MakeBool()); // wildcard fallback
        }

        // Expression lowering
        // Returns the register holding the expression's value.
        // For void expressions the return value is LirNoReg.

        LirReg LowerExpr(const HirExpr& expr)
        {
            if (auto* e = dynamic_cast<const HirLiteralExpr*>(&expr))
            {
                if (IsStringSliceLiteral(*e))
                    return LowerStringLiteralSlice(*e);
                return EmitConst(e->value, e->type);
            }
            if (auto* e = dynamic_cast<const HirVarExpr*>(&expr))
            {
                auto it = locals.find(e->name);
                if (it != locals.end())
                    return EmitLoad(it->second, e->type);
                return EmitNamedLoad(e->name, e->type);
            }
            if (dynamic_cast<const HirSelfExpr*>(&expr))
            {
                auto it = locals.find("self");
                if (it != locals.end())
                    return EmitLoad(it->second, expr.type);
                return EmitNamedLoad("self", expr.type);
            }
            if (auto* e = dynamic_cast<const HirPathExpr*>(&expr))
            {
                std::string path;
                for (std::size_t i = 0; i < e->segments.size(); ++i)
                {
                    if (i) path += "::";
                    path += e->segments[i];
                }
                return EmitNamedLoad(path, e->type);
            }
            if (auto* e = dynamic_cast<const HirUnaryExpr*>(&expr))
                return LowerUnary(*e);
            if (auto* e = dynamic_cast<const HirBinaryExpr*>(&expr))
                return LowerBinary(*e);
            if (auto* e = dynamic_cast<const HirAssignExpr*>(&expr))
                return LowerAssign(*e);
            if (auto* e = dynamic_cast<const HirTernaryExpr*>(&expr))
                return LowerTernary(*e);
            if (auto* e = dynamic_cast<const HirCallExpr*>(&expr))
                return LowerCall(*e);
            if (auto* e = dynamic_cast<const HirIndexExpr*>(&expr))
            {
                LirReg idx = LowerExpr(*e->index);
                LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
                LirReg ptr = EmitIndexPtr(sliceBase, idx, e->type);
                return EmitLoad(ptr, e->type);
            }
            if (auto* e = dynamic_cast<const HirFieldExpr*>(&expr))
            {
                LirReg base = LowerLValue(*e->object);
                LirReg ptr = EmitFieldPtr(base, e->field, e->type);
                return EmitLoad(ptr, e->type);
            }
            if (auto* e = dynamic_cast<const HirStructInitExpr*>(&expr))
                return LowerStructInit(*e);
            if (auto* e = dynamic_cast<const HirSliceExpr*>(&expr))
                return LowerSlice(*e);
            if (auto* e = dynamic_cast<const HirCastExpr*>(&expr))
            {
                LirReg src = LowerExpr(*e->operand);
                LirReg dst = NewReg();
                LirInstr i;
                i.dst = dst;
                i.op = LirOpcode::Cast;
                i.type = e->type;
                i.srcs = {src};
                i.strArg = e->operand->type.ToString(); // source type
                Emit(std::move(i));
                return dst;
            }
            if (auto* e = dynamic_cast<const HirIsExpr*>(&expr))
            {
                LirReg src = LowerExpr(*e->operand);
                LirReg dst = NewReg();
                LirInstr i;
                i.dst = dst;
                i.op = LirOpcode::Call;
                i.type = TypeRef::MakeBool();
                i.srcs = {src};
                i.strArg = "@is_type:" + e->checkType.ToString();
                Emit(std::move(i));
                return dst;
            }
            if (auto* e = dynamic_cast<const HirBlockExpr*>(&expr))
            {
                LowerBlock(e->block);
                return EmitConst("0", e->type);
            }
            // HirRangeExpr: ranges require runtime support; emit a placeholder.
            return EmitConst("0", expr.type);
        }

        LirReg LowerUnary(const HirUnaryExpr& e)
        {
            using TK = TokenKind;
            switch (e.op)
            {
            case TK::Minus:
                return EmitUnary(LirOpcode::Neg, LowerExpr(*e.operand), e.type);
            case TK::Bang:
                return EmitUnary(LirOpcode::Not, LowerExpr(*e.operand), e.type);
            case TK::Tilde:
                return EmitUnary(LirOpcode::BitNot, LowerExpr(*e.operand), e.type);
            case TK::Star:
                {
                    // Dereference: the operand evaluates to a pointer; load through it.
                    LirReg ptr = LowerExpr(*e.operand);
                    return EmitLoad(ptr, e.type);
                }
            case TK::Amp:
                {
                    // Address-of: return the alloca slot for named locals, otherwise materialize a temporary.
                    if (auto* v = dynamic_cast<const HirVarExpr*>(e.operand.get()))
                        if (const auto it = locals.find(v->name); it != locals.end())
                            return it->second;
                    // Non-addressable: evaluate into a temp slot.
                    const LirReg val = LowerExpr(*e.operand);
                    const LirReg slot = EmitAlloca(e.operand->type);
                    EmitStore(val, slot, e.operand->type);
                    return slot;
                }
            default:
                return EmitUnary(LirOpcode::Not, LowerExpr(*e.operand), e.type);
            }
        }

        LirReg LowerBinary(const HirBinaryExpr& e)
        {
            using TK = TokenKind;
            // Short-circuit operators: branch to avoid evaluating the right-hand side.
            if (e.op == TK::AmpAmp || e.op == TK::PipePipe)
            {
                LirReg lhs = LowerExpr(*e.left);
                std::uint32_t rhsBlock = NewBlock(e.op == TK::AmpAmp ? "land.rhs" : "lor.rhs");
                std::uint32_t shortBlock = NewBlock(e.op == TK::AmpAmp ? "land.short" : "lor.short");
                std::uint32_t mergeBlock = NewBlock(e.op == TK::AmpAmp ? "land.merge" : "lor.merge");
                if (e.op == TK::AmpAmp)
                    Branch(lhs, rhsBlock, shortBlock); // false → skip rhs
                else
                    Branch(lhs, shortBlock, rhsBlock); // true  → skip rhs
                // Short-circuit path: result is the known constant.
                SetBlock(shortBlock);
                LirReg shortVal = EmitConst(e.op == TK::AmpAmp ? "0" : "1", TypeRef::MakeBool());
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
            return EmitBinary(BinaryOpcode(e.op), lhs, rhs, e.type);
        }

        LirReg LowerAssign(const HirAssignExpr& e)
        {
            LirReg val = LowerExpr(*e.value);
            if (e.op != TokenKind::Assign)
            {
                // Compound assignment: load current value, compute, then store.
                const LirReg current = LowerExpr(*e.target);
                val = EmitBinary(CompoundOpcode(e.op), current, val, e.type);
            }
            const LirReg ptr = LowerLValue(*e.target);
            EmitStore(val, ptr, e.type);
            return val;
        }

        LirReg LowerTernary(const HirTernaryExpr& e)
        {
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

        LirReg LowerCall(const HirCallExpr& e)
        {
            std::vector<LirReg> argRegs;
            argRegs.reserve(e.args.size());
            for (const auto& arg : e.args)
                argRegs.push_back(LowerExpr(*arg));
            const LirReg dst = e.type.IsOpaque() ? LirNoReg : NewReg();
            LirInstr ci;
            ci.dst = dst;
            ci.type = e.type;
            ci.srcs = std::move(argRegs);
            if (auto* v = dynamic_cast<const HirVarExpr*>(e.callee.get()))
            {
                ci.op = LirOpcode::Call;
                ci.strArg = v->name;
                auto it = funcConvs.find(v->name);
                if (it != funcConvs.end()) ci.callConv = it->second;
            }
            else if (auto* p = dynamic_cast<const HirPathExpr*>(e.callee.get()))
            {
                ci.op = LirOpcode::Call;
                for (std::size_t i = 0; i < p->segments.size(); ++i)
                {
                    if (i) ci.strArg += "::";
                    ci.strArg += p->segments[i];
                }
                auto it = funcConvs.find(ci.strArg);
                if (it != funcConvs.end()) ci.callConv = it->second;
            }
            else
            {
                // Function pointer / indirect call: evaluate callee first.
                ci.op = LirOpcode::CallIndirect;
                LirReg fp = LowerExpr(*e.callee);
                ci.srcs.insert(ci.srcs.begin(), fp);
            }
            Emit(std::move(ci));
            return dst;
        }

        LirReg LowerStructInit(const HirStructInitExpr& e)
        {
            const LirReg slot = EmitAlloca(e.type);
            StoreStructInit(e, slot);
            return EmitLoad(slot, e.type);
        }

        void StoreStructInit(const HirStructInitExpr& e, LirReg slot)
        {
            for (const auto& f : e.fields)
            {
                const LirReg val = LowerExpr(*f.value);
                const LirReg ptr = EmitFieldPtr(slot, f.name, f.value->type);
                EmitStore(val, ptr, f.value->type);
            }
        }

        LirReg LowerSlice(const HirSliceExpr& e)
        {
            const LirReg slot = EmitAlloca(e.type);
            StoreSliceInit(e, slot);
            return slot;
        }

        LirReg LowerStringLiteralSlice(const HirLiteralExpr& e)
        {
            const LirReg slot = EmitAlloca(e.type);
            StoreStringLiteralSlice(e, slot);
            return slot;
        }

        void StoreStringLiteralSlice(const HirLiteralExpr& e, LirReg slot)
        {
            const TypeRef elemType = StringSliceElementType(e);
            const LirReg data = EmitAlloca(elemType);
            fn->blocks[cur].instrs.back().strArg = std::to_string(e.value.size());
            for (std::size_t i = 0; i < e.value.size(); ++i)
            {
                const auto ch = static_cast<unsigned char>(e.value[i]);
                LirReg val = EmitConst(std::to_string(ch), elemType);
                LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
                LirReg ptr = EmitIndexPtr(data, idx, elemType);
                EmitStore(val, ptr, elemType);
            }
            LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
            EmitStore(data, dataField, TypeRef::MakePointer(elemType));
            LirReg len = EmitConst(std::to_string(e.value.size()), TypeRef::MakeUInt64());
            LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
            EmitStore(len, lenField, TypeRef::MakeUInt64());
        }

        void StoreSliceInit(const HirSliceExpr& e, LirReg slot)
        {
            TypeRef elemType = e.elementType;
            if (elemType.IsUnknown() && !e.elements.empty())
                elemType = e.elements.front()->type;
            LirReg data = EmitAlloca(elemType);
            fn->blocks[cur].instrs.back().strArg = std::to_string(e.elements.size());
            for (std::size_t i = 0; i < e.elements.size(); ++i)
            {
                LirReg val = LowerExpr(*e.elements[i]);
                LirReg idx = EmitConst(std::to_string(i), TypeRef::MakeUInt64());
                LirReg ptr = EmitIndexPtr(data, idx, elemType);
                EmitStore(val, ptr, elemType);
            }
            LirReg dataField = EmitFieldPtr(slot, "data", TypeRef::MakePointer(elemType));
            EmitStore(data, dataField, TypeRef::MakePointer(elemType));
            LirReg len = EmitConst(std::to_string(e.elements.size()), TypeRef::MakeUInt64());
            LirReg lenField = EmitFieldPtr(slot, "length", TypeRef::MakeUInt64());
            EmitStore(len, lenField, TypeRef::MakeUInt64());
        }

        LirReg LowerSliceDataPtr(const HirExpr& object, const TypeRef& elemType)
        {
            if (!IsSliceType(object.type))
                return LowerExpr(object);
            LirReg slicePtr = LowerLValue(object);
            LirReg dataField = EmitFieldPtr(slicePtr, "data", TypeRef::MakePointer(elemType));
            return EmitLoad(dataField, TypeRef::MakePointer(elemType));
        }

        // Returns the pointer register for an lvalue expression.
        LirReg LowerLValue(const HirExpr& expr)
        {
            if (auto* e = dynamic_cast<const HirVarExpr*>(&expr))
            {
                auto it = locals.find(e->name);
                if (it != locals.end()) return it->second;
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
            if (auto* e = dynamic_cast<const HirSliceExpr*>(&expr))
            {
                LirReg slot = EmitAlloca(e->type);
                StoreSliceInit(*e, slot);
                return slot;
            }
            if (auto* e = dynamic_cast<const HirFieldExpr*>(&expr))
            {
                LirReg base = LowerLValue(*e->object);
                return EmitFieldPtr(base, e->field, e->type);
            }

            if (auto* e = dynamic_cast<const HirIndexExpr*>(&expr))
            {
                LirReg idx = LowerExpr(*e->index);
                LirReg sliceBase = LowerSliceDataPtr(*e->object, e->type);
                return EmitIndexPtr(sliceBase, idx, e->type);
            }
            if (auto* e = dynamic_cast<const HirUnaryExpr*>(&expr))
            {
                if (e->op == TokenKind::Star)
                    return LowerExpr(*e->operand); // pointer dereference
            }
            // Non-addressable fallback: spill to a temp slot.
            LirReg val = LowerExpr(expr);
            LirReg slot = EmitAlloca(expr.type);
            EmitStore(val, slot, expr.type);
            return slot;
        }
    };

    // Lir public API
    Lir::Lir(HirPackage package)
        : hir(std::move(package))
    {
    }

    LirPackage Lir::Generate() const
    {
        LirLowering lowering;
        return lowering.Run(hir);
    }

    // Dump
    static std::string RegStr(LirReg r)
    {
        return r == LirNoReg ? "<void>" : std::format("%{}", r);
    }

    static std::string BlockLabel(const LirFunc& fn, std::uint32_t idx)
    {
        if (idx < fn.blocks.size()) return fn.blocks[idx].label;
        return std::format("bb{}", idx);
    }

    static void DumpInstr(std::ostream& out, const LirInstr& i, const LirFunc& fn)
    {
        out << "    ";
        switch (i.op)
        {
        case LirOpcode::Const:
            out << std::format("{} = const {} {}\n", RegStr(i.dst), i.type.ToString(), i.strArg);
            return;
        case LirOpcode::Alloca:
            out << std::format("{} = alloca {}\n", RegStr(i.dst), i.type.ToString());
            return;
        case LirOpcode::Load:
            if (!i.srcs.empty())
                out << std::format("{} = load {} {}\n", RegStr(i.dst), i.type.ToString(), RegStr(i.srcs[0]));
            else
                out << std::format("{} = load {} {}\n", RegStr(i.dst), i.type.ToString(), i.strArg);
            return;
        case LirOpcode::Store:
            out << std::format("store {} {}, {}\n",
                               i.type.ToString(),
                               !i.srcs.empty() ? RegStr(i.srcs[0]) : "?",
                               i.srcs.size() > 1 ? RegStr(i.srcs[1]) : "?");
            return;
        case LirOpcode::Cast:
            out << std::format("{} = cast {}: {} to {}\n",
                               RegStr(i.dst),
                               i.srcs.empty() ? "?" : RegStr(i.srcs[0]),
                               i.strArg,
                               i.type.ToString());
            return;

        case LirOpcode::Call:
        case LirOpcode::CallIndirect:
            {
                std::string args;
                const std::size_t first = (i.op == LirOpcode::CallIndirect) ? 1 : 0;
                for (std::size_t k = first; k < i.srcs.size(); ++k)
                {
                    if (k > first) args += ", ";
                    args += RegStr(i.srcs[k]);
                }
                if (i.dst == LirNoReg)
                {
                    if (i.op == LirOpcode::Call)
                        out << std::format("call {} @{}({})\n", i.type.ToString(), i.strArg, args);
                    else
                        out << std::format("call_ind {} {}({})\n", i.type.ToString(), RegStr(i.srcs[0]), args);
                }
                else
                {
                    if (i.op == LirOpcode::Call)
                        out << std::format("{} = call {} @{}({})\n", RegStr(i.dst), i.type.ToString(), i.strArg, args);
                    else
                        out << std::format("{} = call_ind {} {}({})\n", RegStr(i.dst), i.type.ToString(),
                                           RegStr(i.srcs[0]), args);
                }
                return;
            }

        case LirOpcode::FieldPtr:
            out << std::format("{} = fieldptr {} {}, {}\n",
                               RegStr(i.dst), i.type.ToString(),
                               i.srcs.empty() ? "?" : RegStr(i.srcs[0]),
                               i.strArg);
            return;

        case LirOpcode::IndexPtr:
            out << std::format("{} = indexptr {} {}, {}\n",
                               RegStr(i.dst), i.type.ToString(),
                               !i.srcs.empty() ? RegStr(i.srcs[0]) : "?",
                               i.srcs.size() > 1 ? RegStr(i.srcs[1]) : "?");
            return;

        case LirOpcode::Phi:
            {
                std::string preds;
                for (std::size_t k = 0; k < i.phiPreds.size(); ++k)
                {
                    if (k) preds += ", ";
                    preds += std::format("[{}, {}]",
                                         RegStr(i.phiPreds[k].first),
                                         BlockLabel(fn, i.phiPreds[k].second));
                }
                out << std::format("{} = phi {} {}\n", RegStr(i.dst), i.type.ToString(), preds);
                return;
            }
        default:
            {
                // Unary (one src) or binary (two srcs)
                std::string_view opName = OpcodeStr(i.op);
                if (i.srcs.size() == 1)
                    out << std::format("{} = {} {} {}\n",
                                       RegStr(i.dst), opName, i.type.ToString(), RegStr(i.srcs[0]));
                else
                    out << std::format("{} = {} {} {}, {}\n",
                                       RegStr(i.dst), opName, i.type.ToString(),
                                       RegStr(i.srcs[0]), RegStr(i.srcs[1]));
                return;
            }
        }
    }

    static void DumpTerminator(std::ostream& out, const LirTerminator& t, const LirFunc& fn)
    {
        out << "    ";
        switch (t.kind)
        {
        case LirTermKind::Jump:
            out << std::format("jmp {}\n", BlockLabel(fn, t.trueTarget));
            return;
        case LirTermKind::Branch:
            out << std::format("br {}, {}, {}\n",
                               RegStr(t.cond),
                               BlockLabel(fn, t.trueTarget),
                               BlockLabel(fn, t.falseTarget));
            return;
        case LirTermKind::Return:
            if (t.retVal)
                out << std::format("ret {} {}\n", t.retType.ToString(), RegStr(*t.retVal));
            else
                out << "ret void\n";
            return;
        case LirTermKind::Switch:
            {
                out << std::format("switch {} {}, default: {}",
                                   t.retType.ToString(),
                                   RegStr(t.cond),
                                   BlockLabel(fn, t.defaultTarget));
                for (const auto& c : t.cases)
                    out << std::format(", {}: {}", c.value, BlockLabel(fn, c.target));
                out << '\n';
                return;
            }
        }
    }

    static void DumpFunc(std::ostream& out, const LirFunc& fn)
    {
        std::string pub = fn.isPublic ? "pub " : "";
        std::string ext = fn.isExtern ? "extern " : "";
        std::string params;
        for (std::size_t i = 0; i < fn.params.size(); ++i)
        {
            if (i) params += ", ";
            params += std::format("{}: {}", RegStr(fn.params[i].reg), fn.params[i].type.ToString());
        }
        std::string ret = fn.returnType.IsOpaque() ? "" : " -> " + fn.returnType.ToString();
        out << std::format("\n{}{}func {}({}){}\n", pub, ext, fn.name, params, ret);
        for (const auto& block : fn.blocks)
        {
            out << std::format("  {}:\n", block.label);
            for (const auto& instr : block.instrs)
                DumpInstr(out, instr, fn);
            if (block.term)
                DumpTerminator(out, *block.term, fn);
        }
    }

    bool Lir::Dump(const LirPackage& package, const std::filesystem::path& path)
    {
        std::ofstream out(path);
        if (!out) return false;
        out << "=== Low-level Intermediate Representation ===\n";
        for (const auto& mod : package.modules)
        {
            out << '\n';
            out << std::format("Module \"{}\"\n", mod.name);
            out << std::string(std::min<std::size_t>(mod.name.size() + 9, 72), '-') << '\n';
            for (const auto& ta : mod.typeAliases)
            {
                std::string pub = ta.isPublic ? "pub " : "";
                out << std::format("\n{}type {} = {}\n", pub, ta.name, ta.type.ToString());
            }
            for (const auto& s : mod.structs)
            {
                std::string pub = s.isPublic ? "pub " : "";
                std::string typeParams;
                if (!s.typeParams.empty())
                {
                    typeParams = "<";
                    for (std::size_t i = 0; i < s.typeParams.size(); ++i)
                    {
                        if (i) typeParams += ", ";
                        typeParams += s.typeParams[i];
                    }
                    typeParams += ">";
                }
                out << std::format("\n{}struct {}{}\n", pub, s.name, typeParams);
                for (const auto& f : s.fields)
                    out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }
            for (const auto& e : mod.enums)
            {
                std::string pub = e.isPublic ? "pub " : "";
                out << std::format("\n{}enum {}\n", pub, e.name);
                for (const auto& v : e.variants)
                {
                    if (v.fields.empty())
                    {
                        out << std::format("  {}\n", v.name);
                    }
                    else
                    {
                        std::string fields;
                        for (std::size_t i = 0; i < v.fields.size(); ++i)
                        {
                            if (i) fields += ", ";
                            fields += v.fields[i].ToString();
                        }
                        out << std::format("  {}({})\n", v.name, fields);
                    }
                }
            }
            for (const auto& u : mod.unions)
            {
                std::string pub = u.isPublic ? "pub " : "";
                out << std::format("\n{}union {}\n", pub, u.name);
                for (const auto& f : u.fields)
                    out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }
            for (const auto& c : mod.consts)
            {
                std::string pub = c.isPublic ? "pub " : "";
                out << std::format("\n{}const {}: {} = {}\n",
                                   pub, c.name, c.type.ToString(), c.value);
            }
            for (const auto& ev : mod.externVars)
            {
                std::string pub = ev.isPublic ? "pub " : "";
                out << std::format("\nextern {}{}: {}\n", pub, ev.name, ev.type.ToString());
            }
            for (const auto& fn : mod.funcs)
                DumpFunc(out, fn);
        }

        return out.good();
    }
}
