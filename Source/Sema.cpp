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
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Rux
{
    // TypeRef implementation
    bool TypeRef::IsNumeric() const noexcept
    {
        switch (kind)
        {
        case Kind::Int8:
        case Kind::Int16:
        case Kind::Int32:
        case Kind::Int64:
        case Kind::Int:
        case Kind::UInt8:
        case Kind::UInt16:
        case Kind::UInt32:
        case Kind::UInt64:
        case Kind::UInt:
        case Kind::Float32:
        case Kind::Float64:
            return true;
        default: return false;
        }
    }

    bool TypeRef::IsInteger() const noexcept
    {
        switch (kind)
        {
        case Kind::Int8:
        case Kind::Int16:
        case Kind::Int32:
        case Kind::Int64:
        case Kind::Int:
        case Kind::UInt8:
        case Kind::UInt16:
        case Kind::UInt32:
        case Kind::UInt64:
        case Kind::UInt:
            return true;
        default: return false;
        }
    }

    bool TypeRef::IsFloat() const noexcept
    {
        return kind == Kind::Float32 || kind == Kind::Float64;
    }

    bool TypeRef::IsSigned() const noexcept
    {
        switch (kind)
        {
        case Kind::Int8:
        case Kind::Int16:
        case Kind::Int32:
        case Kind::Int64:
        case Kind::Int:
            return true;
        default: return false;
        }
    }

    bool TypeRef::IsAssignableTo(const TypeRef& other) const noexcept
    {
        if (IsUnknown() || other.IsUnknown()) return true;
        if (*this == other) return true;
        // float32 widens implicitly to float64 / float (safe, no precision loss in range)
        if (kind == Kind::Float32 && other.kind == Kind::Float64) return true;
        // int/uint interoperate with their fixed-width platform equivalents (x64: 64-bit)
        if (kind == Kind::Int64 && other.kind == Kind::Int) return true;
        if (kind == Kind::Int && other.kind == Kind::Int64) return true;
        if (kind == Kind::UInt64 && other.kind == Kind::UInt) return true;
        if (kind == Kind::UInt && other.kind == Kind::UInt64) return true;
        // smaller fixed-width integers widen implicitly to int/uint
        if (other.kind == Kind::Int &&
            (kind == Kind::Int8 || kind == Kind::Int16 || kind == Kind::Int32))
            return true;
        if (other.kind == Kind::UInt &&
            (kind == Kind::UInt8 || kind == Kind::UInt16 || kind == Kind::UInt32))
            return true;
        // Numeric types must match exactly unless an explicit cast is used.
        if (IsNumeric() && other.IsNumeric()) return false;
        // Bool types are mutually assignable across widths
        if (IsBool() && other.IsBool()) return true;
        // Any pointer is implicitly assignable to *opaque (like void* in C)
        if (kind == Kind::Pointer && other.kind == Kind::Pointer &&
            !other.inner.empty() && other.inner[0].IsOpaque())
            return true;
        return false;
    }

    std::optional<std::uint64_t> TypeRef::SizeInBytes() const noexcept
    {
        auto alignUp = [](const std::uint64_t value, const std::uint64_t align)
        {
            return (value + align - 1) & ~(align - 1);
        };

        switch (kind)
        {
        case Kind::Unknown:
        case Kind::TypeParam:
            return std::nullopt;
        case Kind::Opaque:
            return 0;
        case Kind::Bool8:
        case Kind::Char8:
        case Kind::Int8:
        case Kind::UInt8:
            return 1;
        case Kind::Bool16:
        case Kind::Char16:
        case Kind::Int16:
        case Kind::UInt16:
            return 2;
        case Kind::Bool32:
        case Kind::Char32:
        case Kind::Int32:
        case Kind::UInt32:
        case Kind::Float32:
            return 4;
        case Kind::Int64:
        case Kind::UInt64:
        case Kind::Int:
        case Kind::UInt:
        case Kind::Float64:
        case Kind::Pointer:
        case Kind::Str:
        case Kind::Func:
            return 8;
        case Kind::Slice:
            return 16;
        case Kind::Range:
            {
                if (inner.empty()) return std::nullopt;
                const auto elemSize = inner[0].SizeInBytes();
                if (!elemSize || *elemSize == 0) return std::nullopt;
                return alignUp(2 * *elemSize + 1, *elemSize);
            }
        case Kind::Tuple:
            {
                std::uint64_t total = 0;
                for (const auto& elem : inner)
                {
                    const auto elemSize = elem.SizeInBytes();
                    if (!elemSize) return std::nullopt;
                    total += *elemSize;
                }
                return total;
            }
        case Kind::Named:
            if (name.starts_with("Slice<")) return 16;
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::string TypeRef::ToString() const
    {
        switch (kind)
        {
        case Kind::Unknown: return "?";
        case Kind::Opaque: return "opaque";
        case Kind::Bool8: return "bool8";
        case Kind::Bool16: return "bool16";
        case Kind::Bool32: return "bool32";
        case Kind::Char8: return "char8";
        case Kind::Char16: return "char16";
        case Kind::Char32: return "char32";
        case Kind::Str: return "String";
        case Kind::Int8: return "int8";
        case Kind::Int16: return "int16";
        case Kind::Int32: return "int32";
        case Kind::Int64: return "int64";
        case Kind::Int: return "int";
        case Kind::UInt8: return "uint8";
        case Kind::UInt16: return "uint16";
        case Kind::UInt32: return "uint32";
        case Kind::UInt64: return "uint64";
        case Kind::UInt: return "uint";
        case Kind::Float32: return "float32";
        case Kind::Float64: return "float64";
        case Kind::Named: return name;
        case Kind::TypeParam: return name;
        case Kind::Pointer: return "*" + (inner.empty() ? "?" : inner[0].ToString());
        case Kind::Slice: return (inner.empty() ? "?" : inner[0].ToString()) + "[]";
        case Kind::Range: return "Range<" + (inner.empty() ? "?" : inner[0].ToString()) + ">";
        case Kind::Tuple:
            {
                std::string s = "(";
                for (std::size_t i = 0; i < inner.size(); ++i)
                {
                    if (i) s += ", ";
                    s += inner[i].ToString();
                }
                return s + ")";
            }
        case Kind::Func:
            {
                std::string s = "func(";
                for (std::size_t i = 0; i + 1 < inner.size(); ++i)
                {
                    if (i) s += ", ";
                    s += inner[i].ToString();
                }
                s += ") -> ";
                s += inner.empty() ? "opaque" : inner.back().ToString();
                return s;
            }
        }
        return "?";
    }

    bool TypeRef::operator==(const TypeRef& o) const noexcept
    {
        if (kind != o.kind || name != o.name || inner.size() != o.inner.size()) return false;
        for (std::size_t i = 0; i < inner.size(); ++i)
            if (inner[i] != o.inner[i]) return false;
        return true;
    }

    // SemaResult

    bool SemaResult::HasErrors() const noexcept
    {
        return std::ranges::any_of(diagnostics,
                                   [](const SemaDiagnostic& d)
                                   {
                                       return d.severity == SemaDiagnostic::Severity::Error;
                                   });
    }

    // Internal: Symbol & Scope
    struct Symbol
    {
        enum class Kind { Var, Func, Type, Const, Module, Interface };

        Kind kind = Kind::Var;
        std::string name;
        SourceLocation location;
        TypeRef type;
        bool isMut = false;
        std::vector<std::string> interfaceMethods; // for Interface kind
    };

    class Scope
    {
    public:
        explicit Scope(Scope* parent = nullptr) : parent(parent)
        {
        }

        // Returns false and emits a diagnostic if the name is already defined.
        bool Define(Symbol sym, std::vector<SemaDiagnostic>& diags,
                    const std::string& sourceName)
        {
            auto it = table.find(sym.name);
            if (it != table.end())
            {
                diags.push_back({
                    SemaDiagnostic::Severity::Error,
                    sourceName,
                    sym.location,
                    std::format("'{}' is already defined (first defined at {}:{})",
                                sym.name, it->second.location.line, it->second.location.column)
                });
                return false;
            }
            table.emplace(sym.name, std::move(sym));
            return true;
        }

        Symbol* Lookup(const std::string& name)
        {
            auto it = table.find(name);
            if (it != table.end()) return &it->second;
            if (parent) return parent->Lookup(name);
            return nullptr;
        }

        [[nodiscard]] Scope* Parent() const { return parent; }

    private:
        Scope* parent;
        std::unordered_map<std::string, Symbol> table;
    };

    // Internal: Analyzer
    class Analyzer
    {
    public:
        Analyzer(std::vector<const Module*>& modules, std::vector<SemaDiagnostic>& diags,
                 std::vector<SemaSymbol>& symbols)
            : modules(modules), diags(diags), symbols(symbols), currentScope(&globalScope)
        {
        }

        void Run()
        {
            RegisterBuiltins();
            for (auto* mod : modules) CollectModule(*mod);
            for (auto* mod : modules) ResolveModuleSignatures(*mod);
            for (auto* mod : modules) CheckModule(*mod);
        }

    private:
        std::vector<const Module*>& modules;
        std::vector<SemaDiagnostic>& diags;
        std::vector<SemaSymbol>& symbols;
        Scope globalScope{nullptr};
        Scope* currentScope;
        std::vector<std::unique_ptr<Scope>> ownedScopes;
        std::string currentFile;
        TypeRef currentReturnType = TypeRef::MakeOpaque();
        int loopDepth = 0;
        std::unordered_set<std::string> activeLabels;
        bool inImpl = false;
        std::vector<std::string> currentTypeParams;
        std::unordered_map<std::string, const StructDecl*> structDecls;

        // Diagnostics

        void EmitError(SourceLocation loc, std::string msg)
        {
            diags.push_back({SemaDiagnostic::Severity::Error, currentFile, loc, std::move(msg)});
        }

        void EmitWarning(SourceLocation loc, std::string msg)
        {
            diags.push_back({SemaDiagnostic::Severity::Warning, currentFile, loc, std::move(msg)});
        }

        // Scope management
        void PushScope()
        {
            ownedScopes.push_back(std::make_unique<Scope>(currentScope));
            currentScope = ownedScopes.back().get();
        }

        void PopScope()
        {
            assert(currentScope->Parent() != nullptr && "cannot pop global scope");
            currentScope = currentScope->Parent();
        }

        bool Define(Symbol sym) const
        {
            return currentScope->Define(std::move(sym), diags, currentFile);
        }

        TypeRef MakeFuncType(const std::vector<Param>& params,
                             const std::optional<TypeExprPtr>& returnType,
                             const std::vector<std::string>& typeParams = {})
        {
            auto savedTypeParams = currentTypeParams;
            currentTypeParams = typeParams;

            std::vector<TypeRef> paramTypes;
            for (const auto& param : params)
            {
                if (!param.isVariadic)
                    paramTypes.push_back(ResolveType(*param.type));
            }
            TypeRef ret = returnType ? ResolveType(*returnType->get()) : TypeRef::MakeOpaque();

            currentTypeParams = savedTypeParams;
            return TypeRef::MakeFunc(std::move(paramTypes), std::move(ret));
        }

        // ── Builtins ──────────────────────────────────────────────────────────────

        void RegisterBuiltins()
        {
            auto add = [&](const char* name, TypeRef t)
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Type;
                sym.name = name;
                sym.type = std::move(t);
                globalScope.Define(sym, diags, "<builtin>");
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
            add("int", TypeRef::MakeInt());
            add("uint8", TypeRef::MakeUInt8());
            add("uint16", TypeRef::MakeUInt16());
            add("uint32", TypeRef::MakeUInt32());
            add("uint64", TypeRef::MakeUInt64());
            add("uint", TypeRef::MakeUInt());
            add("float32", TypeRef::MakeFloat32());
            add("float64", TypeRef::MakeFloat64());
            add("float", TypeRef::MakeFloat());
        }

        // ── First pass: collect global declaration names ───────────────────────────

        void CollectModule(const Module& mod)
        {
            currentFile = mod.name;
            for (const auto& decl : mod.items)
                CollectDecl(*decl, globalScope);
        }

        void ResolveModuleSignatures(const Module& mod)
        {
            currentFile = mod.name;
            for (const auto& decl : mod.items)
                ResolveDeclSignature(*decl);
        }

        void ResolveDeclSignature(const Decl& decl)
        {
            if (auto* d = dynamic_cast<const FuncDecl*>(&decl))
            {
                if (Symbol* sym = globalScope.Lookup(d->name))
                    sym->type = MakeFuncType(d->params, d->returnType, d->typeParams);
            }
            else if (auto* d = dynamic_cast<const ExternFuncDecl*>(&decl))
            {
                if (Symbol* sym = globalScope.Lookup(d->name))
                    sym->type = MakeFuncType(d->params, d->returnType);
            }
            else if (auto* d = dynamic_cast<const ExternBlockDecl*>(&decl))
            {
                for (const auto& item : d->items)
                    ResolveDeclSignature(*item);
            }
            else if (auto* d = dynamic_cast<const ModuleDecl*>(&decl))
            {
                for (const auto& item : d->items)
                    ResolveDeclSignature(*item);
            }
        }

        void CollectDecl(const Decl& decl, Scope& scope)
        {
            // Records the symbol in `scope` and, for top-level (global) scope,
            // also appends a SemaSymbol to `symbols_` for the dump.
            bool isGlobal = (&scope == &globalScope);

            auto simple = [&](Symbol::Kind kind, const std::string& name,
                              SemaSymbol::Kind pubKind, std::string resolvedType = {},
                              bool isMut = false)
            {
                Symbol sym;
                sym.kind = kind;
                sym.name = name;
                sym.location = decl.location;
                sym.isMut = isMut;
                if (scope.Define(sym, diags, currentFile) && isGlobal)
                {
                    symbols.push_back({
                        pubKind, name, currentFile,
                        decl.location, std::move(resolvedType), isMut
                    });
                }
            };

            if (auto* d = dynamic_cast<const FuncDecl*>(&decl))
                simple(Symbol::Kind::Func, d->name, SemaSymbol::Kind::Func);
            else if (auto* d = dynamic_cast<const StructDecl*>(&decl))
            {
                structDecls[d->name] = d;
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "struct");
            }
            else if (auto* d = dynamic_cast<const EnumDecl*>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "enum");
            else if (auto* d = dynamic_cast<const UnionDecl*>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "union");
            else if (auto* d = dynamic_cast<const InterfaceDecl*>(&decl))
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Interface;
                sym.name = d->name;
                sym.location = d->location;
                for (auto& m : d->methods)
                    sym.interfaceMethods.push_back(m->name);
                if (scope.Define(sym, diags, currentFile) && isGlobal)
                    symbols.push_back({
                        SemaSymbol::Kind::Interface, d->name,
                        currentFile, d->location, "interface"
                    });
            }
            else if (auto* d = dynamic_cast<const ConstDecl*>(&decl))
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Const;
                sym.name = d->name;
                sym.location = d->location;
                if (d->type)
                    sym.type = ResolveType(*d->type->get());
                if (scope.Define(sym, diags, currentFile) && isGlobal)
                {
                    symbols.push_back({
                        SemaSymbol::Kind::Const, d->name, currentFile,
                        d->location, sym.type.IsUnknown() ? "" : sym.type.ToString(), false
                    });
                }
            }
            else if (auto* d = dynamic_cast<const TypeAliasDecl*>(&decl))
                simple(Symbol::Kind::Type, d->name, SemaSymbol::Kind::Type, "type alias");
            else if (auto* d = dynamic_cast<const ExternFuncDecl*>(&decl))
                simple(Symbol::Kind::Func, d->name, SemaSymbol::Kind::Func, "extern");
            else if (auto* d = dynamic_cast<const ExternVarDecl*>(&decl))
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = d->name;
                sym.location = d->location;
                sym.isMut = true;
                if (scope.Define(sym, diags, currentFile) && isGlobal)
                    symbols.push_back({
                        SemaSymbol::Kind::Var, d->name,
                        currentFile, d->location, "extern", true
                    });
            }
            else if (auto* d = dynamic_cast<const ExternBlockDecl*>(&decl))
            {
                for (auto& item : d->items)
                    CollectDecl(*item, scope);
            }
            else if (auto* d = dynamic_cast<const ModuleDecl*>(&decl))
            {
                simple(Symbol::Kind::Module, d->name, SemaSymbol::Kind::Module);
                for (auto& item : d->items)
                    CollectDecl(*item, scope); // flatten into parent scope for now
            }
            // ImplDecl and UseDecl don't add names in the first pass
        }

        // ── Type resolution ───────────────────────────────────────────────────────

        std::string GenericTypeName(const NamedTypeExpr& type)
        {
            std::string name = type.name;
            if (!type.typeArgs.empty())
            {
                name += "<";
                for (std::size_t i = 0; i < type.typeArgs.size(); ++i)
                {
                    if (i) name += ", ";
                    name += ResolveType(*type.typeArgs[i]).ToString();
                }
                name += ">";
            }
            return name;
        }

        std::string GenericStructInitName(const StructInitExpr& expr)
        {
            std::string name = expr.typeName;
            if (!expr.typeArgs.empty())
            {
                name += "<";
                for (std::size_t i = 0; i < expr.typeArgs.size(); ++i)
                {
                    if (i) name += ", ";
                    name += ResolveType(*expr.typeArgs[i]).ToString();
                }
                name += ">";
            }
            return name;
        }

        static std::string SliceTypeName(const TypeRef& elemType)
        {
            return "Slice<" + elemType.ToString() + ">";
        }

        static std::string BaseTypeName(const std::string& name)
        {
            const std::size_t pos = name.find('<');
            return pos == std::string::npos ? name : name.substr(0, pos);
        }

        static std::uint64_t AlignUp(const std::uint64_t value, const std::uint64_t align)
        {
            return (value + align - 1) & ~(align - 1);
        }

        static TypeRef StringLiteralElementType(const Token& tok)
        {
            if (tok.text.starts_with("c16\"")) return TypeRef::MakeChar16();
            if (tok.text.starts_with("c32\"")) return TypeRef::MakeChar32();
            return TypeRef::MakeChar8();
        }

        static TypeRef StringLiteralType(const Token& tok)
        {
            return TypeRef::MakeNamed(SliceTypeName(StringLiteralElementType(tok)));
        }

        static TypeRef CharLiteralType(const Token& tok)
        {
            if (tok.text.starts_with("c8'")) return TypeRef::MakeChar8();
            if (tok.text.starts_with("c16'")) return TypeRef::MakeChar16();
            if (tok.text.starts_with("c32'")) return TypeRef::MakeChar32();
            return TypeRef::MakeChar();
        }

        static std::string NumericLiteralSuffix(std::string_view text)
        {
            static constexpr std::string_view suffixes[] = {
                "i8", "i16", "i32", "i64",
                "u8", "u16", "u32", "u64",
                "f32", "f64"
            };
            for (auto suffix : suffixes)
            {
                if (text.size() > suffix.size() &&
                    text.substr(text.size() - suffix.size()) == suffix)
                    return std::string(suffix);
            }
            return {};
        }

        static TypeRef SuffixedLiteralType(const Token& tok)
        {
            const std::string suffix = NumericLiteralSuffix(tok.text);
            if (suffix == "i8") return TypeRef::MakeInt8();
            if (suffix == "i16") return TypeRef::MakeInt16();
            if (suffix == "i32") return TypeRef::MakeInt32();
            if (suffix == "i64") return TypeRef::MakeInt64();
            if (suffix == "u8") return TypeRef::MakeUInt8();
            if (suffix == "u16") return TypeRef::MakeUInt16();
            if (suffix == "u32") return TypeRef::MakeUInt32();
            if (suffix == "u64") return TypeRef::MakeUInt64();
            if (suffix == "f32") return TypeRef::MakeFloat32();
            if (suffix == "f64") return TypeRef::MakeFloat64();
            return tok.kind == TokenKind::FloatLiteral ? TypeRef::MakeFloat64() : TypeRef::MakeInt();
        }

        static std::optional<TypeRef> BuiltinTypeFromName(const std::string& name)
        {
            if (name == "opaque") return TypeRef::MakeOpaque();
            if (name == "bool" || name == "bool8") return TypeRef::MakeBool8();
            if (name == "bool16") return TypeRef::MakeBool16();
            if (name == "bool32") return TypeRef::MakeBool32();
            if (name == "char" || name == "char32") return TypeRef::MakeChar32();
            if (name == "char8") return TypeRef::MakeChar8();
            if (name == "char16") return TypeRef::MakeChar16();
            if (name == "String") return TypeRef::MakeStr();
            if (name == "int8") return TypeRef::MakeInt8();
            if (name == "int16") return TypeRef::MakeInt16();
            if (name == "int32") return TypeRef::MakeInt32();
            if (name == "int64") return TypeRef::MakeInt64();
            if (name == "int") return TypeRef::MakeInt();
            if (name == "uint8") return TypeRef::MakeUInt8();
            if (name == "uint16") return TypeRef::MakeUInt16();
            if (name == "uint32") return TypeRef::MakeUInt32();
            if (name == "uint64") return TypeRef::MakeUInt64();
            if (name == "uint") return TypeRef::MakeUInt();
            if (name == "float32") return TypeRef::MakeFloat32();
            if (name == "float64") return TypeRef::MakeFloat64();
            if (name == "float") return TypeRef::MakeFloat();
            return std::nullopt;
        }

        static std::optional<TypeRef> SliceElementType(const TypeRef& type)
        {
            if (type.kind == TypeRef::Kind::Slice && !type.inner.empty())
                return type.inner[0];
            if (type.kind != TypeRef::Kind::Named) return std::nullopt;
            constexpr std::string_view prefix = "Slice<";
            if (!type.name.starts_with(prefix) || type.name.back() != '>') return std::nullopt;
            std::string elemName = type.name.substr(prefix.size(), type.name.size() - prefix.size() - 1);
            if (auto builtin = BuiltinTypeFromName(elemName)) return *builtin;
            return TypeRef::MakeNamed(elemName);
        }

        TypeRef ResolveType(const TypeExpr& expr)
        {
            if (auto* t = dynamic_cast<const NamedTypeExpr*>(&expr))
            {
                if (t->typeArgs.empty())
                {
                    for (const auto& tp : currentTypeParams)
                        if (tp == t->name) return TypeRef::MakeTypeParam(t->name);
                }

                Symbol* sym = currentScope->Lookup(t->name);
                if (sym && (sym->kind == Symbol::Kind::Type ||
                    sym->kind == Symbol::Kind::Interface))
                {
                    if (t->typeArgs.empty() && !sym->type.IsUnknown()) return sym->type; // builtin
                    return TypeRef::MakeNamed(GenericTypeName(*t)); // user-defined
                }
                EmitError(expr.location, std::format("unknown type '{}'", t->name));
                return TypeRef::MakeUnknown();
            }
            if (auto* t = dynamic_cast<const PathTypeExpr*>(&expr))
            {
                // Simplified: treat path as Named with last segment
                return TypeRef::MakeNamed(t->segments.back());
            }
            if (auto* t = dynamic_cast<const PointerTypeExpr*>(&expr))
                return TypeRef::MakePointer(ResolveType(*t->pointee));
            if (auto* t = dynamic_cast<const SliceTypeExpr*>(&expr))
                return TypeRef::MakeNamed(SliceTypeName(ResolveType(*t->element)));
            if (auto* t = dynamic_cast<const TupleTypeExpr*>(&expr))
            {
                std::vector<TypeRef> elems;
                for (auto& e : t->elements)
                    elems.push_back(ResolveType(*e));
                return TypeRef::MakeTuple(std::move(elems));
            }
            if (dynamic_cast<const SelfTypeExpr*>(&expr))
                return TypeRef::MakeNamed("self");
            return TypeRef::MakeUnknown();
        }

        TypeRef ResolveTypeWithSubstitution(
            const TypeExpr& expr,
            const std::unordered_map<std::string, TypeRef>& substitutions)
        {
            if (auto* t = dynamic_cast<const NamedTypeExpr*>(&expr))
            {
                if (t->typeArgs.empty())
                {
                    if (auto it = substitutions.find(t->name); it != substitutions.end())
                        return it->second;
                    return ResolveType(expr);
                }

                TypeRef named = TypeRef::MakeNamed(t->name);
                named.name += "<";
                for (std::size_t i = 0; i < t->typeArgs.size(); ++i)
                {
                    if (i) named.name += ", ";
                    named.name += ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions).ToString();
                }
                named.name += ">";
                return named;
            }
            if (auto* t = dynamic_cast<const PointerTypeExpr*>(&expr))
                return TypeRef::MakePointer(ResolveTypeWithSubstitution(*t->pointee, substitutions));
            if (auto* t = dynamic_cast<const SliceTypeExpr*>(&expr))
                return TypeRef::MakeNamed(SliceTypeName(ResolveTypeWithSubstitution(*t->element, substitutions)));
            if (auto* t = dynamic_cast<const TupleTypeExpr*>(&expr))
            {
                std::vector<TypeRef> elems;
                for (auto& elem : t->elements)
                    elems.push_back(ResolveTypeWithSubstitution(*elem, substitutions));
                return TypeRef::MakeTuple(std::move(elems));
            }
            return ResolveType(expr);
        }

        std::optional<std::uint64_t> SizeOfTypeRef(
            const TypeRef& type,
            const std::unordered_map<std::string, TypeRef>& substitutions = {})
        {
            if (type.kind == TypeRef::Kind::Named)
            {
                if (type.name.starts_with("Slice<")) return 16;
                if (auto it = substitutions.find(type.name); it != substitutions.end())
                    return SizeOfTypeRef(it->second, substitutions);
                return SizeOfStruct(BaseTypeName(type.name), substitutions);
            }

            if (type.kind == TypeRef::Kind::Range)
            {
                if (type.inner.empty()) return std::nullopt;
                const auto elemSize = SizeOfTypeRef(type.inner[0], substitutions);
                if (!elemSize || *elemSize == 0) return std::nullopt;
                return AlignUp(2 * *elemSize + 1, *elemSize);
            }

            if (type.kind == TypeRef::Kind::Tuple)
            {
                std::uint64_t total = 0;
                for (const auto& elem : type.inner)
                {
                    const auto elemSize = SizeOfTypeRef(elem, substitutions);
                    if (!elemSize) return std::nullopt;
                    total += *elemSize;
                }
                return total;
            }

            return type.SizeInBytes();
        }

        std::optional<std::uint64_t> SizeOfStruct(
            const std::string& name,
            const std::unordered_map<std::string, TypeRef>& substitutions = {})
        {
            const auto structIt = structDecls.find(name);
            if (structIt == structDecls.end()) return std::nullopt;

            std::uint64_t offset = 0;
            std::uint64_t maxAlign = 1;
            for (const auto& field : structIt->second->fields)
            {
                const auto fieldSize = SizeOfTypeExprWithSubstitution(*field.type, substitutions);
                if (!fieldSize) return std::nullopt;
                const std::uint64_t align = *fieldSize > 0 ? std::min<std::uint64_t>(*fieldSize, 8) : 1;
                if (align > 1) offset = AlignUp(offset, align);
                offset += *fieldSize > 0 ? *fieldSize : 8;
                maxAlign = std::max(maxAlign, align);
            }
            return AlignUp(offset, maxAlign);
        }

        std::optional<std::uint64_t> SizeOfTypeExprWithSubstitution(
            const TypeExpr& expr,
            const std::unordered_map<std::string, TypeRef>& substitutions = {})
        {
            if (auto* t = dynamic_cast<const NamedTypeExpr*>(&expr))
            {
                const auto structIt = structDecls.find(t->name);
                if (structIt != structDecls.end())
                {
                    std::unordered_map<std::string, TypeRef> fieldSubstitutions = substitutions;
                    const auto& params = structIt->second->typeParams;
                    for (std::size_t i = 0; i < params.size() && i < t->typeArgs.size(); ++i)
                        fieldSubstitutions[params[i]] =
                            ResolveTypeWithSubstitution(*t->typeArgs[i], substitutions);
                    return SizeOfStruct(t->name, fieldSubstitutions);
                }
            }

            return SizeOfTypeRef(ResolveTypeWithSubstitution(expr, substitutions), substitutions);
        }

        std::optional<std::uint64_t> SizeOfTypeExpr(const TypeExpr& expr)
        {
            return SizeOfTypeExprWithSubstitution(expr);
        }

        // ── Second pass: check declarations ───────────────────────────────────────

        void CheckModule(const Module& mod)
        {
            currentFile = mod.name;
            for (const auto& decl : mod.items)
                CheckDecl(*decl);
        }

        void CheckDecl(const Decl& decl)
        {
            if (auto* d = dynamic_cast<const FuncDecl*>(&decl))
                CheckFuncDecl(*d);
            else if (auto* d = dynamic_cast<const StructDecl*>(&decl))
                CheckStructDecl(*d);
            else if (auto* d = dynamic_cast<const EnumDecl*>(&decl))
                CheckEnumDecl(*d);
            else if (auto* d = dynamic_cast<const UnionDecl*>(&decl))
                CheckUnionDecl(*d);
            else if (auto* d = dynamic_cast<const InterfaceDecl*>(&decl))
                CheckInterfaceDecl(*d);
            else if (auto* d = dynamic_cast<const ImplDecl*>(&decl))
                CheckImplDecl(*d);
            else if (auto* d = dynamic_cast<const ModuleDecl*>(&decl))
                CheckModuleDecl(*d);
            else if (auto* d = dynamic_cast<const ConstDecl*>(&decl))
                CheckConstDecl(*d);
            else if (auto* d = dynamic_cast<const TypeAliasDecl*>(&decl))
                ResolveType(*d->type); // triggers unknown-type errors
            else if (auto* d = dynamic_cast<const ExternFuncDecl*>(&decl))
            {
                if (d->dll.empty())
                    EmitError(d->location,
                              std::format(
                                  "extern function '{}' must specify a source DLL via @[Import(lib: \"dll.dll\")]",
                                  d->name));
                if (d->returnType) ResolveType(*d->returnType->get());
                for (auto& p : d->params)
                    if (!p.isVariadic) ResolveType(*p.type);
            }
            else if (auto* d = dynamic_cast<const ExternVarDecl*>(&decl))
                ResolveType(*d->type);
            else if (auto* d = dynamic_cast<const ExternBlockDecl*>(&decl))
                for (auto& item : d->items)
                    CheckDecl(*item);
            // UseDecl: import resolution deferred
        }

        void CheckFuncDecl(const FuncDecl& d, bool isMethod = false)
        {
            auto savedTypeParams = currentTypeParams;
            currentTypeParams = d.typeParams;

            TypeRef retType = d.returnType
                                  ? ResolveType(*d.returnType->get())
                                  : TypeRef::MakeOpaque();

            auto savedRet = currentReturnType;
            currentReturnType = retType;

            PushScope();

            for (const auto& tp : d.typeParams)
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Type;
                sym.name = tp;
                sym.type = TypeRef::MakeTypeParam(tp);
                Define(sym);
            }

            if (isMethod)
            {
                Symbol self;
                self.kind = Symbol::Kind::Var;
                self.name = "self";
                self.type = TypeRef::MakeNamed("self");
                self.isMut = true;
                Define(self);
            }

            for (const auto& param : d.params)
            {
                if (param.isVariadic) continue;
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = param.name;
                sym.location = param.location;
                sym.type = ResolveType(*param.type);
                sym.isMut = false;
                Define(sym);
            }

            if (!d.body)
                EmitError(d.location,
                          std::format("function '{}' has no body", d.name));
            else
                CheckBlock(*d.body);

            PopScope();
            currentReturnType = savedRet;
            currentTypeParams = savedTypeParams;
        }

        void CheckStructDecl(const StructDecl& d)
        {
            auto savedTypeParams = currentTypeParams;
            currentTypeParams = d.typeParams;

            PushScope();
            for (const auto& tp : d.typeParams)
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Type;
                sym.name = tp;
                sym.type = TypeRef::MakeTypeParam(tp);
                Define(sym);
            }

            std::unordered_set<std::string> seen;
            for (const auto& field : d.fields)
            {
                if (!seen.insert(field.name).second)
                    EmitError(field.location,
                              std::format("duplicate field '{}' in struct '{}'", field.name, d.name));
                ResolveType(*field.type);
            }

            PopScope();
            currentTypeParams = savedTypeParams;
        }

        void CheckEnumDecl(const EnumDecl& d)
        {
            std::unordered_set<std::string> seen;
            for (const auto& variant : d.variants)
            {
                if (!seen.insert(variant.name).second)
                    EmitError(variant.location,
                              std::format("duplicate variant '{}' in enum '{}'", variant.name, d.name));
                for (const auto& f : variant.fields)
                    ResolveType(*f);
            }
        }

        void CheckUnionDecl(const UnionDecl& d)
        {
            std::unordered_set<std::string> seen;
            for (const auto& field : d.fields)
            {
                if (!seen.insert(field.name).second)
                    EmitError(field.location,
                              std::format("duplicate field '{}' in union '{}'", field.name, d.name));
                ResolveType(*field.type);
            }
        }

        void CheckInterfaceDecl(const InterfaceDecl& d)
        {
            std::unordered_set<std::string> seen;
            for (const auto& method : d.methods)
            {
                if (!seen.insert(method->name).second)
                    EmitError(method->location,
                              std::format("duplicate method '{}' in interface '{}'",
                                          method->name, d.name));
                if (method->returnType)
                    ResolveType(*method->returnType->get());
                for (const auto& p : method->params)
                    if (!p.isVariadic) ResolveType(*p.type);
            }
        }

        void CheckImplDecl(const ImplDecl& d)
        {
            if (!currentScope->Lookup(d.typeName))
                EmitError(d.location,
                          std::format("impl for unknown type '{}'", d.typeName));

            if (d.interfaceName)
            {
                Symbol* ifaceSym = currentScope->Lookup(*d.interfaceName);
                if (!ifaceSym || ifaceSym->kind != Symbol::Kind::Interface)
                {
                    EmitError(d.location,
                              std::format("'{}' is not a known interface", *d.interfaceName));
                }
                else
                {
                    std::unordered_set<std::string> implNames;
                    for (const auto& m : d.methods)
                        implNames.insert(m->name);
                    for (const auto& required : ifaceSym->interfaceMethods)
                    {
                        if (!implNames.count(required))
                            EmitError(d.location,
                                      std::format("impl of '{}' for '{}' is missing method '{}'",
                                                  *d.interfaceName, d.typeName, required));
                    }
                }
            }

            bool savedInImpl = inImpl;
            inImpl = true;
            for (const auto& m : d.methods)
                CheckFuncDecl(*m, /*isMethod=*/true);
            inImpl = savedInImpl;
        }

        void CheckModuleDecl(const ModuleDecl& d)
        {
            PushScope();
            for (const auto& item : d.items)
                CheckDecl(*item);
            PopScope();
        }

        void CheckConstDecl(const ConstDecl& d)
        {
            TypeRef valueType = CheckExpr(*d.value);
            TypeRef constType = d.type ? ResolveType(*d.type->get()) : valueType;
            if (d.type && !valueType.IsUnknown() && !constType.IsUnknown() &&
                !valueType.IsAssignableTo(constType))
                EmitError(d.value->location,
                          std::format("cannot assign '{}' to constant of type '{}'",
                                      valueType.ToString(), constType.ToString()));
            if (Symbol* sym = currentScope->Lookup(d.name))
                sym->type = constType;
        }

        // ── Block & statements ────────────────────────────────────────────────────

        void CheckBlock(const Block& block)
        {
            PushScope();
            for (const auto& stmt : block.stmts)
                CheckStmt(*stmt);
            PopScope();
        }

        void CheckStmt(const Stmt& stmt)
        {
            if (auto* s = dynamic_cast<const ExprStmt*>(&stmt))
            {
                CheckExpr(*s->expr);
            }
            else if (auto* s = dynamic_cast<const LetStmt*>(&stmt))
            {
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
            }
            else if (auto* s = dynamic_cast<const IfStmt*>(&stmt))
            {
                CheckExpr(*s->condition);
                CheckBlock(*s->thenBlock);
                for (const auto& elif : s->elseIfs)
                {
                    CheckExpr(*elif.condition);
                    CheckBlock(*elif.block);
                }
                if (s->elseBlock)
                    CheckBlock(*s->elseBlock);
            }
            else if (auto* s = dynamic_cast<const WhileStmt*>(&stmt))
            {
                if (!s->label.empty()) activeLabels.insert(s->label);
                CheckExpr(*s->condition);
                ++loopDepth;
                CheckBlock(*s->body);
                --loopDepth;
                if (!s->label.empty()) activeLabels.erase(s->label);
            }
            else if (auto* s = dynamic_cast<const DoWhileStmt*>(&stmt))
            {
                if (!s->label.empty()) activeLabels.insert(s->label);
                ++loopDepth;
                CheckBlock(*s->body);
                --loopDepth;
                CheckExpr(*s->condition);
                if (!s->label.empty()) activeLabels.erase(s->label);
            }
            else if (auto* s = dynamic_cast<const LoopStmt*>(&stmt))
            {
                if (!s->label.empty()) activeLabels.insert(s->label);
                ++loopDepth;
                CheckBlock(*s->body);
                --loopDepth;
                if (!s->label.empty()) activeLabels.erase(s->label);
            }
            else if (auto* s = dynamic_cast<const ForStmt*>(&stmt))
            {
                TypeRef iterType = CheckExpr(*s->iterable);
                PushScope(); // scope for the loop variable
                Symbol var;
                var.kind = Symbol::Kind::Var;
                var.name = s->variable;
                var.location = s->location;
                if (iterType.IsRange() && !iterType.inner.empty())
                    var.type = iterType.inner[0];
                else
                    var.type = TypeRef::MakeUnknown();
                var.isMut = false;
                Define(var);
                if (!s->label.empty()) activeLabels.insert(s->label);
                ++loopDepth;
                CheckBlock(*s->body); // CheckBlock pushes its own nested scope
                --loopDepth;
                if (!s->label.empty()) activeLabels.erase(s->label);
                PopScope();
            }
            else if (auto* s = dynamic_cast<const MatchStmt*>(&stmt))
            {
                CheckExpr(*s->subject);
                for (const auto& arm : s->arms)
                {
                    PushScope(); // each arm has its own binding scope
                    CheckPattern(*arm.pattern);
                    CheckExpr(*arm.body);
                    PopScope();
                }
            }
            else if (auto* s = dynamic_cast<const ReturnStmt*>(&stmt))
            {
                if (s->value)
                {
                    if (TypeRef valType = CheckExpr(**s->value);
                        !valType.IsUnknown() && !currentReturnType.IsUnknown() &&
                        !currentReturnType.IsOpaque() &&
                        !valType.IsAssignableTo(currentReturnType))
                        EmitError(s->location,
                                  std::format("return type mismatch: expected '{}', found '{}'",
                                              currentReturnType.ToString(), valType.ToString()));
                }
                else if (!currentReturnType.IsOpaque() && !currentReturnType.IsUnknown())
                {
                    EmitError(s->location,
                              std::format("missing return value; expected '{}'",
                                          currentReturnType.ToString()));
                }
            }
            else if (auto* s = dynamic_cast<const BreakStmt*>(&stmt))
            {
                if (loopDepth == 0)
                    EmitError(stmt.location, "'break' outside of a loop");
                else if (!s->label.empty() && !activeLabels.count(s->label))
                    EmitError(stmt.location, std::format("unknown loop label '{}'", s->label));
            }
            else if (auto* s = dynamic_cast<const ContinueStmt*>(&stmt))
            {
                if (loopDepth == 0)
                    EmitError(stmt.location, "'continue' outside of a loop");
                else if (!s->label.empty() && !activeLabels.count(s->label))
                    EmitError(stmt.location, std::format("unknown loop label '{}'", s->label));
            }
            else if (auto* s = dynamic_cast<const DeclStmt*>(&stmt))
            {
                CollectDecl(*s->decl, *currentScope);
                CheckDecl(*s->decl);
            }
        }

        void CheckPattern(const Pattern& pat)
        {
            if (auto* p = dynamic_cast<const IdentPattern*>(&pat))
            {
                Symbol sym;
                sym.kind = Symbol::Kind::Var;
                sym.name = p->name;
                sym.location = p->location;
                sym.type = TypeRef::MakeUnknown();
                sym.isMut = false;
                Define(sym);
            }
            else if (auto* p = dynamic_cast<const GuardedPattern*>(&pat))
            {
                CheckPattern(*p->inner);
                CheckExpr(*p->guard);
            }
            else if (auto* p = dynamic_cast<const RangePattern*>(&pat))
            {
                CheckPattern(*p->lo);
                CheckPattern(*p->hi);
            }
            else if (auto* p = dynamic_cast<const TuplePattern*>(&pat))
            {
                for (const auto& e : p->elements)
                    CheckPattern(*e);
            }
            else if (auto* p = dynamic_cast<const StructPattern*>(&pat))
            {
                if (!currentScope->Lookup(p->typeName))
                    EmitError(p->location,
                              std::format("unknown type '{}' in struct pattern", p->typeName));
                for (const auto& f : p->fields)
                    CheckPattern(*f.pattern);
            }
            else if (auto* p = dynamic_cast<const EnumPattern*>(&pat))
            {
                if (!p->path.empty() && !currentScope->Lookup(p->path[0]))
                    EmitError(p->location,
                              std::format("unknown name '{}' in enum pattern", p->path[0]));
                for (const auto& a : p->args)
                    CheckPattern(*a);
            }
            // WildcardPattern, LiteralPattern: nothing to resolve
        }

        // ── Expressions ───────────────────────────────────────────────────────────

        TypeRef CheckExpr(const Expr& expr)
        {
            if (auto* e = dynamic_cast<const LiteralExpr*>(&expr))
                return LiteralType(e->token);

            if (auto* e = dynamic_cast<const IdentExpr*>(&expr))
            {
                Symbol* sym = currentScope->Lookup(e->name);
                if (sym) return sym->type;
                EmitError(e->location, std::format("undefined name '{}'", e->name));
                return TypeRef::MakeUnknown();
            }

            if (dynamic_cast<const SelfExpr*>(&expr))
            {
                if (!inImpl)
                    EmitError(expr.location, "'self' used outside of an impl block");
                return TypeRef::MakeNamed("self");
            }

            if (auto* e = dynamic_cast<const PathExpr*>(&expr))
            {
                if (!e->segments.empty() && !currentScope->Lookup(e->segments[0]))
                    EmitError(e->location,
                              std::format("undefined name '{}'", e->segments[0]));
                return TypeRef::MakeUnknown();
            }

            if (auto* e = dynamic_cast<const SizeOfExpr*>(&expr))
            {
                TypeRef t = ResolveType(*e->type);
                if (!t.IsUnknown() && !SizeOfTypeExpr(*e->type))
                    EmitError(e->location,
                              std::format("cannot determine size of type '{}'", t.ToString()));
                return TypeRef::MakeUInt64();
            }

            if (auto* e = dynamic_cast<const UnaryExpr*>(&expr))
            {
                if (e->op == TokenKind::PlusPlus || e->op == TokenKind::MinusMinus)
                    CheckMutability(*e->operand);
                TypeRef t = CheckExpr(*e->operand);
                return CheckUnary(e->op, t, e->location);
            }

            if (auto* e = dynamic_cast<const PostfixExpr*>(&expr))
            {
                CheckMutability(*e->operand);
                TypeRef t = CheckExpr(*e->operand);
                if (!t.IsUnknown() && !t.IsNumeric())
                    EmitError(e->location,
                              std::format("'{}' applied to non-numeric type '{}'",
                                          e->op == TokenKind::PlusPlus ? "++" : "--",
                                          t.ToString()));
                return t;
            }

            if (auto* e = dynamic_cast<const BinaryExpr*>(&expr))
            {
                TypeRef l = CheckExpr(*e->left);
                TypeRef r = CheckExpr(*e->right);
                return CheckBinary(e->op, l, r, e->location);
            }

            if (auto* e = dynamic_cast<const AssignExpr*>(&expr))
            {
                CheckMutability(*e->target);
                TypeRef tgt = CheckExpr(*e->target);
                TypeRef val = CheckExpr(*e->value);
                if (!tgt.IsUnknown() && !val.IsUnknown() && !val.IsAssignableTo(tgt))
                    EmitError(e->location,
                              std::format("cannot assign '{}' to '{}'",
                                          val.ToString(), tgt.ToString()));
                return TypeRef::MakeOpaque();
            }

            if (auto* e = dynamic_cast<const TernaryExpr*>(&expr))
            {
                TypeRef cond = CheckExpr(*e->condition);
                if (!cond.IsUnknown() && !cond.IsBool())
                    EmitError(e->condition->location, "ternary condition must be 'bool'");
                TypeRef thenT = CheckExpr(*e->thenExpr);
                TypeRef elseT = CheckExpr(*e->elseExpr);
                return thenT.IsUnknown() ? elseT : thenT;
            }

            if (auto* e = dynamic_cast<const RangeExpr*>(&expr))
            {
                TypeRef loType = e->lo ? CheckExpr(*e->lo) : TypeRef::MakeUnknown();
                TypeRef hiType = e->hi ? CheckExpr(*e->hi) : TypeRef::MakeUnknown();
                if (!loType.IsUnknown() && !hiType.IsUnknown() &&
                    !loType.IsNumeric() && !hiType.IsNumeric())
                    EmitError(e->location, "range operands must be numeric");
                TypeRef elemType = loType.IsUnknown() ? hiType : loType;
                if (elemType.IsUnknown()) elemType = TypeRef::MakeInt64();
                return TypeRef::MakeRange(elemType);
            }

            if (auto* e = dynamic_cast<const CallExpr*>(&expr))
            {
                TypeRef calleeType = CheckExpr(*e->callee);
                std::vector<TypeRef> argTypes;
                argTypes.reserve(e->args.size());
                for (const auto& arg : e->args)
                    argTypes.push_back(CheckExpr(*arg));

                if (calleeType.kind == TypeRef::Kind::Func && !calleeType.inner.empty())
                {
                    const std::size_t paramCount = calleeType.inner.size() - 1;
                    if (argTypes.size() != paramCount)
                    {
                        EmitError(e->location,
                                  std::format("function expects {} argument(s), got {}",
                                              paramCount, argTypes.size()));
                    }
                    else
                    {
                        for (std::size_t i = 0; i < argTypes.size(); ++i)
                        {
                            const TypeRef& argType = argTypes[i];
                            const TypeRef& paramType = calleeType.inner[i];
                            if (!argType.IsUnknown() && !paramType.IsUnknown() &&
                                !argType.IsAssignableTo(paramType))
                                EmitError(e->args[i]->location,
                                          std::format("cannot pass '{}' to parameter of type '{}'",
                                                      argType.ToString(), paramType.ToString()));
                        }
                    }
                    return calleeType.inner.back();
                }
                return TypeRef::MakeUnknown();
            }

            if (auto* e = dynamic_cast<const IndexExpr*>(&expr))
            {
                TypeRef obj = CheckExpr(*e->object);
                CheckExpr(*e->index);
                if (auto elemType = SliceElementType(obj))
                    return *elemType;
                return TypeRef::MakeUnknown();
            }

            if (auto* e = dynamic_cast<const FieldExpr*>(&expr))
            {
                TypeRef obj = CheckExpr(*e->object);
                if (auto elemType = SliceElementType(obj))
                {
                    if (e->field == "data") return TypeRef::MakePointer(*elemType);
                    if (e->field == "length") return TypeRef::MakeUInt64();
                    EmitError(e->location,
                              std::format("unknown field '{}' on type '{}'",
                                          e->field, obj.ToString()));
                    return TypeRef::MakeUnknown();
                }
                if (obj.IsRange())
                {
                    TypeRef elemType = obj.inner.empty() ? TypeRef::MakeInt64() : obj.inner[0];
                    if (e->field == "lo" || e->field == "hi") return elemType;
                    if (e->field == "inclusive") return TypeRef::MakeBool();
                    EmitError(e->location,
                              std::format("unknown field '{}' on type '{}'",
                                          e->field, obj.ToString()));
                    return TypeRef::MakeUnknown();
                }
                return TypeRef::MakeUnknown(); // field type lookup needs full type info
            }

            if (auto* e = dynamic_cast<const StructInitExpr*>(&expr))
            {
                if (!currentScope->Lookup(e->typeName))
                    EmitError(e->location,
                              std::format("unknown type '{}' in struct initializer", e->typeName));
                for (const auto& f : e->fields) CheckExpr(*f.value);
                return TypeRef::MakeNamed(GenericStructInitName(*e));
            }

            if (auto* e = dynamic_cast<const SliceExpr*>(&expr))
            {
                TypeRef elemType = TypeRef::MakeUnknown();
                for (const auto& el : e->elements)
                {
                    TypeRef t = CheckExpr(*el);
                    if (elemType.IsUnknown()) elemType = t;
                }
                return TypeRef::MakeNamed(SliceTypeName(elemType));
            }

            if (auto* e = dynamic_cast<const CastExpr*>(&expr))
            {
                CheckExpr(*e->operand);
                return ResolveType(*e->type);
            }

            if (auto* e = dynamic_cast<const IsExpr*>(&expr))
            {
                CheckExpr(*e->operand);
                ResolveType(*e->type);
                return TypeRef::MakeBool();
            }

            if (auto* e = dynamic_cast<const BlockExpr*>(&expr))
            {
                CheckBlock(*e->block);
                return TypeRef::MakeUnknown();
            }

            return TypeRef::MakeUnknown();
        }

        static TypeRef LiteralType(const Token& tok)
        {
            switch (tok.kind)
            {
            case TokenKind::IntLiteral:
            case TokenKind::FloatLiteral: return SuffixedLiteralType(tok);
            case TokenKind::StringLiteral: return StringLiteralType(tok);
            case TokenKind::CharLiteral: return CharLiteralType(tok);
            case TokenKind::BoolLiteral: return TypeRef::MakeBool();
            default: return TypeRef::MakeUnknown();
            }
        }

        TypeRef CheckUnary(TokenKind op, const TypeRef& t, SourceLocation loc)
        {
            if (t.IsUnknown()) return TypeRef::MakeUnknown();
            switch (op)
            {
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
            case TokenKind::PlusPlus:
            case TokenKind::MinusMinus:
                if (!t.IsNumeric())
                    EmitError(loc, std::format("'{}' applied to non-numeric type '{}'",
                                               op == TokenKind::PlusPlus ? "++" : "--", t.ToString()));
                return t;
            default:
                return TypeRef::MakeUnknown();
            }
        }

        TypeRef CheckBinary(TokenKind op, const TypeRef& l, const TypeRef& r, SourceLocation loc)
        {
            if (l.IsUnknown() || r.IsUnknown()) return TypeRef::MakeUnknown();

            using TK = TokenKind;
            switch (op)
            {
            case TK::Plus:
                if (l.kind == TypeRef::Kind::Pointer && r.IsInteger())
                    return l;
                if (l.IsInteger() && r.kind == TypeRef::Kind::Pointer)
                    return r;
                if (!l.IsNumeric())
                    EmitError(loc, std::format("'+' applied to non-numeric type '{}'", l.ToString()));
                return l;

            case TK::Minus:
                if (l.kind == TypeRef::Kind::Pointer && r.IsInteger())
                    return l;
                if (!l.IsNumeric())
                    EmitError(loc, std::format("'-' applied to non-numeric type '{}'", l.ToString()));
                return l;

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
        void CheckMutability(const Expr& target)
        {
            if (auto* e = dynamic_cast<const IdentExpr*>(&target))
            {
                Symbol* sym = currentScope->Lookup(e->name);
                if (!sym) return;
                if (sym->kind == Symbol::Kind::Const)
                {
                    EmitError(target.location,
                              std::format("cannot assign to constant '{}'", e->name));
                    return;
                }
                if (sym->kind == Symbol::Kind::Var && !sym->isMut)
                {
                    EmitError(target.location,
                              std::format("cannot assign to immutable variable '{}'", e->name));
                }
            }
            // Field and index targets: would need full type info to check properly
        }
    };

    // ── Sema public API ───────────────────────────────────────────────────────────

    Sema::Sema(std::vector<const Module*> modules)
        : modules(std::move(modules))
    {
    }

    SemaResult Sema::Analyze()
    {
        Analyzer analyzer(modules, diags, symbols);
        analyzer.Run();
        return SemaResult{std::move(diags), std::move(symbols)};
    }

    bool Sema::DumpResult(const SemaResult& result, const std::filesystem::path& path)
    {
        std::ofstream out(path);
        if (!out) return false;

        static constexpr auto kindName = [](SemaSymbol::Kind k) -> std::string_view
        {
            switch (k)
            {
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

        if (result.symbols.empty())
        {
            out << "(none)\n";
        }
        else
        {
            for (const auto& sym : result.symbols)
            {
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

        if (result.diagnostics.empty())
        {
            out << "(none)\n";
        }
        else
        {
            for (const auto& diag : result.diagnostics)
            {
                const char* sev = diag.severity == SemaDiagnostic::Severity::Error
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
