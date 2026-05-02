/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Hir.h"

#include <cassert>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>

namespace Rux {
    // ── Internal: Symbol & Scope ──────────────────────────────────────────────────

    struct HirSymbol {
        enum class Kind { Var, Func, Type, Const, Interface };

        Kind kind = Kind::Var;
        std::string name;
        TypeRef type;
        bool isMut = false;
    };

    class HirScope {
    public:
        explicit HirScope(HirScope *parent = nullptr) : parent_(parent) {
        }

        void Define(HirSymbol sym) {
            table_.emplace(sym.name, std::move(sym));
        }

        HirSymbol *Lookup(const std::string &name) {
            auto it = table_.find(name);
            if (it != table_.end()) return &it->second;
            if (parent_) return parent_->Lookup(name);
            return nullptr;
        }

        HirScope *Parent() const { return parent_; }

    private:
        HirScope *parent_;
        std::unordered_map<std::string, HirSymbol> table_;
    };

    // ── Operator → string ─────────────────────────────────────────────────────────

    static std::string_view OpStr(TokenKind op) {
        using TK = TokenKind;
        switch (op) {
            case TK::Plus: return "+";
            case TK::Minus: return "-";
            case TK::Star: return "*";
            case TK::Slash: return "/";
            case TK::Percent: return "%";
            case TK::StarStar: return "**";
            case TK::Amp: return "&";
            case TK::Pipe: return "|";
            case TK::Caret: return "^";
            case TK::Tilde: return "~";
            case TK::LessLess: return "<<";
            case TK::GreaterGreater: return ">>";
            case TK::AmpAmp: return "&&";
            case TK::PipePipe: return "||";
            case TK::Bang: return "!";
            case TK::Equal: return "==";
            case TK::BangEqual: return "!=";
            case TK::Less: return "<";
            case TK::LessEqual: return "<=";
            case TK::Greater: return ">";
            case TK::GreaterEqual: return ">=";
            case TK::Assign: return "=";
            case TK::PlusAssign: return "+=";
            case TK::MinusAssign: return "-=";
            case TK::StarAssign: return "*=";
            case TK::SlashAssign: return "/=";
            case TK::PercentAssign: return "%=";
            case TK::AmpAssign: return "&=";
            case TK::PipeAssign: return "|=";
            case TK::CaretAssign: return "^=";
            case TK::LessLessAssign: return "<<=";
            case TK::GreaterGreaterAssign: return ">>=";
            default: return "?";
        }
    }

    // ── Internal: Lowering ────────────────────────────────────────────────────────

    class Lowering {
    public:
        explicit Lowering(std::vector<const Module *> &modules)
            : modules_(modules), currentScope_(&globalScope_) {
        }

        HirPackage Run() {
            RegisterBuiltins();
            for (auto *mod: modules_) CollectModule(*mod);

            HirPackage pkg;
            for (auto *mod: modules_)
                pkg.modules.push_back(LowerModule(*mod));
            return pkg;
        }

    private:
        std::vector<const Module *> &modules_;
        HirScope globalScope_{nullptr};
        HirScope *currentScope_;
        std::vector<std::unique_ptr<HirScope> > ownedScopes_;

        std::string currentFile_;
        TypeRef currentReturnType_ = TypeRef::MakeOpaque();
        bool inImpl_ = false;
        std::vector<std::string> currentTypeParams_;

        // ── Scope management ──────────────────────────────────────────────────────

        void PushScope() {
            ownedScopes_.push_back(std::make_unique<HirScope>(currentScope_));
            currentScope_ = ownedScopes_.back().get();
        }

        void PopScope() {
            assert(currentScope_->Parent() != nullptr && "cannot pop global scope");
            currentScope_ = currentScope_->Parent();
        }

        void Define(HirSymbol sym) {
            currentScope_->Define(std::move(sym));
        }

        // ── Builtins ──────────────────────────────────────────────────────────────

        void RegisterBuiltins() {
            auto add = [&](const char *name, TypeRef t) {
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Type;
                sym.name = name;
                sym.type = std::move(t);
                globalScope_.Define(std::move(sym));
            };
            add("opaque", TypeRef::MakeOpaque());
            add("bool8", TypeRef::MakeBool8());
            add("bool16", TypeRef::MakeBool16());
            add("bool32", TypeRef::MakeBool32());
            add("bool", TypeRef::MakeBool());
            add("char8", TypeRef::MakeChar8());
            add("char16", TypeRef::MakeChar16());
            add("char32", TypeRef::MakeChar32());
            add("char", TypeRef::MakeChar());
            add("String", TypeRef::MakeStr());
            add("int8", TypeRef::MakeInt8());
            add("int16", TypeRef::MakeInt16());
            add("int32", TypeRef::MakeInt32());
            add("int64", TypeRef::MakeInt64());
            add("uint8", TypeRef::MakeUInt8());
            add("uint16", TypeRef::MakeUInt16());
            add("uint32", TypeRef::MakeUInt32());
            add("uint64", TypeRef::MakeUInt64());
            add("float32", TypeRef::MakeFloat32());
            add("float64", TypeRef::MakeFloat64());
        }

        // ── First pass: collect global names ─────────────────────────────────────

        void CollectModule(const Module &mod) {
            currentFile_ = mod.name;
            for (const auto &decl: mod.items)
                CollectDecl(*decl);
        }

        void CollectDecl(const Decl &decl) {
            auto simple = [&](HirSymbol::Kind k, const std::string &name, TypeRef t = {}) {
                HirSymbol sym;
                sym.kind = k;
                sym.name = name;
                sym.type = std::move(t);
                globalScope_.Define(std::move(sym));
            };

            if (auto *d = dynamic_cast<const FuncDecl *>(&decl))
                simple(HirSymbol::Kind::Func, d->name);
            else if (auto *d = dynamic_cast<const StructDecl *>(&decl))
                simple(HirSymbol::Kind::Type, d->name, TypeRef::MakeNamed(d->name));
            else if (auto *d = dynamic_cast<const EnumDecl *>(&decl))
                simple(HirSymbol::Kind::Type, d->name, TypeRef::MakeNamed(d->name));
            else if (auto *d = dynamic_cast<const UnionDecl *>(&decl))
                simple(HirSymbol::Kind::Type, d->name, TypeRef::MakeNamed(d->name));
            else if (auto *d = dynamic_cast<const InterfaceDecl *>(&decl))
                simple(HirSymbol::Kind::Interface, d->name, TypeRef::MakeNamed(d->name));
            else if (auto *d = dynamic_cast<const ConstDecl *>(&decl))
                simple(HirSymbol::Kind::Const, d->name);
            else if (auto *d = dynamic_cast<const TypeAliasDecl *>(&decl))
                simple(HirSymbol::Kind::Type, d->name);
            else if (auto *d = dynamic_cast<const ExternFuncDecl *>(&decl))
                simple(HirSymbol::Kind::Func, d->name);
            else if (auto *d = dynamic_cast<const ExternVarDecl *>(&decl)) {
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Var;
                sym.name = d->name;
                sym.isMut = true;
                globalScope_.Define(std::move(sym));
            } else if (auto *d = dynamic_cast<const ExternBlockDecl *>(&decl)) {
                for (auto &item: d->items)
                    CollectDecl(*item);
            } else if (auto *d = dynamic_cast<const ModuleDecl *>(&decl)) {
                for (auto &item: d->items)
                    CollectDecl(*item);
            }
        }

        // ── Type resolution ───────────────────────────────────────────────────────

        TypeRef ResolveType(const TypeExpr &expr) {
            if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
                for (const auto &tp: currentTypeParams_)
                    if (tp == t->name) return TypeRef::MakeTypeParam(t->name);
                HirSymbol *sym = currentScope_->Lookup(t->name);
                if (sym && (sym->kind == HirSymbol::Kind::Type ||
                            sym->kind == HirSymbol::Kind::Interface)) {
                    if (!sym->type.IsUnknown()) return sym->type;
                    return TypeRef::MakeNamed(t->name);
                }
                return TypeRef::MakeNamed(t->name); // best-effort for unresolved names
            }
            if (auto *t = dynamic_cast<const PathTypeExpr *>(&expr))
                return TypeRef::MakeNamed(t->segments.back());
            if (auto *t = dynamic_cast<const PointerTypeExpr *>(&expr))
                return TypeRef::MakePointer(ResolveType(*t->pointee));
            if (auto *t = dynamic_cast<const ArrayTypeExpr *>(&expr))
                return TypeRef::MakeArray(ResolveType(*t->element));
            if (auto *t = dynamic_cast<const TupleTypeExpr *>(&expr)) {
                std::vector<TypeRef> elems;
                for (auto &e: t->elements)
                    elems.push_back(ResolveType(*e));
                return TypeRef::MakeTuple(std::move(elems));
            }
            if (dynamic_cast<const SelfTypeExpr *>(&expr))
                return TypeRef::MakeNamed("self");
            return TypeRef::MakeUnknown();
        }

        static std::string DecodeCharLiteral(const std::string &text) {
            // text is raw source like 'A' or '\n' — strip quotes and decode
            uint32_t cp = 0;
            if (text.size() >= 2) {
                std::size_t i = 1; // skip opening '
                if (text[i] == '\\' && i + 1 < text.size()) {
                    switch (text[i + 1]) {
                        case 'n':  cp = '\n'; break;
                        case 't':  cp = '\t'; break;
                        case 'r':  cp = '\r'; break;
                        case '0':  cp = 0;    break;
                        case '\\': cp = '\\'; break;
                        case '\'': cp = '\''; break;
                        case '"':  cp = '"';  break;
                        default:   cp = static_cast<unsigned char>(text[i + 1]); break;
                    }
                } else if (text[i] != '\'') {
                    cp = static_cast<unsigned char>(text[i]);
                }
            }
            return std::to_string(cp);
        }

        static std::string DecodeStringLiteral(const std::string &text) {
            // text is raw source like "hello\n" — strip quotes and decode escapes
            std::string out;
            if (text.size() < 2) return out;

            for (std::size_t i = 1; i + 1 < text.size(); ++i) {
                if (text[i] != '\\') {
                    out += text[i];
                    continue;
                }

                if (++i + 1 > text.size()) break;
                switch (text[i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '0': out += '\0'; break;
                    case '\\': out += '\\'; break;
                    case '\'': out += '\''; break;
                    case '"': out += '"'; break;
                    default: break;
                }
            }

            return out;
        }

        static TypeRef LiteralType(const Token &tok) {
            switch (tok.kind) {
                case TokenKind::IntLiteral: return TypeRef::MakeInt32();
                case TokenKind::FloatLiteral: return TypeRef::MakeFloat64();
                case TokenKind::StringLiteral: return TypeRef::MakeStr();
                case TokenKind::CharLiteral: return TypeRef::MakeChar();
                case TokenKind::BoolLiteral: return TypeRef::MakeBool();
                default: return TypeRef::MakeUnknown();
            }
        }

        std::vector<HirParam> LowerParams(const std::vector<Param> &params) {
            std::vector<HirParam> out;
            out.reserve(params.size());
            for (const auto &p: params) {
                HirParam hp;
                hp.name = p.name;
                hp.isVariadic = p.isVariadic;
                hp.type = p.isVariadic ? TypeRef::MakeUnknown() : ResolveType(*p.type);
                out.push_back(std::move(hp));
            }
            return out;
        }

        // ── Module lowering ───────────────────────────────────────────────────────

        HirModule LowerModule(const Module &mod) {
            currentFile_ = mod.name;
            HirModule hmod;
            hmod.name = mod.name;
            for (const auto &decl: mod.items)
                LowerTopLevelDecl(*decl, hmod);
            return hmod;
        }

        void LowerTopLevelDecl(const Decl &decl, HirModule &hmod) {
            if (auto *d = dynamic_cast<const FuncDecl *>(&decl))
                hmod.funcs.push_back(LowerFunc(*d));
            else if (auto *d = dynamic_cast<const StructDecl *>(&decl))
                hmod.structs.push_back(LowerStruct(*d));
            else if (auto *d = dynamic_cast<const EnumDecl *>(&decl))
                hmod.enums.push_back(LowerEnum(*d));
            else if (auto *d = dynamic_cast<const UnionDecl *>(&decl))
                hmod.unions.push_back(LowerUnion(*d));
            else if (auto *d = dynamic_cast<const InterfaceDecl *>(&decl))
                hmod.interfaces.push_back(LowerInterface(*d));
            else if (auto *d = dynamic_cast<const ImplDecl *>(&decl))
                hmod.impls.push_back(LowerImpl(*d));
            else if (auto *d = dynamic_cast<const ConstDecl *>(&decl))
                hmod.consts.push_back(LowerConst(*d));
            else if (auto *d = dynamic_cast<const ExternFuncDecl *>(&decl))
                hmod.externFuncs.push_back(LowerExternFunc(*d));
            else if (auto *d = dynamic_cast<const ExternVarDecl *>(&decl))
                hmod.externVars.push_back(LowerExternVar(*d));
            else if (auto *d = dynamic_cast<const ExternBlockDecl *>(&decl)) {
                for (auto &item: d->items)
                    LowerTopLevelDecl(*item, hmod);
            } else if (auto *d = dynamic_cast<const TypeAliasDecl *>(&decl))
                hmod.typeAliases.push_back(LowerTypeAlias(*d));
            else if (auto *d = dynamic_cast<const ModuleDecl *>(&decl)) {
                for (auto &item: d->items)
                    LowerTopLevelDecl(*item, hmod);
            }
            // UseDecl: resolved imports, no HIR representation
        }

        // ── Declaration lowering ──────────────────────────────────────────────────

        HirFunc LowerFunc(const FuncDecl &d, bool isMethod = false) {
            auto savedTypeParams = currentTypeParams_;
            currentTypeParams_ = d.typeParams;

            TypeRef retType = d.returnType
                                  ? ResolveType(*d.returnType->get())
                                  : TypeRef::MakeOpaque();

            auto savedRet = currentReturnType_;
            currentReturnType_ = retType;

            PushScope();

            for (const auto &tp: d.typeParams) {
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Type;
                sym.name = tp;
                sym.type = TypeRef::MakeTypeParam(tp);
                Define(sym);
            }

            if (isMethod) {
                HirSymbol self;
                self.kind = HirSymbol::Kind::Var;
                self.name = "self";
                self.type = TypeRef::MakeNamed("self");
                self.isMut = true;
                Define(self);
            }

            for (const auto &param: d.params) {
                if (param.isVariadic) continue;
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Var;
                sym.name = param.name;
                sym.type = ResolveType(*param.type);
                Define(sym);
            }

            std::optional<HirBlock> body;
            if (d.body)
                body = LowerBlock(*d.body);

            PopScope();
            currentReturnType_ = savedRet;
            currentTypeParams_ = savedTypeParams;

            HirFunc hf;
            hf.name = d.name;
            hf.isPublic = d.isPublic;
            hf.isAsm = d.isAsm;
            hf.callConv = d.callConv;
            hf.typeParams = d.typeParams;
            hf.params = LowerParams(d.params);
            hf.returnType = retType;
            hf.body = std::move(body);
            hf.location = d.location;
            return hf;
        }

        HirStruct LowerStruct(const StructDecl &d) {
            HirStruct hs;
            hs.name = d.name;
            hs.isPublic = d.isPublic;
            hs.location = d.location;
            for (const auto &f: d.fields) {
                HirStructField hf;
                hf.name = f.name;
                hf.isPublic = f.isPublic;
                hf.type = ResolveType(*f.type);
                hs.fields.push_back(std::move(hf));
            }
            return hs;
        }

        HirEnum LowerEnum(const EnumDecl &d) {
            HirEnum he;
            he.name = d.name;
            he.isPublic = d.isPublic;
            he.location = d.location;
            for (const auto &v: d.variants) {
                HirEnumVariant hv;
                hv.name = v.name;
                for (const auto &f: v.fields)
                    hv.fields.push_back(ResolveType(*f));
                he.variants.push_back(std::move(hv));
            }
            return he;
        }

        HirUnion LowerUnion(const UnionDecl &d) {
            HirUnion hu;
            hu.name = d.name;
            hu.isPublic = d.isPublic;
            hu.location = d.location;
            for (const auto &f: d.fields) {
                HirUnionField hf;
                hf.name = f.name;
                hf.type = ResolveType(*f.type);
                hu.fields.push_back(std::move(hf));
            }
            return hu;
        }

        HirInterface LowerInterface(const InterfaceDecl &d) {
            HirInterface hi;
            hi.name = d.name;
            hi.isPublic = d.isPublic;
            hi.location = d.location;
            for (const auto &m: d.methods) {
                HirInterfaceMethod hm;
                hm.name = m->name;
                hm.location = m->location;
                hm.returnType = m->returnType
                                    ? ResolveType(*m->returnType->get())
                                    : TypeRef::MakeOpaque();
                hm.params = LowerParams(m->params);
                hi.methods.push_back(std::move(hm));
            }
            return hi;
        }

        HirImplBlock LowerImpl(const ImplDecl &d) {
            bool savedInImpl = inImpl_;
            inImpl_ = true;

            HirImplBlock hib;
            hib.typeName = d.typeName;
            hib.interfaceName = d.interfaceName;
            hib.location = d.location;
            for (const auto &m: d.methods)
                hib.methods.push_back(LowerFunc(*m, /*isMethod=*/true));

            inImpl_ = savedInImpl;
            return hib;
        }

        HirConst LowerConst(const ConstDecl &d) {
            HirConst hc;
            hc.name = d.name;
            hc.isPublic = d.isPublic;
            hc.type = ResolveType(*d.type);
            hc.value = LowerExpr(*d.value);
            hc.location = d.location;
            return hc;
        }

        HirExternFunc LowerExternFunc(const ExternFuncDecl &d) {
            HirExternFunc hef;
            hef.name = d.name;
            hef.dll = d.dll;
            hef.isPublic = d.isPublic;
            hef.callConv = d.callConv;
            hef.isVariadic = d.isVariadic;
            hef.returnType = d.returnType
                                 ? ResolveType(*d.returnType->get())
                                 : TypeRef::MakeOpaque();
            hef.params = LowerParams(d.params);
            hef.location = d.location;
            return hef;
        }

        HirExternVar LowerExternVar(const ExternVarDecl &d) {
            HirExternVar hev;
            hev.name = d.name;
            hev.isPublic = d.isPublic;
            hev.type = ResolveType(*d.type);
            hev.location = d.location;
            return hev;
        }

        HirTypeAlias LowerTypeAlias(const TypeAliasDecl &d) {
            HirTypeAlias hta;
            hta.name = d.name;
            hta.isPublic = d.isPublic;
            hta.type = ResolveType(*d.type);
            hta.location = d.location;
            return hta;
        }

        // ── Block & statement lowering ────────────────────────────────────────────

        HirBlock LowerBlock(const Block &block) {
            HirBlock hb;
            hb.location = block.location;
            PushScope();
            for (const auto &stmt: block.stmts)
                hb.stmts.push_back(LowerStmt(*stmt));
            PopScope();
            return hb;
        }

        HirStmtPtr LowerStmt(const Stmt &stmt) {
            if (auto *s = dynamic_cast<const ExprStmt *>(&stmt)) {
                auto hs = std::make_unique<HirExprStmt>();
                hs->location = s->location;
                hs->expr = LowerExpr(*s->expr);
                return hs;
            }

            if (auto *s = dynamic_cast<const LetStmt *>(&stmt)) {
                auto hs = std::make_unique<HirLetStmt>();
                hs->location = s->location;
                hs->isMut = s->isMut;
                hs->name = s->name;
                hs->init = LowerExpr(*s->init);
                hs->type = s->type
                               ? ResolveType(*s->type->get())
                               : hs->init->type;

                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Var;
                sym.name = s->name;
                sym.type = hs->type;
                sym.isMut = s->isMut;
                Define(sym);
                return hs;
            }

            if (auto *s = dynamic_cast<const IfStmt *>(&stmt)) {
                auto hs = std::make_unique<HirIfStmt>();
                hs->location = s->location;
                hs->condition = LowerExpr(*s->condition);
                hs->thenBlock = LowerBlock(*s->thenBlock);
                for (const auto &elif: s->elseIfs) {
                    HirIfStmt::ElseIf hElif;
                    hElif.location = elif.location;
                    hElif.condition = LowerExpr(*elif.condition);
                    hElif.block = LowerBlock(*elif.block);
                    hs->elseIfs.push_back(std::move(hElif));
                }
                if (s->elseBlock)
                    hs->elseBlock = LowerBlock(*s->elseBlock);
                return hs;
            }

            if (auto *s = dynamic_cast<const WhileStmt *>(&stmt)) {
                auto hs = std::make_unique<HirWhileStmt>();
                hs->location = s->location;
                hs->condition = LowerExpr(*s->condition);
                hs->body = LowerBlock(*s->body);
                return hs;
            }

            if (auto *s = dynamic_cast<const ForStmt *>(&stmt)) {
                auto hs = std::make_unique<HirForStmt>();
                hs->location = s->location;
                hs->variable = s->variable;
                hs->iterable = LowerExpr(*s->iterable);
                hs->varType = TypeRef::MakeUnknown();

                PushScope();
                HirSymbol var;
                var.kind = HirSymbol::Kind::Var;
                var.name = s->variable;
                var.type = TypeRef::MakeUnknown();
                Define(var);

                hs->body = LowerBlock(*s->body);
                PopScope();
                return hs;
            }

            if (auto *s = dynamic_cast<const MatchStmt *>(&stmt)) {
                auto hs = std::make_unique<HirMatchStmt>();
                hs->location = s->location;
                hs->subject = LowerExpr(*s->subject);
                for (const auto &arm: s->arms) {
                    HirMatchArm ha;
                    ha.location = arm.location;
                    PushScope();
                    ha.pattern = LowerPattern(*arm.pattern);
                    ha.body = LowerExpr(*arm.body);
                    PopScope();
                    hs->arms.push_back(std::move(ha));
                }
                return hs;
            }

            if (auto *s = dynamic_cast<const ReturnStmt *>(&stmt)) {
                auto hs = std::make_unique<HirReturnStmt>();
                hs->location = s->location;
                if (s->value)
                    hs->value = LowerExpr(**s->value);
                return hs;
            }

            if (dynamic_cast<const BreakStmt *>(&stmt)) {
                auto hs = std::make_unique<HirBreakStmt>();
                hs->location = stmt.location;
                return hs;
            }

            if (dynamic_cast<const ContinueStmt *>(&stmt)) {
                auto hs = std::make_unique<HirContinueStmt>();
                hs->location = stmt.location;
                return hs;
            }

            if (auto *s = dynamic_cast<const DeclStmt *>(&stmt)) {
                auto hs = std::make_unique<HirLocalDecl>();
                hs->location = s->location;
                if (auto *fd = dynamic_cast<const FuncDecl *>(s->decl.get()))
                    hs->description = std::format("func {}", fd->name);
                else if (auto *cd = dynamic_cast<const ConstDecl *>(s->decl.get()))
                    hs->description = std::format("const {}", cd->name);
                else if (auto *ta = dynamic_cast<const TypeAliasDecl *>(s->decl.get()))
                    hs->description = std::format("type {}", ta->name);
                else
                    hs->description = "<local decl>";
                CollectDecl(*s->decl);
                return hs;
            }

            // Unreachable in valid AST
            auto hs = std::make_unique<HirLocalDecl>();
            hs->location = stmt.location;
            hs->description = "<unknown stmt>";
            return hs;
        }

        // ── Expression lowering ───────────────────────────────────────────────────

        HirExprPtr LowerExpr(const Expr &expr) {
            if (auto *e = dynamic_cast<const LiteralExpr *>(&expr)) {
                auto he = std::make_unique<HirLiteralExpr>();
                he->location = e->location;
                he->type = LiteralType(e->token);
                if (e->token.kind == TokenKind::CharLiteral)
                    he->value = DecodeCharLiteral(e->token.text);
                else if (e->token.kind == TokenKind::StringLiteral)
                    he->value = DecodeStringLiteral(e->token.text);
                else
                    he->value = e->token.text;
                return he;
            }

            if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
                auto he = std::make_unique<HirVarExpr>();
                he->location = e->location;
                he->name = e->name;
                if (HirSymbol *sym = currentScope_->Lookup(e->name))
                    he->type = sym->type;
                return he;
            }

            if (dynamic_cast<const SelfExpr *>(&expr)) {
                auto he = std::make_unique<HirSelfExpr>();
                he->location = expr.location;
                he->type = TypeRef::MakeNamed("self");
                return he;
            }

            if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
                auto he = std::make_unique<HirPathExpr>();
                he->location = e->location;
                he->segments = e->segments;
                if (!e->segments.empty())
                    if (HirSymbol *sym = currentScope_->Lookup(e->segments[0]))
                        he->type = sym->type;
                return he;
            }

            if (auto *e = dynamic_cast<const UnaryExpr *>(&expr)) {
                auto he = std::make_unique<HirUnaryExpr>();
                he->location = e->location;
                he->op = e->op;
                he->operand = LowerExpr(*e->operand);
                he->type = InferUnaryType(e->op, he->operand->type);
                return he;
            }

            if (auto *e = dynamic_cast<const BinaryExpr *>(&expr)) {
                auto he = std::make_unique<HirBinaryExpr>();
                he->location = e->location;
                he->op = e->op;
                he->left = LowerExpr(*e->left);
                he->right = LowerExpr(*e->right);
                he->type = InferBinaryType(e->op, he->left->type, he->right->type);
                return he;
            }

            if (auto *e = dynamic_cast<const AssignExpr *>(&expr)) {
                auto he = std::make_unique<HirAssignExpr>();
                he->location = e->location;
                he->op = e->op;
                he->target = LowerExpr(*e->target);
                he->value = LowerExpr(*e->value);
                he->type = TypeRef::MakeOpaque();
                return he;
            }

            if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
                auto he = std::make_unique<HirTernaryExpr>();
                he->location = e->location;
                he->condition = LowerExpr(*e->condition);
                he->thenExpr = LowerExpr(*e->thenExpr);
                he->elseExpr = LowerExpr(*e->elseExpr);
                he->type = he->thenExpr->type.IsUnknown()
                               ? he->elseExpr->type
                               : he->thenExpr->type;
                return he;
            }

            if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
                auto he = std::make_unique<HirRangeExpr>();
                he->location = e->location;
                he->inclusive = e->inclusive;
                if (e->lo) he->lo = LowerExpr(*e->lo);
                if (e->hi) he->hi = LowerExpr(*e->hi);
                he->type = TypeRef::MakeNamed("Range");
                return he;
            }

            if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
                auto he = std::make_unique<HirCallExpr>();
                he->location = e->location;
                he->callee = LowerExpr(*e->callee);
                for (const auto &arg: e->args)
                    he->args.push_back(LowerExpr(*arg));
                // Propagate return type if callee is a known func type
                if (he->callee->type.kind == TypeRef::Kind::Func &&
                    !he->callee->type.inner.empty())
                    he->type = he->callee->type.inner.back();
                return he;
            }

            if (auto *e = dynamic_cast<const IndexExpr *>(&expr)) {
                auto he = std::make_unique<HirIndexExpr>();
                he->location = e->location;
                he->object = LowerExpr(*e->object);
                he->index = LowerExpr(*e->index);
                if (he->object->type.kind == TypeRef::Kind::Array &&
                    !he->object->type.inner.empty())
                    he->type = he->object->type.inner[0];
                return he;
            }

            if (auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
                auto he = std::make_unique<HirFieldExpr>();
                he->location = e->location;
                he->object = LowerExpr(*e->object);
                he->field = e->field;
                // Field type resolution requires full struct layout info; mark unknown
                return he;
            }

            if (auto *e = dynamic_cast<const StructInitExpr *>(&expr)) {
                auto he = std::make_unique<HirStructInitExpr>();
                he->location = e->location;
                he->typeName = e->typeName;
                he->type = TypeRef::MakeNamed(e->typeName);
                for (const auto &f: e->fields) {
                    HirStructInitField hf;
                    hf.name = f.name;
                    hf.value = LowerExpr(*f.value);
                    he->fields.push_back(std::move(hf));
                }
                return he;
            }

            if (auto *e = dynamic_cast<const ArrayExpr *>(&expr)) {
                auto he = std::make_unique<HirArrayExpr>();
                he->location = e->location;
                TypeRef elemType = TypeRef::MakeUnknown();
                for (const auto &el: e->elements) {
                    he->elements.push_back(LowerExpr(*el));
                    if (elemType.IsUnknown()) elemType = he->elements.back()->type;
                }
                he->type = TypeRef::MakeArray(elemType);
                return he;
            }

            if (auto *e = dynamic_cast<const CastExpr *>(&expr)) {
                auto he = std::make_unique<HirCastExpr>();
                he->location = e->location;
                he->operand = LowerExpr(*e->operand);
                he->targetType = ResolveType(*e->type);
                he->type = he->targetType;
                return he;
            }

            if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
                auto he = std::make_unique<HirIsExpr>();
                he->location = e->location;
                he->operand = LowerExpr(*e->operand);
                he->checkType = ResolveType(*e->type);
                he->type = TypeRef::MakeBool();
                return he;
            }

            if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
                auto he = std::make_unique<HirBlockExpr>();
                he->location = e->location;
                he->block = LowerBlock(*e->block);
                return he;
            }

            // Fallback for unrecognized expression kinds
            auto he = std::make_unique<HirLiteralExpr>();
            he->location = expr.location;
            he->value = "<expr>";
            return he;
        }

        TypeRef InferUnaryType(TokenKind op, const TypeRef &t) {
            switch (op) {
                case TokenKind::Bang: return TypeRef::MakeBool();
                case TokenKind::Amp: return TypeRef::MakePointer(t);
                case TokenKind::Star: return t.inner.empty() ? TypeRef::MakeUnknown() : t.inner[0];
                default: return t;
            }
        }

        TypeRef InferBinaryType(TokenKind op, const TypeRef &l, const TypeRef &r) {
            using TK = TokenKind;
            switch (op) {
                case TK::AmpAmp:
                case TK::PipePipe:
                case TK::Equal:
                case TK::BangEqual:
                case TK::Less:
                case TK::LessEqual:
                case TK::Greater:
                case TK::GreaterEqual:
                    return TypeRef::MakeBool();
                default:
                    return l.IsUnknown() ? r : l;
            }
        }

        // ── Pattern lowering ──────────────────────────────────────────────────────

        HirPatternPtr LowerPattern(const Pattern &pat) {
            if (dynamic_cast<const WildcardPattern *>(&pat)) {
                auto hp = std::make_unique<HirWildcardPattern>();
                hp->location = pat.location;
                return hp;
            }

            if (auto *p = dynamic_cast<const LiteralPattern *>(&pat)) {
                auto hp = std::make_unique<HirLiteralPattern>();
                hp->location = p->location;
                hp->value = p->value.text;
                hp->type = LiteralType(p->value);
                return hp;
            }

            if (auto *p = dynamic_cast<const IdentPattern *>(&pat)) {
                auto hp = std::make_unique<HirBindingPattern>();
                hp->location = p->location;
                hp->name = p->name;
                HirSymbol sym;
                sym.kind = HirSymbol::Kind::Var;
                sym.name = p->name;
                sym.type = TypeRef::MakeUnknown();
                Define(sym);
                return hp;
            }

            if (auto *p = dynamic_cast<const RangePattern *>(&pat)) {
                auto hp = std::make_unique<HirRangePattern>();
                hp->location = p->location;
                hp->inclusive = p->inclusive;
                hp->lo = LowerPattern(*p->lo);
                hp->hi = LowerPattern(*p->hi);
                return hp;
            }

            if (auto *p = dynamic_cast<const EnumPattern *>(&pat)) {
                auto hp = std::make_unique<HirEnumPattern>();
                hp->location = p->location;
                hp->path = p->path;
                if (!p->path.empty())
                    if (HirSymbol *sym = currentScope_->Lookup(p->path[0]))
                        hp->resolvedType = sym->type;
                for (const auto &a: p->args)
                    hp->args.push_back(LowerPattern(*a));
                return hp;
            }

            if (auto *p = dynamic_cast<const StructPattern *>(&pat)) {
                auto hp = std::make_unique<HirStructPattern>();
                hp->location = p->location;
                hp->typeName = p->typeName;
                if (HirSymbol *sym = currentScope_->Lookup(p->typeName))
                    hp->resolvedType = sym->type;
                for (const auto &f: p->fields) {
                    HirStructPatternField hf;
                    hf.name = f.name;
                    hf.pattern = LowerPattern(*f.pattern);
                    hp->fields.push_back(std::move(hf));
                }
                return hp;
            }

            if (auto *p = dynamic_cast<const TuplePattern *>(&pat)) {
                auto hp = std::make_unique<HirTuplePattern>();
                hp->location = p->location;
                for (const auto &e: p->elements)
                    hp->elements.push_back(LowerPattern(*e));
                return hp;
            }

            if (auto *p = dynamic_cast<const GuardedPattern *>(&pat)) {
                auto hp = std::make_unique<HirGuardedPattern>();
                hp->location = p->location;
                hp->inner = LowerPattern(*p->inner);
                hp->guard = LowerExpr(*p->guard);
                return hp;
            }

            auto hp = std::make_unique<HirWildcardPattern>();
            hp->location = pat.location;
            return hp;
        }
    };

    // ── Hir public API ────────────────────────────────────────────────────────────

    Hir::Hir(std::vector<const Module *> modules)
        : modules_(std::move(modules)) {
    }

    HirPackage Hir::Generate() {
        Lowering lowering(modules_);
        return lowering.Run();
    }

    // ── Dump ──────────────────────────────────────────────────────────────────────

    static std::string PrintPattern(const HirPattern &pat);

    static std::string PrintExpr(const HirExpr &expr);

    static std::string PrintExpr(const HirExpr &expr) {
        if (auto *e = dynamic_cast<const HirLiteralExpr *>(&expr))
            return e->value;

        if (auto *e = dynamic_cast<const HirVarExpr *>(&expr))
            return e->name;

        if (dynamic_cast<const HirSelfExpr *>(&expr))
            return "self";

        if (auto *e = dynamic_cast<const HirPathExpr *>(&expr)) {
            std::string s;
            for (std::size_t i = 0; i < e->segments.size(); ++i) {
                if (i) s += "::";
                s += e->segments[i];
            }
            return s;
        }

        if (auto *e = dynamic_cast<const HirUnaryExpr *>(&expr))
            return std::string(OpStr(e->op)) + PrintExpr(*e->operand);

        if (auto *e = dynamic_cast<const HirBinaryExpr *>(&expr))
            return std::format("({} {} {})",
                               PrintExpr(*e->left), OpStr(e->op), PrintExpr(*e->right));

        if (auto *e = dynamic_cast<const HirAssignExpr *>(&expr))
            return std::format("{} {} {}",
                               PrintExpr(*e->target), OpStr(e->op), PrintExpr(*e->value));

        if (auto *e = dynamic_cast<const HirTernaryExpr *>(&expr))
            return std::format("{} ? {} : {}",
                               PrintExpr(*e->condition),
                               PrintExpr(*e->thenExpr),
                               PrintExpr(*e->elseExpr));

        if (auto *e = dynamic_cast<const HirRangeExpr *>(&expr)) {
            std::string lo = e->lo ? PrintExpr(*e->lo) : "";
            std::string hi = e->hi ? PrintExpr(*e->hi) : "";
            return lo + (e->inclusive ? "..." : "..") + hi;
        }

        if (auto *e = dynamic_cast<const HirCallExpr *>(&expr)) {
            std::string s = PrintExpr(*e->callee) + "(";
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                if (i) s += ", ";
                s += PrintExpr(*e->args[i]);
            }
            return s + ")";
        }

        if (auto *e = dynamic_cast<const HirIndexExpr *>(&expr))
            return std::format("{}[{}]", PrintExpr(*e->object), PrintExpr(*e->index));

        if (auto *e = dynamic_cast<const HirFieldExpr *>(&expr))
            return std::format("{}.{}", PrintExpr(*e->object), e->field);

        if (auto *e = dynamic_cast<const HirStructInitExpr *>(&expr)) {
            std::string s = e->typeName + " { ";
            for (std::size_t i = 0; i < e->fields.size(); ++i) {
                if (i) s += ", ";
                s += e->fields[i].name + ": " + PrintExpr(*e->fields[i].value);
            }
            return s + " }";
        }

        if (auto *e = dynamic_cast<const HirArrayExpr *>(&expr)) {
            std::string s = "[";
            for (std::size_t i = 0; i < e->elements.size(); ++i) {
                if (i) s += ", ";
                s += PrintExpr(*e->elements[i]);
            }
            return s + "]";
        }

        if (auto *e = dynamic_cast<const HirCastExpr *>(&expr))
            return std::format("{} as {}", PrintExpr(*e->operand), e->targetType.ToString());

        if (auto *e = dynamic_cast<const HirIsExpr *>(&expr))
            return std::format("{} is {}", PrintExpr(*e->operand), e->checkType.ToString());

        if (dynamic_cast<const HirBlockExpr *>(&expr))
            return "{ ... }";

        return "<expr>";
    }

    static std::string PrintPattern(const HirPattern &pat) {
        if (dynamic_cast<const HirWildcardPattern *>(&pat))
            return "_";

        if (auto *p = dynamic_cast<const HirLiteralPattern *>(&pat))
            return p->value;

        if (auto *p = dynamic_cast<const HirBindingPattern *>(&pat))
            return p->name;

        if (auto *p = dynamic_cast<const HirRangePattern *>(&pat)) {
            std::string lo = p->lo ? PrintPattern(*p->lo) : "";
            std::string hi = p->hi ? PrintPattern(*p->hi) : "";
            return lo + (p->inclusive ? "..." : "..") + hi;
        }

        if (auto *p = dynamic_cast<const HirEnumPattern *>(&pat)) {
            std::string s;
            for (std::size_t i = 0; i < p->path.size(); ++i) {
                if (i) s += ".";
                s += p->path[i];
            }
            if (!p->args.empty()) {
                s += "(";
                for (std::size_t i = 0; i < p->args.size(); ++i) {
                    if (i) s += ", ";
                    s += PrintPattern(*p->args[i]);
                }
                s += ")";
            }
            return s;
        }

        if (auto *p = dynamic_cast<const HirStructPattern *>(&pat)) {
            std::string s = p->typeName + " { ";
            for (std::size_t i = 0; i < p->fields.size(); ++i) {
                if (i) s += ", ";
                s += p->fields[i].name + ": " + PrintPattern(*p->fields[i].pattern);
            }
            return s + " }";
        }

        if (auto *p = dynamic_cast<const HirTuplePattern *>(&pat)) {
            std::string s = "(";
            for (std::size_t i = 0; i < p->elements.size(); ++i) {
                if (i) s += ", ";
                s += PrintPattern(*p->elements[i]);
            }
            return s + ")";
        }

        if (auto *p = dynamic_cast<const HirGuardedPattern *>(&pat))
            return PrintPattern(*p->inner) + " if " + PrintExpr(*p->guard);

        return "_";
    }

    static void DumpBlock(std::ostream &out, const HirBlock &block, const std::string &indent);

    static void DumpStmt(std::ostream &out, const HirStmt &stmt, const std::string &indent);

    static void DumpBlock(std::ostream &out, const HirBlock &block, const std::string &indent) {
        for (const auto &stmt: block.stmts)
            DumpStmt(out, *stmt, indent);
    }

    static void DumpStmt(std::ostream &out, const HirStmt &stmt, const std::string &indent) {
        if (auto *s = dynamic_cast<const HirExprStmt *>(&stmt)) {
            out << indent << PrintExpr(*s->expr) << '\n';
            return;
        }

        if (auto *s = dynamic_cast<const HirLetStmt *>(&stmt)) {
            std::string mut = s->isMut ? "mut " : "";
            out << std::format("{}let {}{}: {} = {}\n",
                               indent, mut, s->name, s->type.ToString(), PrintExpr(*s->init));
            return;
        }

        if (auto *s = dynamic_cast<const HirIfStmt *>(&stmt)) {
            out << std::format("{}if {}\n", indent, PrintExpr(*s->condition));
            DumpBlock(out, s->thenBlock, indent + "  ");
            for (const auto &elif: s->elseIfs) {
                out << std::format("{}else if {}\n", indent, PrintExpr(*elif.condition));
                DumpBlock(out, elif.block, indent + "  ");
            }
            if (s->elseBlock) {
                out << indent << "else\n";
                DumpBlock(out, *s->elseBlock, indent + "  ");
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirWhileStmt *>(&stmt)) {
            out << std::format("{}while {}\n", indent, PrintExpr(*s->condition));
            DumpBlock(out, s->body, indent + "  ");
            return;
        }

        if (auto *s = dynamic_cast<const HirForStmt *>(&stmt)) {
            out << std::format("{}for {} in {}\n",
                               indent, s->variable, PrintExpr(*s->iterable));
            DumpBlock(out, s->body, indent + "  ");
            return;
        }

        if (auto *s = dynamic_cast<const HirMatchStmt *>(&stmt)) {
            out << std::format("{}match {}\n", indent, PrintExpr(*s->subject));
            for (const auto &arm: s->arms) {
                out << std::format("{}  {} =>\n",
                                   indent, PrintPattern(*arm.pattern));
                out << std::format("{}    {}\n", indent, PrintExpr(*arm.body));
            }
            return;
        }

        if (auto *s = dynamic_cast<const HirReturnStmt *>(&stmt)) {
            if (s->value)
                out << std::format("{}return {}\n", indent, PrintExpr(**s->value));
            else
                out << indent << "return\n";
            return;
        }

        if (dynamic_cast<const HirBreakStmt *>(&stmt)) {
            out << indent << "break\n";
            return;
        }

        if (dynamic_cast<const HirContinueStmt *>(&stmt)) {
            out << indent << "continue\n";
            return;
        }

        if (auto *s = dynamic_cast<const HirLocalDecl *>(&stmt)) {
            out << std::format("{}[local {}]\n", indent, s->description);
            return;
        }
    }

    static void DumpFuncSignature(std::ostream &out, const HirFunc &f, const std::string &prefix = "") {
        std::string pub = f.isPublic ? "pub " : "";
        std::string asm_ = f.isAsm ? "asm " : "";
        std::string tps;
        if (!f.typeParams.empty()) {
            tps = "<";
            for (std::size_t i = 0; i < f.typeParams.size(); ++i) {
                if (i) tps += ", ";
                tps += f.typeParams[i];
            }
            tps += ">";
        }
        std::string params;
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i) params += ", ";
            if (f.params[i].isVariadic)
                params += "...";
            else
                params += f.params[i].name + ": " + f.params[i].type.ToString();
        }
        std::string ret = f.returnType.IsOpaque()
                              ? ""
                              : " -> " + f.returnType.ToString();
        out << std::format("{}{}{}func {}{}{}{}\n",
                           prefix, pub, asm_, f.name, tps,
                           params.empty() ? "()" : "(" + params + ")", ret);
    }

    bool Hir::Dump(const HirPackage &package, const std::filesystem::path &path) {
        std::ofstream out(path);
        if (!out) return false;

        out << "=== High-level Intermediate Representation ===\n";

        for (const auto &mod: package.modules) {
            out << '\n';
            out << std::format("Module \"{}\"\n", mod.name);
            out << std::string(std::min<std::size_t>(mod.name.size() + 9, 72), '-') << '\n';

            for (const auto &c: mod.consts) {
                std::string pub = c.isPublic ? "pub " : "";
                out << std::format("\n{}const {}: {} = {}\n",
                                   pub, c.name, c.type.ToString(), PrintExpr(*c.value));
            }

            for (const auto &ta: mod.typeAliases) {
                std::string pub = ta.isPublic ? "pub " : "";
                out << std::format("\n{}type {} = {}\n",
                                   pub, ta.name, ta.type.ToString());
            }

            for (const auto &ev: mod.externVars) {
                std::string pub = ev.isPublic ? "pub " : "";
                out << std::format("\nextern {}{}: {}\n",
                                   pub, ev.name, ev.type.ToString());
            }

            for (const auto &ef: mod.externFuncs) {
                std::string pub = ef.isPublic ? "pub " : "";
                std::string params;
                for (std::size_t i = 0; i < ef.params.size(); ++i) {
                    if (i) params += ", ";
                    if (ef.params[i].isVariadic) params += "...";
                    else params += ef.params[i].name + ": " + ef.params[i].type.ToString();
                }
                if (ef.isVariadic && !ef.params.empty()) params += ", ...";
                std::string ret = ef.returnType.IsOpaque()
                                      ? ""
                                      : " -> " + ef.returnType.ToString();
                std::string attr;
                if (!ef.dll.empty())
                    attr += std::format("@[Import(lib: \"{}\")]\n", ef.dll);
                if (ef.callConv == CallingConvention::Win64)
                    attr += "@[Call(.Win64)]\n";
                out << std::format("\n{}extern {}func {}({}){}\n",
                                   attr, pub, ef.name, params, ret);
            }

            for (const auto &s: mod.structs) {
                std::string pub = s.isPublic ? "pub " : "";
                out << std::format("\n{}struct {}\n", pub, s.name);
                for (const auto &f: s.fields) {
                    std::string fpub = f.isPublic ? "pub " : "";
                    out << std::format("  {}{}: {}\n", fpub, f.name, f.type.ToString());
                }
            }

            for (const auto &e: mod.enums) {
                std::string pub = e.isPublic ? "pub " : "";
                out << std::format("\n{}enum {}\n", pub, e.name);
                for (const auto &v: e.variants) {
                    if (v.fields.empty()) {
                        out << std::format("  {}\n", v.name);
                    } else {
                        std::string fields;
                        for (std::size_t i = 0; i < v.fields.size(); ++i) {
                            if (i) fields += ", ";
                            fields += v.fields[i].ToString();
                        }
                        out << std::format("  {}({})\n", v.name, fields);
                    }
                }
            }

            for (const auto &u: mod.unions) {
                std::string pub = u.isPublic ? "pub " : "";
                out << std::format("\n{}union {}\n", pub, u.name);
                for (const auto &f: u.fields)
                    out << std::format("  {}: {}\n", f.name, f.type.ToString());
            }

            for (const auto &iface: mod.interfaces) {
                std::string pub = iface.isPublic ? "pub " : "";
                out << std::format("\n{}interface {}\n", pub, iface.name);
                for (const auto &m: iface.methods) {
                    std::string params;
                    for (std::size_t i = 0; i < m.params.size(); ++i) {
                        if (i) params += ", ";
                        if (m.params[i].isVariadic) params += "...";
                        else params += m.params[i].name + ": " + m.params[i].type.ToString();
                    }
                    std::string ret = m.returnType.IsOpaque()
                                          ? ""
                                          : " -> " + m.returnType.ToString();
                    out << std::format("  func {}({}){}\n", m.name, params, ret);
                }
            }

            for (const auto &impl: mod.impls) {
                out << '\n';
                if (impl.interfaceName)
                    out << std::format("impl {} for {}\n", impl.typeName, *impl.interfaceName);
                else
                    out << std::format("impl {}\n", impl.typeName);
                for (const auto &m: impl.methods) {
                    DumpFuncSignature(out, m, "  ");
                    if (m.body)
                        DumpBlock(out, *m.body, "    ");
                }
            }

            for (const auto &f: mod.funcs) {
                out << '\n';
                DumpFuncSignature(out, f);
                if (f.body)
                    DumpBlock(out, *f.body, "  ");
            }
        }

        return out.good();
    }
} // namespace Rux
