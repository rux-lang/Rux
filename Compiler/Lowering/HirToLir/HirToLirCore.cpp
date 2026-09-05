// Package, module, checked-builder, and function setup for HIR-to-LIR lowering.

#include "Lowering/HirToLir/HirToLir.h"
#include "Lowering/HirToLir/HirToLirContext.h"
#include "Types/PrimitiveCatalog.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string_view>
#include <utility>

namespace Rux::HirToLirDetail {
namespace {
/// Whether `text` names `identifier` as a whole word, so that a type argument list mentioning `T` is told apart from
/// one mentioning `Tag`.
[[nodiscard]] bool ContainsIdentifier(const std::string_view text, const std::string_view identifier) {
    const auto isWordCharacter = [](const char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    };
    for (std::size_t pos = text.find(identifier); pos != std::string_view::npos; pos = text.find(identifier, pos + 1)) {
        const std::size_t end = pos + identifier.size();
        if ((pos == 0 || !isWordCharacter(text[pos - 1])) && (end == text.size() || !isWordCharacter(text[end]))) {
            return true;
        }
    }
    return false;
}
} // namespace

HirToLirContext::HirToLirContext(const TargetContext &target, std::vector<Diagnostic> &outputDiagnostics)
    : targetContext(target)
    , diagnostics(outputDiagnostics) {
}

LirPackage HirToLirContext::Run(const HirPackage &hir) {
    globalConsts.clear();
    for (const auto &mod : hir.modules) {
        for (const auto &iface : mod.interfaces) {
            interfacesByName[iface.name] = &iface;
        }
    }
    // How wide an enum's tag is stored, which decides how wide it must be read back. A variant carrying fields packs
    // its tag and payload into one word, so those enums are always full width; a plain C-like enum is stored at its
    // declared base type, which may be a single byte. Collected package-wide because a match may name an enum
    // declared in another module.
    enumTagTypes.clear();
    genericPayloadEnums.clear();
    for (const auto &mod : hir.modules) {
        for (const auto &declaration : mod.enums) {
            TypeRef tagType = TypeRef::MakeInt64();
            if (!declaration.IsVariant() && !declaration.baseType.IsUnknown()) {
                tagType = declaration.baseType;
            }
            enumTagTypes[declaration.name] = tagType;
            enumTagTypes[mod.name + "::" + declaration.name] = tagType;
            // Collected on the same terms, and for the same reason, as the tag widths above: which of these an
            // instantiation belongs to is what says a missing layout marker is a bug rather than a compact enum.
            if (declaration.IsVariant() && !declaration.typeParams.empty()) {
                genericPayloadEnums[declaration.name] = declaration.typeParams;
                genericPayloadEnums[mod.name + "::" + declaration.name] = declaration.typeParams;
            }
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
    pkg.dropGlues = hir.dropGlues;
    for (const auto &mod : hir.modules) {
        pkg.modules.push_back(LowerModule(mod));
    }
    // Glue is synthesized once the modules are lowered, because a plan describes a type rather than a module and every
    // cleanup in the package names the same symbol for it. It joins the last module for the same reason a monomorphized
    // instance does: a function has to belong to one to be emitted.
    if (!pkg.modules.empty()) {
        for (LirFunc &glue : SynthesizeDropGlue(hir.dropGlues)) {
            pkg.modules.back().funcs.push_back(std::move(glue));
        }
    }
    return pkg;
}

/// The name a function reaches the linker under: the second `#Link` argument override when one was given, otherwise the
/// Rux name itself.
const std::string &HirToLirContext::SymbolFor(const std::string &name) const {
    const auto it = externSymbols.find(name);
    return it == externSymbols.end() ? name : it->second;
}

/// C's default argument promotions, which apply to the types C itself has.
///
/// A bool wider than `int` is not one of them: C has no such type, so there is nothing to promote it to and it is
/// passed at its own width. The same holds for every bool at or above 32 bits.
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

/// Preserve the declaration boundary on every target. Apple and FreeBSD AArch64 apply C's default argument promotions
/// before their platform call layouts; keeping the promotion in LIR makes the value's ABI type explicit and lets code
/// generation move it like any other converted value.
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

void HirToLirContext::BuilderFailure(std::string detail) const {
    diagnostics.push_back(ErrorDiagnostic(
        std::format("invalid internal LIR while lowering function '{}': {}",
                    currentFunction.empty() ? "<unknown>" : currentFunction, std::move(detail)),
        {"HIR-to-LIR lowering generated an instruction or control-flow edge rejected by the checked builder"},
        "please report this compiler bug with a minimal source example"));
}

[[nodiscard]] LirOpcode HirToLirContext::RequireOpcode(const std::optional<LirOpcode> opcode) {
    if (!opcode) {
        BuilderFailure("source operator has no LIR opcode mapping");
        return LirOpcode::Add;
    }
    return *opcode;
}

[[nodiscard]] std::uint32_t HirToLirContext::NewBlock(std::string label) const {
    return builder->CreateBlock(std::move(label));
}

void HirToLirContext::SetBlock(const std::uint32_t idx) {
    if (!builder->SelectBlock(idx)) {
        BuilderFailure(std::format("cannot select block {}: {}", idx, builder->FailureReason()));
    }
}

[[nodiscard]] bool HirToLirContext::IsTerminated() const {
    return builder->IsTerminated();
}

// Instruction emission
void HirToLirContext::Emit(LirInstr i) const {
    if (!builder->Insert(std::move(i))) {
        BuilderFailure("cannot insert instruction into the current block: " + std::string(builder->FailureReason()));
    }
}

void HirToLirContext::Terminate(LirTerminator t) const {
    if (!builder->Terminate(std::move(t))) {
        BuilderFailure("cannot terminate the current block: " + std::string(builder->FailureReason()));
    }
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

void HirToLirContext::EmitStore(LirReg val, LirReg ptr, TypeRef type, const bool isVolatile) const {
    LirInstr i;
    i.dst = LirNoReg;
    i.op = LirOpcode::Store;
    i.type = std::move(type);
    i.srcs = {val, ptr};
    i.isVolatile = isVolatile;
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
    // A bool holds one of two values however wide its storage is, so a cast into one asks whether the source is
    // non-zero rather than what its low bits are. Emitting the comparison here keeps the rule in one place and gives
    // both back ends the normalized zero-or-one they already produce for every other comparison.
    if (toType.IsBool() && !fromType.IsBool() && IsScalar(fromType)) {
        const LirReg zero = EmitConst("0", fromType);
        const LirReg tested = EmitBinary(LirOpcode::CmpNe, src, zero, TypeRef::MakeBool());
        return TypeRef::MakeBool() == toType ? tested : EmitWidenBool(tested, toType);
    }

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

/// Restate an already-normalized `bool8` at a wider bool's storage.
///
/// The value is zero or one either way, so this only changes the width it is stored at and can never lose or invent a
/// bit -- which is why it stays a plain cast rather than going back through the zero test above.
LirReg HirToLirContext::EmitWidenBool(const LirReg source, const TypeRef &toType) {
    LirReg dst = NewReg();
    LirInstr widen;
    widen.dst = dst;
    widen.op = LirOpcode::Cast;
    widen.type = toType;
    widen.srcs = {source};
    widen.strArg = TypeRef::MakeBool().ToString();
    Emit(std::move(widen));
    return dst;
}

/// The types that live in a register and are held to a width: everything a comparison can widen one side of without
/// changing what is being asked.
bool HirToLirContext::IsScalar(const TypeRef &t) {
    return t.IsNumeric() || t.IsBool() || t.IsChar();
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

bool HirToLirContext::IsPointerArithmetic(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Pointer && !type.inner.empty();
}

/// Offsets a pointer by a count of elements, scaling by the pointee size.
///
/// The scaling is deliberately left to IndexPtr rather than emitted here as a multiply. Only the back end can size a
/// struct pointee: it owns the layout map, while `TypeRef::SizeInBytes` is a purely syntactic query that answers
/// nothing for a named type. Emitting the multiply here silently skipped it for every struct, so `ptr + 1` advanced one
/// byte instead of one element. Routing through IndexPtr gives pointer arithmetic and `ptr[n]` one scaling rule.
LirReg HirToLirContext::EmitPointerOffset(LirReg base, LirReg index, const TypeRef &pointerType) {
    return EmitIndexPtr(base, index, pointerType.inner[0]);
}

/// Moves a pointer one element forward or back, for `++` and `--`. A negative index is correct for the back end's
/// signed multiply, and stays correct when the caller's own index type is unsigned, because the two's-complement
/// product is the same either way.
LirReg HirToLirContext::EmitPointerStep(LirReg base, const TypeRef &pointerType, bool forward) {
    const LirReg step = EmitConst(forward ? "1" : "-1", TypeRef::MakeInt());
    return EmitPointerOffset(base, step, pointerType);
}

/// `pointee` names what the symbol holds, so that a later FieldPtr can find the layout. Opaque is right for a function
/// address, which is never dereferenced.
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
    const TypeRef &candidate = t.kind == TypeRef::Kind::Reference && !t.inner.empty() ? t.inner.front() : t;
    return candidate.kind == TypeRef::Kind::Named && interfacesByName.contains(candidate.name);
}

bool HirToLirContext::IsSliceType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Named && type.name.starts_with("Slice<");
}

/// Whether a value is a 16-byte `{data, length}` view. A string has a slice's representation exactly, reaching its
/// code units through the same two fields, so every rule about how such a value is stored, copied, and passed holds
/// for both. What differs between them -- writability, iteration, sub-ranging -- semantic analysis has already
/// settled before anything reaches here.
bool HirToLirContext::IsViewType(const TypeRef &type) {
    return IsSliceType(type) || type.IsString();
}

bool HirToLirContext::IsArrayType(const TypeRef &type) {
    return type.kind == TypeRef::Kind::Array;
}

/// Whether a type names an instantiation of a generic enum that carries payloads -- the shape whose size the front
/// end records as a layout marker. A type argument still naming one of the declaration's own parameters is the
/// generic declaration rather than an instantiation of it, and has no layout until it has been substituted.
bool HirToLirContext::IsInstantiatedPayloadEnum(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named || !type.name.ends_with('>')) {
        return false;
    }
    const std::size_t open = type.name.find('<');
    if (open == std::string::npos) {
        return false;
    }
    const auto declared = genericPayloadEnums.find(type.name.substr(0, open));
    if (declared == genericPayloadEnums.end()) {
        return false;
    }
    const std::string_view args(type.name.data() + open + 1, type.name.size() - open - 2);
    return std::ranges::none_of(declared->second,
                                [&](const std::string &param) { return ContainsIdentifier(args, param); });
}

/// How many bytes an enum instantiation occupies, read from the layout marker the front end attaches to the type.
///
/// This is the only place that marker is read, because the answer decides the enum's representation and a construct
/// and a match that disagreed about it would build one shape and decode another. A generic enum with payloads that
/// reaches lowering without a marker is a front-end bug rather than a compact enum, and is reported as one: silently
/// treating it as compact is what turned a returned `Option<int32>` into a packed word its callee never wrote.
std::optional<std::uint64_t> HirToLirContext::EnumLayoutSize(const TypeRef &type) const {
    if (type.kind != TypeRef::Kind::Named) {
        return std::nullopt;
    }
    if (!type.inner.empty() && type.inner[0].kind == TypeRef::Kind::Array) {
        return type.SizeInBytes();
    }
    if (IsInstantiatedPayloadEnum(type)) {
        BuilderFailure(std::format("variant type '{}' reached lowering without a layout marker", type.name));
    }
    return std::nullopt;
}

bool HirToLirContext::IsAggregateEnumType(const TypeRef &type) const {
    // Semantic analysis attaches an array marker containing the complete storage size to every payload enum. Even a
    // marker of eight bytes or fewer needs the aggregate path: the compact representation has no addressable payload
    // storage for destruction or borrowing.
    return EnumLayoutSize(type).has_value();
}

/// The type an enum's tag is stored as, which is what a match has to read it back as. Defaults to a full word, so an
/// enum this pass never saw declared behaves as it always did.
TypeRef HirToLirContext::EnumTagType(const TypeRef &enumType) const {
    if (enumType.kind != TypeRef::Kind::Named) {
        return TypeRef::MakeInt64();
    }
    const auto found = enumTagTypes.find(enumType.name);
    return found == enumTagTypes.end() ? TypeRef::MakeInt64() : found->second;
}

bool HirToLirContext::IsStringSliceLiteral(const HirLiteralExpr &e) {
    if (e.type.IsString()) {
        return true;
    }
    return e.type.kind == TypeRef::Kind::Named &&
           (e.type.name == "Slice<char8>" || e.type.name == "Slice<char16>" || e.type.name == "Slice<char32>");
}

TypeRef HirToLirContext::StringSliceElementType(const HirLiteralExpr &e) {
    if (e.type.IsString()) {
        return TypeRef::MakePrimitive(StringCodeUnitKind(e.type.kind));
    }
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
        ed.form = e.form;
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
        if (impl.methods.size() != impl.methodLinkerNames.size()) {
            BuilderFailure("implementation method and linker-name tables have different sizes");
        }
        for (std::size_t i = 0; i < std::min(impl.methods.size(), impl.methodLinkerNames.size()); ++i) {
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

/// Render a simple constant expression to a printable string.
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

/// The literal an array element spells out, with a leading minus folded in and a named constant resolved to the literal
/// it stands for. Anything else is not a constant the backend can lay out; the semantic analyzer rejects those before
/// we get here.
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

/// Records constant slice/array contents for direct read-only emission.
void HirToLirContext::CollectConstContents(const HirConst &c, LirConstDecl &cd) const {
    if (!IsViewType(c.type) && !IsArrayType(c.type)) {
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
        const std::size_t count =
            arr->repeatedElement ? static_cast<std::size_t>(arr->repeatCount) : arr->elements.size();
        for (std::size_t index = 0; index < count; ++index) {
            const HirExpr *element = arr->repeatedElement ? arr->repeatedElement.get() : arr->elements[index].get();
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
    currentFunction = nameOverride.empty() ? hf.name : std::string(nameOverride);
    locals.clear();
    localConsts.clear();
    enumPayloadSlots.clear();
    partialCleanupFrames.clear();
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
        for (const auto &param : hf.params) {
            lf.params.push_back({LirNoReg, param.type, param.name});
        }
        return lf;
    }
    dropFlags.clear();
    CheckedLirBuilder functionBuilder(lf);
    builder = &functionBuilder;
    SetBlock(NewBlock("entry"));
    for (const auto &param : hf.params) {
        const std::string &name = param.name;
        const TypeRef &type = param.type;
        const LirReg pr = builder->DefineParameter();
        if (param.isVariadic) {
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
        else if (IsViewType(type)) {
            // Slice and string values are 16-byte {data, length} structs;
            // callers pass a pointer. pr holds that pointer directly — FieldPtr
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
    // A parameter taken by value arrives owning what it holds, so its flag starts set and the caller's copy of that
    // ownership is already gone.
    for (const auto &param : hf.params) {
        MarkBindingLive(param.bindingId, true);
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
HirToLirLowering::HirToLirLowering(HirPackage inputHir, TargetContext inputTarget)
    : hir(std::move(inputHir))
    , target(inputTarget) {
}

LirPackage HirToLirLowering::Generate() {
    diagnostics.clear();
    HirToLirDetail::HirToLirContext lowering(target, diagnostics);
    return lowering.Run(hir);
}

const std::vector<Diagnostic> &HirToLirLowering::Diagnostics() const noexcept {
    return diagnostics;
}
} // namespace Rux
