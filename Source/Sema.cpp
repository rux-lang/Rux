/*
    Rux Compiler
    Copyright © 2026 Ivan Muzyka
    Licensed under the MIT License
*/

#include "Rux/Sema.h"
#include "Rux/Type.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace Rux {
    // ── TypeRef implementation ────────────────────────────────────────────────────

    bool TypeRef::IsNumeric() const noexcept {
        switch (kind) {
            case Kind::Int8:
            case Kind::Int16:
            case Kind::Int32:
            case Kind::Int64:
            case Kind::UInt8:
            case Kind::UInt16:
            case Kind::UInt32:
            case Kind::UInt64:
            case Kind::Float32:
            case Kind::Float64:
                return true;
            default: return false;
        }
    }

    bool TypeRef::IsInteger() const noexcept {
        switch (kind) {
            case Kind::Int8:
            case Kind::Int16:
            case Kind::Int32:
            case Kind::Int64:
            case Kind::UInt8:
            case Kind::UInt16:
            case Kind::UInt32:
            case Kind::UInt64:
                return true;
            default: return false;
        }
    }

    bool TypeRef::IsFloat() const noexcept {
        return kind == Kind::Float32 || kind == Kind::Float64;
    }

    bool TypeRef::IsSigned() const noexcept {
        switch (kind) {
            case Kind::Int8:
            case Kind::Int16:
            case Kind::Int32:
            case Kind::Int64:
                return true;
            default: return false;
        }
    }

    bool TypeRef::IsAssignableTo(const TypeRef &other) const noexcept {
        if (IsUnknown() || other.IsUnknown()) return true;
        if (*this == other) return true;
        // Numeric types are mutually assignable (widening/narrowing checked later)
        if (IsNumeric() && other.IsNumeric()) return true;
        return false;
    }

    std::string TypeRef::ToString() const {
        switch (kind) {
            case Kind::Unknown: return "?";
            case Kind::Void: return "void";
            case Kind::Bool: return "bool";
            case Kind::Char: return "char";
            case Kind::Str: return "String";
            case Kind::Int8: return "int8";
            case Kind::Int16: return "int16";
            case Kind::Int32: return "int32";
            case Kind::Int64: return "int64";
            case Kind::UInt8: return "uint8";
            case Kind::UInt16: return "uint16";
            case Kind::UInt32: return "uint32";
            case Kind::UInt64: return "uint64";
            case Kind::Float32: return "float32";
            case Kind::Float64: return "float64";
            case Kind::Named: return name;
            case Kind::TypeParam: return name;
            case Kind::Pointer: return "*" + (inner.empty() ? "?" : inner[0].ToString());
            case Kind::Array: return (inner.empty() ? "?" : inner[0].ToString()) + "[]";
            case Kind::Tuple: {
                std::string s = "(";
                for (std::size_t i = 0; i < inner.size(); ++i) {
                    if (i) s += ", ";
                    s += inner[i].ToString();
                }
                return s + ")";
            }
            case Kind::Func: {
                std::string s = "func(";
                for (std::size_t i = 0; i + 1 < inner.size(); ++i) {
                    if (i) s += ", ";
                    s += inner[i].ToString();
                }
                s += ") -> ";
                s += inner.empty() ? "void" : inner.back().ToString();
                return s;
            }
        }
        return "?";
    }

    bool TypeRef::operator==(const TypeRef &o) const noexcept {
        if (kind != o.kind || name != o.name || inner.size() != o.inner.size()) return false;
        for (std::size_t i = 0; i < inner.size(); ++i)
            if (inner[i] != o.inner[i]) return false;
        return true;
    }

    // ── SemaResult ────────────────────────────────────────────────────────────────

    bool SemaResult::HasErrors() const noexcept {
        return std::ranges::any_of(diagnostics,
                                   [](const SemaDiagnostic &d) {
                                       return d.severity == SemaDiagnostic::Severity::Error;
                                   });
    }

    // ── Internal: Symbol & Scope ──────────────────────────────────────────────────

    struct Symbol {
        enum class Kind { Var, Func, Type, Const, Module, Interface };

        Kind kind = Kind::Var;
        std::string name;
        SourceLocation location;
        TypeRef type;
        bool isMut = false;
        std::vector<std::string> interfaceMethods; // for Interface kind
    };

    class Scope {
    public:
        explicit Scope(Scope *parent = nullptr) : parent_(parent) {
        }

        // Returns false and emits a diagnostic if the name is already defined.
        bool Define(Symbol sym, std::vector<SemaDiagnostic> &diags,
                    const std::string &sourceName) {
            auto it = table_.find(sym.name);
            if (it != table_.end()) {
                diags.push_back({
                    SemaDiagnostic::Severity::Error,
                    sourceName,
                    sym.location,
                    std::format("'{}' is already defined (first defined at {}:{})",
                                sym.name, it->second.location.line, it->second.location.column)
                });
                return false;
            }
            table_.emplace(sym.name, std::move(sym));
            return true;
        }

        Symbol *Lookup(const std::string &name) {
            auto it = table_.find(name);
            if (it != table_.end()) return &it->second;
            if (parent_) return parent_->Lookup(name);
            return nullptr;
        }

        Scope *Parent() const { return parent_; }

    private:
        Scope *parent_;
        std::unordered_map<std::string, Symbol> table_;
    };

    // ── Internal: Analyzer ────────────────────────────────────────────────────────

    class Analyzer {
    public:
        Analyzer(std::vector<const Module *> &modules, std::vector<SemaDiagnostic> &diags,
                 std::vector<SemaSymbol> &symbols)
            : modules_(modules), diags_(diags), symbols_(symbols), currentScope_(&globalScope_) {
        }

        void Run() {
            RegisterBuiltins();
            for (auto *mod: modules_) CollectModule(*mod);
            for (auto *mod: modules_) CheckModule(*mod);
        }

    private:
        std::vector<const Module *> &modules_;
        std::vector<SemaDiagnostic> &diags_;
        std::vector<SemaSymbol> &symbols_;
        Scope globalScope_{nullptr};
        Scope *currentScope_;
        std::vector<std::unique_ptr<Scope> > ownedScopes_;

        std::string currentFile_;
        TypeRef currentReturnType_ = TypeRef::MakeVoid();
        int loopDepth_ = 0;
        bool inImpl_ = false;
        std::vector<std::string> currentTypeParams_;

        // ── Diagnostics ───────────────────────────────────────────────────────────

        void EmitError(SourceLocation loc, std::string msg) {
            diags_.push_back({SemaDiagnostic::Severity::Error, currentFile_, loc, std::move(msg)});
        }

        void EmitWarning(SourceLocation loc, std::string msg) {
            diags_.push_back({SemaDiagnostic::Severity::Warning, currentFile_, loc, std::move(msg)});
        }

        // ── Scope management ──────────────────────────────────────────────────────

        void PushScope() {
            ownedScopes_.push_back(std::make_unique<Scope>(currentScope_));
            currentScope_ = ownedScopes_.back().get();
        }

        void PopScope() {
            assert(currentScope_->Parent() != nullptr && "cannot pop global scope");
            currentScope_ = currentScope_->Parent();
        }

        bool Define(Symbol sym) {
            return currentScope_->Define(std::move(sym), diags_, currentFile_);
        }

        // ── Builtins ──────────────────────────────────────────────────────────────

        void RegisterBuiltins() {
            auto add = [&](const char *name, TypeRef t) {
                Symbol sym;
                sym.kind = Symbol::Kind::Type;
                sym.name = name;
                sym.type = std::move(t);
                globalScope_.Define(sym, diags_, "<builtin>");
            };
            add("void", TypeRef::MakeVoid());
            add("bool", TypeRef::MakeBool());
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

        // ── First pass: collect global declaration names ───────────────────────────

        void CollectModule(const Module &mod) {
            currentFile_ = mod.name;
            for (const auto &decl: mod.items)
                CollectDecl(*decl, globalScope_);
        }

        void CollectDecl(const Decl &decl, Scope &scope) {
            // Records the symbol in `scope` and, for top-level (global) scope,
            // also appends a SemaSymbol to `symbols_` for the dump.
            bool isGlobal = (&scope == &globalScope_);

            auto simple = [&](Symbol::Kind kind, const std::string &name,
                              SemaSymbol::Kind pubKind, std::string resolvedType = {},
                              bool isMut = false) {
                Symbol sym;
                sym.kind = kind;
                sym.name = name;
                sym.location = decl.location;
                sym.isMut = isMut;
                if (scope.Define(sym, diags_, currentFile_) && isGlobal) {
                    symbols_.push_back({
                        pubKind, name, currentFile_,
                        decl.location, std::move(resolvedType), isMut
                    });
                }
            };

            if (auto *d = dynamic_cast<const FuncDecl *>(&decl))
                simple(Symbol::Kind::Func, d->name, SemaSymbol::Kind::Func);
            else if (auto *d = dynamic_cast<const StructDecl *>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "struct");
            else if (auto *d = dynamic_cast<const EnumDecl *>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "enum");
            else if (auto *d = dynamic_cast<const UnionDecl *>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "union");
            else if (auto *d = dynamic_cast<const InterfaceDecl *>(&decl)) {
                Symbol sym;
                sym.kind = Symbol::Kind::Interface;
                sym.name = d->name;
                sym.location = d->location;
                for (auto &m: d->methods)
                    sym.interfaceMethods.push_back(m->name);
                if (scope.Define(sym, diags_, currentFile_) && isGlobal)
                    symbols_.push_back({
                        SemaSymbol::Kind::Interface, d->name,
                        currentFile_, d->location, "interface"
                    });
            } else if (auto *d = dynamic_cast<const ConstDecl *>(&decl))
                simple(Symbol::Kind::Const, d->name, SemaSymbol::Kind::Const);
            else if (auto *d = dynamic_cast<const TypeAliasDecl *>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "type alias");
            else if (auto *d = dynamic_cast<const ExternFuncDecl *>(&decl))
                simple(Symbol::Kind::Func, d->name, SemaSymbol::Kind::Func, "extern");
            else if (auto *d = dynamic_cast<const ExternVarDecl *>(&decl)) {
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = d->name;
                sym.location = d->location;
                sym.isMut = true;
                if (scope.Define(sym, diags_, currentFile_) && isGlobal)
                    symbols_.push_back({
                        SemaSymbol::Kind::Var, d->name,
                        currentFile_, d->location, "extern", true
                    });
            } else if (auto *d = dynamic_cast<const ModuleDecl *>(&decl)) {
                simple(Symbol::Kind::Module, d->name, SemaSymbol::Kind::Module);
                for (auto &item: d->items)
                    CollectDecl(*item, scope); // flatten into parent scope for now
            }
            // ImplDecl and UseDecl don't add names in the first pass
        }

        // ── Type resolution ───────────────────────────────────────────────────────

        TypeRef ResolveType(const TypeExpr &expr) {
            if (auto *t = dynamic_cast<const NamedTypeExpr *>(&expr)) {
                for (const auto &tp: currentTypeParams_)
                    if (tp == t->name) return TypeRef::MakeTypeParam(t->name);

                Symbol *sym = currentScope_->Lookup(t->name);
                if (sym && (sym->kind == Symbol::Kind::Type ||
                            sym->kind == Symbol::Kind::Interface)) {
                    if (!sym->type.IsUnknown()) return sym->type; // builtin
                    return TypeRef::MakeNamed(t->name); // user-defined
                }
                EmitError(expr.location, std::format("unknown type '{}'", t->name));
                return TypeRef::MakeUnknown();
            }
            if (auto *t = dynamic_cast<const PathTypeExpr *>(&expr)) {
                // Simplified: treat path as Named with last segment
                return TypeRef::MakeNamed(t->segments.back());
            }
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

        // ── Second pass: check declarations ───────────────────────────────────────

        void CheckModule(const Module &mod) {
            currentFile_ = mod.name;
            for (const auto &decl: mod.items)
                CheckDecl(*decl);
        }

        void CheckDecl(const Decl &decl) {
            if (auto *d = dynamic_cast<const FuncDecl *>(&decl))
                CheckFuncDecl(*d);
            else if (auto *d = dynamic_cast<const StructDecl *>(&decl))
                CheckStructDecl(*d);
            else if (auto *d = dynamic_cast<const EnumDecl *>(&decl))
                CheckEnumDecl(*d);
            else if (auto *d = dynamic_cast<const UnionDecl *>(&decl))
                CheckUnionDecl(*d);
            else if (auto *d = dynamic_cast<const InterfaceDecl *>(&decl))
                CheckInterfaceDecl(*d);
            else if (auto *d = dynamic_cast<const ImplDecl *>(&decl))
                CheckImplDecl(*d);
            else if (auto *d = dynamic_cast<const ModuleDecl *>(&decl))
                CheckModuleDecl(*d);
            else if (auto *d = dynamic_cast<const ConstDecl *>(&decl))
                CheckConstDecl(*d);
            else if (auto *d = dynamic_cast<const TypeAliasDecl *>(&decl))
                ResolveType(*d->type); // triggers unknown-type errors
            else if (auto *d = dynamic_cast<const ExternFuncDecl *>(&decl)) {
                if (d->returnType) ResolveType(*d->returnType->get());
                for (auto &p: d->params)
                    if (!p.isVariadic) ResolveType(*p.type);
            } else if (auto *d = dynamic_cast<const ExternVarDecl *>(&decl))
                ResolveType(*d->type);
            // UseDecl: import resolution deferred
        }

        void CheckFuncDecl(const FuncDecl &d, bool isMethod = false) {
            auto savedTypeParams = currentTypeParams_;
            currentTypeParams_ = d.typeParams;

            TypeRef retType = d.returnType
                                  ? ResolveType(*d.returnType->get())
                                  : TypeRef::MakeVoid();

            auto savedRet = currentReturnType_;
            currentReturnType_ = retType;

            PushScope();

            for (const auto &tp: d.typeParams) {
                Symbol sym;
                sym.kind = Symbol::Kind::Type;
                sym.name = tp;
                sym.type = TypeRef::MakeTypeParam(tp);
                Define(sym);
            }

            if (isMethod) {
                Symbol self;
                self.kind = Symbol::Kind::Var;
                self.name = "self";
                self.type = TypeRef::MakeNamed("self");
                self.isMut = true;
                Define(self);
            }

            for (const auto &param: d.params) {
                if (param.isVariadic) continue;
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = param.name;
                sym.location = param.location;
                sym.type = ResolveType(*param.type);
                sym.isMut = false;
                Define(sym);
            }

            if (d.body)
                CheckBlock(*d.body);

            PopScope();
            currentReturnType_ = savedRet;
            currentTypeParams_ = savedTypeParams;
        }

        void CheckStructDecl(const StructDecl &d) {
            std::unordered_set<std::string> seen;
            for (const auto &field: d.fields) {
                if (!seen.insert(field.name).second)
                    EmitError(field.location,
                              std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
                ResolveType(*field.type);
            }
        }

        void CheckEnumDecl(const EnumDecl &d) {
            std::unordered_set<std::string> seen;
            for (const auto &variant: d.variants) {
                if (!seen.insert(variant.name).second)
                    EmitError(variant.location,
                              std::format("duplicate variant '{}' in enum '{}'", variant.name, d.name));
                for (const auto &f: variant.fields)
                    ResolveType(*f);
            }
        }

        void CheckUnionDecl(const UnionDecl &d) {
            std::unordered_set<std::string> seen;
            for (const auto &field: d.fields) {
                if (!seen.insert(field.name).second)
                    EmitError(field.location,
                              std::format("duplicate field '{}' in union '{}'", field.name, d.name));
                ResolveType(*field.type);
            }
        }

        void CheckInterfaceDecl(const InterfaceDecl &d) {
            std::unordered_set<std::string> seen;
            for (const auto &method: d.methods) {
                if (!seen.insert(method->name).second)
                    EmitError(method->location,
                              std::format("duplicate method '{}' in interface '{}'",
                                          method->name, d.name));
                if (method->returnType)
                    ResolveType(*method->returnType->get());
                for (const auto &p: method->params)
                    if (!p.isVariadic) ResolveType(*p.type);
            }
        }

        void CheckImplDecl(const ImplDecl &d) {
            if (!currentScope_->Lookup(d.typeName))
                EmitError(d.location,
                          std::format("impl for unknown type '{}'", d.typeName));

            if (d.interfaceName) {
                Symbol *ifaceSym = currentScope_->Lookup(*d.interfaceName);
                if (!ifaceSym || ifaceSym->kind != Symbol::Kind::Interface) {
                    EmitError(d.location,
                              std::format("'{}' is not a known interface", *d.interfaceName));
                } else {
                    std::unordered_set<std::string> implNames;
                    for (const auto &m: d.methods)
                        implNames.insert(m->name);
                    for (const auto &required: ifaceSym->interfaceMethods) {
                        if (!implNames.count(required))
                            EmitError(d.location,
                                      std::format("impl of '{}' for '{}' is missing method '{}'",
                                                  *d.interfaceName, d.typeName, required));
                    }
                }
            }

            bool savedInImpl = inImpl_;
            inImpl_ = true;
            for (const auto &m: d.methods)
                CheckFuncDecl(*m, /*isMethod=*/true);
            inImpl_ = savedInImpl;
        }

        void CheckModuleDecl(const ModuleDecl &d) {
            PushScope();
            for (const auto &item: d.items)
                CheckDecl(*item);
            PopScope();
        }

        void CheckConstDecl(const ConstDecl &d) {
            ResolveType(*d.type);
            CheckExpr(*d.value);
        }

        // ── Block & statements ────────────────────────────────────────────────────

        void CheckBlock(const Block &block) {
            PushScope();
            for (const auto &stmt: block.stmts)
                CheckStmt(*stmt);
            PopScope();
        }

        void CheckStmt(const Stmt &stmt) {
            if (auto *s = dynamic_cast<const ExprStmt *>(&stmt)) {
                CheckExpr(*s->expr);
            } else if (auto *s = dynamic_cast<const LetStmt *>(&stmt)) {
                TypeRef initType = CheckExpr(*s->init);
                TypeRef declType = s->type
                                       ? ResolveType(*s->type->get())
                                       : initType;

                if (!s->type && declType.IsUnknown())
                    EmitWarning(s->location,
                                std::format("cannot infer type of '{}'", s->name));

                if (s->type && !initType.IsUnknown() && !declType.IsUnknown() &&
                    !initType.IsAssignableTo(declType))
                    EmitError(s->location,
                              std::format("cannot assign '{}' to '{}'",
                                          initType.ToString(), declType.ToString()));

                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = s->name;
                sym.location = s->location;
                sym.type = declType;
                sym.isMut = s->isMut;
                Define(sym);
            } else if (auto *s = dynamic_cast<const IfStmt *>(&stmt)) {
                CheckExpr(*s->condition);
                CheckBlock(*s->thenBlock);
                for (const auto &elif: s->elseIfs) {
                    CheckExpr(*elif.condition);
                    CheckBlock(*elif.block);
                }
                if (s->elseBlock)
                    CheckBlock(*s->elseBlock);
            } else if (auto *s = dynamic_cast<const WhileStmt *>(&stmt)) {
                CheckExpr(*s->condition);
                ++loopDepth_;
                CheckBlock(*s->body);
                --loopDepth_;
            } else if (auto *s = dynamic_cast<const ForStmt *>(&stmt)) {
                CheckExpr(*s->iterable);
                PushScope(); // scope for the loop variable
                Symbol var;
                var.kind = Symbol::Kind::Var;
                var.name = s->variable;
                var.location = s->location;
                var.type = TypeRef::MakeUnknown(); // element type needs full type inference
                var.isMut = false;
                Define(var);
                ++loopDepth_;
                CheckBlock(*s->body); // CheckBlock pushes its own nested scope
                --loopDepth_;
                PopScope();
            } else if (auto *s = dynamic_cast<const MatchStmt *>(&stmt)) {
                CheckExpr(*s->subject);
                for (const auto &arm: s->arms) {
                    PushScope(); // each arm has its own binding scope
                    CheckPattern(*arm.pattern);
                    CheckExpr(*arm.body);
                    PopScope();
                }
            } else if (auto *s = dynamic_cast<const ReturnStmt *>(&stmt)) {
                if (s->value) {
                    TypeRef valType = CheckExpr(**s->value);
                    if (!valType.IsUnknown() && !currentReturnType_.IsUnknown() &&
                        !currentReturnType_.IsVoid() &&
                        !valType.IsAssignableTo(currentReturnType_))
                        EmitError(s->location,
                                  std::format("return type mismatch: expected '{}', found '{}'",
                                              currentReturnType_.ToString(), valType.ToString()));
                } else if (!currentReturnType_.IsVoid() && !currentReturnType_.IsUnknown()) {
                    EmitError(s->location,
                              std::format("missing return value; expected '{}'",
                                          currentReturnType_.ToString()));
                }
            } else if (dynamic_cast<const BreakStmt *>(&stmt)) {
                if (loopDepth_ == 0)
                    EmitError(stmt.location, "'break' outside of a loop");
            } else if (dynamic_cast<const ContinueStmt *>(&stmt)) {
                if (loopDepth_ == 0)
                    EmitError(stmt.location, "'continue' outside of a loop");
            } else if (auto *s = dynamic_cast<const DeclStmt *>(&stmt)) {
                CollectDecl(*s->decl, *currentScope_);
                CheckDecl(*s->decl);
            }
        }

        void CheckPattern(const Pattern &pat) {
            if (auto *p = dynamic_cast<const IdentPattern *>(&pat)) {
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = p->name;
                sym.location = p->location;
                sym.type = TypeRef::MakeUnknown();
                sym.isMut = false;
                Define(sym);
            } else if (auto *p = dynamic_cast<const GuardedPattern *>(&pat)) {
                CheckPattern(*p->inner);
                CheckExpr(*p->guard);
            } else if (auto *p = dynamic_cast<const RangePattern *>(&pat)) {
                CheckPattern(*p->lo);
                CheckPattern(*p->hi);
            } else if (auto *p = dynamic_cast<const TuplePattern *>(&pat)) {
                for (const auto &e: p->elements)
                    CheckPattern(*e);
            } else if (auto *p = dynamic_cast<const StructPattern *>(&pat)) {
                if (!currentScope_->Lookup(p->typeName))
                    EmitError(p->location,
                              std::format("unknown type '{}' in struct pattern", p->typeName));
                for (const auto &f: p->fields)
                    CheckPattern(*f.pattern);
            } else if (auto *p = dynamic_cast<const EnumPattern *>(&pat)) {
                if (!p->path.empty() && !currentScope_->Lookup(p->path[0]))
                    EmitError(p->location,
                              std::format("unknown name '{}' in enum pattern", p->path[0]));
                for (const auto &a: p->args)
                    CheckPattern(*a);
            }
            // WildcardPattern, LiteralPattern: nothing to resolve
        }

        // ── Expressions ───────────────────────────────────────────────────────────

        TypeRef CheckExpr(const Expr &expr) {
            if (auto *e = dynamic_cast<const LiteralExpr *>(&expr))
                return LiteralType(e->token);

            if (auto *e = dynamic_cast<const IdentExpr *>(&expr)) {
                Symbol *sym = currentScope_->Lookup(e->name);
                if (sym) return sym->type;
                EmitError(e->location, std::format("undefined name '{}'", e->name));
                return TypeRef::MakeUnknown();
            }

            if (dynamic_cast<const SelfExpr *>(&expr)) {
                if (!inImpl_)
                    EmitError(expr.location, "'self' used outside of an impl block");
                return TypeRef::MakeNamed("self");
            }

            if (auto *e = dynamic_cast<const PathExpr *>(&expr)) {
                if (!e->segments.empty() && !currentScope_->Lookup(e->segments[0]))
                    EmitError(e->location,
                              std::format("undefined name '{}'", e->segments[0]));
                return TypeRef::MakeUnknown();
            }

            if (auto *e = dynamic_cast<const UnaryExpr *>(&expr)) {
                TypeRef t = CheckExpr(*e->operand);
                return CheckUnary(e->op, t, e->location);
            }

            if (auto *e = dynamic_cast<const BinaryExpr *>(&expr)) {
                TypeRef l = CheckExpr(*e->left);
                TypeRef r = CheckExpr(*e->right);
                return CheckBinary(e->op, l, r, e->location);
            }

            if (auto *e = dynamic_cast<const AssignExpr *>(&expr)) {
                CheckMutability(*e->target);
                TypeRef tgt = CheckExpr(*e->target);
                TypeRef val = CheckExpr(*e->value);
                if (!tgt.IsUnknown() && !val.IsUnknown() && !val.IsAssignableTo(tgt))
                    EmitError(e->location,
                              std::format("cannot assign '{}' to '{}'",
                                          val.ToString(), tgt.ToString()));
                return TypeRef::MakeVoid();
            }

            if (auto *e = dynamic_cast<const TernaryExpr *>(&expr)) {
                TypeRef cond = CheckExpr(*e->condition);
                if (!cond.IsUnknown() && !cond.IsBool())
                    EmitError(e->condition->location, "ternary condition must be 'bool'");
                TypeRef thenT = CheckExpr(*e->thenExpr);
                TypeRef elseT = CheckExpr(*e->elseExpr);
                return thenT.IsUnknown() ? elseT : thenT;
            }

            if (auto *e = dynamic_cast<const RangeExpr *>(&expr)) {
                if (e->lo) CheckExpr(*e->lo);
                if (e->hi) CheckExpr(*e->hi);
                return TypeRef::MakeNamed("Range");
            }

            if (auto *e = dynamic_cast<const CallExpr *>(&expr)) {
                TypeRef calleeType = CheckExpr(*e->callee);
                for (const auto &arg: e->args) CheckExpr(*arg);
                if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty())
                    return calleeType.inner.back();
                return TypeRef::MakeUnknown();
            }

            if (auto *e = dynamic_cast<const IndexExpr *>(&expr)) {
                TypeRef obj = CheckExpr(*e->object);
                CheckExpr(*e->index);
                if (obj.kind == TypeRef::Kind::Array && !obj.inner.empty())
                    return obj.inner[0];
                return TypeRef::MakeUnknown();
            }

            if (auto *e = dynamic_cast<const FieldExpr *>(&expr)) {
                CheckExpr(*e->object);
                return TypeRef::MakeUnknown(); // field type lookup needs full type info
            }

            if (auto *e = dynamic_cast<const StructInitExpr *>(&expr)) {
                if (!currentScope_->Lookup(e->typeName))
                    EmitError(e->location,
                              std::format("unknown type '{}' in struct initializer", e->typeName));
                for (const auto &f: e->fields) CheckExpr(*f.value);
                return TypeRef::MakeNamed(e->typeName);
            }

            if (auto *e = dynamic_cast<const ArrayExpr *>(&expr)) {
                TypeRef elemType = TypeRef::MakeUnknown();
                for (const auto &el: e->elements) {
                    TypeRef t = CheckExpr(*el);
                    if (elemType.IsUnknown()) elemType = t;
                }
                return TypeRef::MakeArray(elemType);
            }

            if (auto *e = dynamic_cast<const CastExpr *>(&expr)) {
                CheckExpr(*e->operand);
                return ResolveType(*e->type);
            }

            if (auto *e = dynamic_cast<const IsExpr *>(&expr)) {
                CheckExpr(*e->operand);
                ResolveType(*e->type);
                return TypeRef::MakeBool();
            }

            if (auto *e = dynamic_cast<const BlockExpr *>(&expr)) {
                CheckBlock(*e->block);
                return TypeRef::MakeUnknown();
            }

            return TypeRef::MakeUnknown();
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

        TypeRef CheckUnary(TokenKind op, const TypeRef &t, SourceLocation loc) {
            if (t.IsUnknown()) return TypeRef::MakeUnknown();
            switch (op) {
                case TokenKind::Bang:
                    if (!t.IsBool())
                        EmitError(loc, std::format("'!' applied to non-bool type '{}'", t.ToString()));
                    return TypeRef::MakeBool();
                case TokenKind::Minus:
                    if (!t.IsNumeric())
                        EmitError(loc, std::format("unary '-' applied to non-numeric type '{}'", t.ToString()));
                    return t;
                case TokenKind::Tilde:
                    if (!t.IsInteger())
                        EmitError(loc, std::format("'~' applied to non-integer type '{}'", t.ToString()));
                    return t;
                case TokenKind::Star:
                    if (t.kind != TypeRef::Kind::Pointer)
                        EmitError(loc, std::format("'*' (dereference) applied to non-pointer type '{}'", t.ToString()));
                    return t.inner.empty() ? TypeRef::MakeUnknown() : t.inner[0];
                case TokenKind::Amp:
                    return TypeRef::MakePointer(t);
                default:
                    return TypeRef::MakeUnknown();
            }
        }

        TypeRef CheckBinary(TokenKind op, const TypeRef &l, const TypeRef &r, SourceLocation loc) {
            if (l.IsUnknown() || r.IsUnknown()) return TypeRef::MakeUnknown();

            using TK = TokenKind;
            switch (op) {
                case TK::Plus:
                case TK::Minus:
                case TK::Star:
                case TK::Slash:
                case TK::Percent:
                case TK::StarStar:
                    if (!l.IsNumeric())
                        EmitError(loc, std::format("'{}' applied to non-numeric type '{}'",
                                                   op == TK::Plus
                                                       ? "+"
                                                       : op == TK::Minus
                                                             ? "-"
                                                             : op == TK::Star
                                                                   ? "*"
                                                                   : op == TK::Slash
                                                                         ? "/"
                                                                         : op == TK::Percent
                                                                               ? "%"
                                                                               : "**", l.ToString()));
                    return l;

                case TK::Amp:
                case TK::Pipe:
                case TK::Caret:
                case TK::LessLess:
                case TK::GreaterGreater:
                    if (!l.IsInteger())
                        EmitError(loc, std::format("bitwise operator applied to non-integer type '{}'",
                                                   l.ToString()));
                    return l;

                case TK::AmpAmp:
                case TK::PipePipe:
                    if (!l.IsBool())
                        EmitError(loc, std::format("'{}' applied to non-bool type '{}'",
                                                   op == TK::AmpAmp ? "&&" : "||", l.ToString()));
                    if (!r.IsBool())
                        EmitError(loc, std::format("'{}' applied to non-bool type '{}'",
                                                   op == TK::AmpAmp ? "&&" : "||", r.ToString()));
                    return TypeRef::MakeBool();

                case TK::Equal:
                case TK::BangEqual:
                case TK::Less:
                case TK::LessEqual:
                case TK::Greater:
                case TK::GreaterEqual:
                    return TypeRef::MakeBool();

                default:
                    return TypeRef::MakeUnknown();
            }
        }

        // Check that an assignment target is mutable.
        void CheckMutability(const Expr &target) {
            if (auto *e = dynamic_cast<const IdentExpr *>(&target)) {
                Symbol *sym = currentScope_->Lookup(e->name);
                if (sym && sym->kind == Symbol::Kind::Var && !sym->isMut)
                    EmitError(target.location,
                              std::format("cannot assign to immutable variable '{}'", e->name));
            }
            // Field and index targets: would need full type info to check properly
        }
    };

    // ── Sema public API ───────────────────────────────────────────────────────────

    Sema::Sema(std::vector<const Module *> modules)
        : modules_(std::move(modules)) {
    }

    SemaResult Sema::Analyze() {
        Analyzer analyzer(modules_, diags_, symbols_);
        analyzer.Run();
        return SemaResult{std::move(diags_), std::move(symbols_)};
    }

    bool Sema::DumpResult(const SemaResult &result, const std::filesystem::path &path) {
        std::ofstream out(path);
        if (!out) return false;

        static constexpr auto kindName = [](SemaSymbol::Kind k) -> std::string_view {
            switch (k) {
                case SemaSymbol::Kind::Var: return "var";
                case SemaSymbol::Kind::Func: return "func";
                case SemaSymbol::Kind::Type: return "type";
                case SemaSymbol::Kind::Const: return "const";
                case SemaSymbol::Kind::Module: return "module";
                case SemaSymbol::Kind::Interface: return "interface";
            }
            return "?";
        };

        out << "=== Semantic Analysis Results ===\n\n";

        // ── Symbols ───────────────────────────────────────────────────────────────
        out << std::format("Symbols ({} total)\n", result.symbols.size());
        out << std::string(40, '-') << '\n';

        if (result.symbols.empty()) {
            out << "(none)\n";
        } else {
            for (const auto &sym: result.symbols) {
                std::string tag = std::format("{:<10}", kindName(sym.kind));
                std::string qname = sym.name;
                if (sym.isMut) qname += " (mut)";
                std::string typeStr = sym.resolvedType.empty() ? "" : "  " + sym.resolvedType;
                out << std::format("{}  {:<28}{}  [{}:{}:{}]\n",
                                   tag, qname, typeStr,
                                   sym.sourceName, sym.location.line, sym.location.column);
            }
        }

        out << '\n';

        // ── Diagnostics ───────────────────────────────────────────────────────────
        out << std::format("Diagnostics ({} total)\n", result.diagnostics.size());
        out << std::string(40, '-') << '\n';

        if (result.diagnostics.empty()) {
            out << "(none)\n";
        } else {
            for (const auto &diag: result.diagnostics) {
                const char *sev = diag.severity == SemaDiagnostic::Severity::Error
                                      ? "error"
                                      : "warning";
                out << std::format("{}:{}:{}: {}: {}\n",
                                   diag.sourceName, diag.location.line, diag.location.column,
                                   sev, diag.message);
            }
        }

        return out.good();
    }
}
