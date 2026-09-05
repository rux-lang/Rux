#pragma once

#include "CodeGen/BackendDiagnostics.h"
#include "CodeGen/ConstantData.h"
#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/RcuModuleBuilder.h"
#include "CodeGen/RuntimeFailure.h"
#include "CodeGen/X86_64/Assembler.h"
#include "CodeGen/X86_64/CallAndTerminatorEmitter.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/FunctionEmitter.h"
#include "CodeGen/X86_64/RcuEmitter.h"
#include "Object/Rcu/RcuMetadata.h"
#include "Unicode/Utf.h"

#include <array>
#include <cstring>
#include <format>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux::X86_64Detail {
using namespace Layout;

class X86_64ModuleEmitter final : private X86_64FunctionEmitterHooks, private X86_64CallAndTerminatorHooks {
public:
    explicit X86_64ModuleEmitter(const LirModule &module, const std::vector<LirStructDecl> &inputStructDecls,
                                 const std::vector<std::string> &inputPackageInterfaceNames, std::string packageName,
                                 const Target::OS inputTargetOs, const BuildInfo &inputBuildInfo,
                                 std::vector<Diagnostic> &inputDiagnostics)
        : mod(module)
        , structDecls(inputStructDecls)
        , packageInterfaceNames(inputPackageInterfaceNames)
        , pkgName(std::move(packageName))
        , targetOs(inputTargetOs)
        , buildInfo(inputBuildInfo)
        , diagnostics(inputDiagnostics)
        , moduleBuilder({.arch = RcuArch::X86_64,
                         .sourcePath = module.name,
                         .packageName = pkgName,
                         .buildTimestamp = RcuBuildTimestamp(buildInfo),
                         .ruxVersion = RcuCompilerVersion(buildInfo)})
        , enc(moduleBuilder.SectionData(RcuModuleSection::Text)) {
    }

    RcuFile Generate();

private:
    const LirModule &mod;
    const std::vector<LirStructDecl> &structDecls;
    const std::vector<std::string> &packageInterfaceNames;
    std::string pkgName;
    Target::OS targetOs;
    const BuildInfo &buildInfo;
    std::vector<Diagnostic> &diagnostics;

    // `Default` means the internal Rux ABI, `.C` the target's C ABI; both are
    // decided by the target, so the rules live in Target/CallingConvention.h
    // rather than being restated per back end.
    [[nodiscard]] CallingConvention EffectiveConv(const CallingConvention c) const {
        if (c == CallingConvention::Default) {
            return PlatformDefaultConvention(targetOs, Target::Arch::X86_64);
        }
        return ResolveCConvention(c, targetOs, Target::Arch::X86_64);
    }

    RcuModuleBuilder moduleBuilder;

    X64Enc enc;

    int constIdx = 0;

    // Declared extern symbols (by name → symbol index)
    std::unordered_map<std::string, uint32_t> externSyms;
    std::unordered_map<std::string, uint32_t> funcSyms;
    std::unordered_map<std::string, uint32_t> dataSyms;

    // Struct field layouts
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;
    std::unordered_set<std::string> reportedDiagnostics;
    std::string currentFunc;

    const X86_64FramePlan *activeFramePlan = nullptr;
    X86_64CallEmitter *activeCallEmitter = nullptr;

    void Report(Diagnostic diagnostic) {
        if (reportedDiagnostics.insert(diagnostic.message).second) {
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    [[nodiscard]] std::vector<std::uint8_t> &TextData() {
        return moduleBuilder.SectionData(RcuModuleSection::Text);
    }

    [[nodiscard]] std::vector<std::uint8_t> &RodataData() {
        return moduleBuilder.SectionData(RcuModuleSection::RoData);
    }

    [[nodiscard]] std::vector<std::uint8_t> &DataData() {
        return moduleBuilder.SectionData(RcuModuleSection::Data);
    }

    [[nodiscard]] uint32_t DeclareSymbol(std::string name, std::string typeName, const std::uint8_t kind,
                                         const std::uint8_t visibility) {
        return moduleBuilder
            .DeclareSymbol(
                {.name = std::move(name), .typeName = std::move(typeName), .kind = kind, .visibility = visibility})
            .value_or(~0u);
    }

    [[nodiscard]] uint32_t DefineDataSymbol(std::string name, std::string typeName, const std::uint8_t kind,
                                            const std::uint8_t visibility, const RcuModuleSection section,
                                            const std::uint32_t offset, const std::uint32_t size) {
        return moduleBuilder
            .AddDefinition(
                {.name = std::move(name), .typeName = std::move(typeName), .kind = kind, .visibility = visibility},
                section, offset, size)
            .value_or(~0u);
    }

    [[nodiscard]] const X86_64FramePlan &FramePlan() const {
        return *activeFramePlan;
    }

    [[nodiscard]] int32_t Disp(const LirReg r) const {
        return -FramePlan().SlotOffsets().at(r);
    }

    [[nodiscard]] int SizeOfRuntime(const TypeRef &t) const override {
        return RuntimeSizeOf(t, layouts, interfaceNames);
    }

    [[nodiscard]] bool IsAggregate(const TypeRef &t) const override;

    [[nodiscard]] bool IsWin64ByRefAggregate(const TypeRef &t) const {
        if (!IsAggregate(t)) {
            return false;
        }
        const int size = SizeOfRuntime(t);
        return size > 0 && size != 1 && size != 2 && size != 4 && size != 8;
    }

    [[nodiscard]] bool IsSysVMemoryAggregate(const TypeRef &t) const {
        return IsAggregate(t) && SizeOfRuntime(t) > 16;
    }

    [[nodiscard]] bool IsWin64AddressParam(const TypeRef &t) const {
        if (t.IsString()) {
            return true;
        }
        if (t.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(t.name);
        return t.isIntrinsicSlice || interfaceNames.count(base) > 0;
    }

    [[nodiscard]] bool IsRegPointerTo(const LirReg reg, const TypeRef &pointee) const override {
        const auto &registerTypes = FramePlan().RegisterTypes();
        const auto it = registerTypes.find(reg);
        return it != registerTypes.end() && it->second.kind == TypeRef::Kind::Pointer && !it->second.inner.empty() &&
               it->second.inner[0] == pointee;
    }

    uint32_t GetOrAddExtern(const std::string &name, uint8_t kind, const std::string &dll = {}) {
        auto it = externSyms.find(name);
        if (it != externSyms.end()) {
            return it->second;
        }
        const uint32_t idx = moduleBuilder.DeclareExternal(name, kind, dll).value_or(~0u);
        externSyms[name] = idx;
        return idx;
    }

    void PredeclareFunctions() {
        for (const auto &func : mod.funcs) {
            if (func.isExtern || funcSyms.contains(func.name)) {
                continue;
            }
            funcSyms[func.name] = DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        }
    }

    // Align read-only data to `align` bytes (zero-fill), return current offset.
    uint32_t AlignRodata(int align) {
        return moduleBuilder.AlignSection(RcuModuleSection::RoData, static_cast<std::uint16_t>(align));
    }

    uint32_t InternStr(const std::string &val);

    uint32_t InternF32(const std::string &val);

    uint32_t InternF64(const std::string &val);

    uint32_t InternF32SignMask();

    uint32_t InternF64SignMask();

    [[nodiscard]] std::uint32_t InternFloatSignMask(const bool float32) override {
        return float32 ? InternF32SignMask() : InternF64SignMask();
    }

    [[nodiscard]] std::uint32_t InternStringLiteral(const std::string &value) override {
        return InternStr(value);
    }

    [[nodiscard]] std::uint32_t InternFloat32Literal(const std::string &value) override {
        return InternF32(value);
    }

    [[nodiscard]] std::uint32_t InternFloat64Literal(const std::string &value) override {
        return InternF64(value);
    }

    [[nodiscard]] std::uint32_t ResolveNamedDataSymbol(const std::string &name) override {
        return GetOrAddExtern(name, RcuSymKind::ExternData);
    }

    [[nodiscard]] std::uint32_t ResolveGlobalSymbol(const std::string &name) override {
        if (const auto data = dataSyms.find(name); data != dataSyms.end()) {
            return data->second;
        }
        if (const auto function = funcSyms.find(name); function != funcSyms.end()) {
            return function->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternData);
    }

    void AddTextReloc(uint32_t sectionOff, uint32_t symIdx, int32_t addend = 0) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, sectionOff, symIdx, RcuRelType::Rel32, addend);
    }

    void AddTextRelocation(const std::uint32_t sectionOffset, const std::uint32_t symbol) override {
        AddTextReloc(sectionOffset, symbol);
    }

    void EmitCallArguments(const std::vector<LirReg> &arguments) override {
        activeCallEmitter->EmitArguments(arguments);
    }

    [[nodiscard]] std::uint32_t ResolveCallSymbol(const std::string &name) override {
        if (const auto function = funcSyms.find(name); function != funcSyms.end()) {
            return function->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternFunc);
    }

    void AddRodataReloc(uint32_t sectionOff, uint32_t symIdx, uint16_t type, int32_t addend = 0) {
        (void)moduleBuilder.AddRelocation(RcuModuleSection::RoData, sectionOff, symIdx, type, addend);
    }

    // Load A (rax / xmm0) and B (r10 / xmm1)
    void LoadA(const LirReg reg, const TypeRef &t) const override;

    void LoadB(LirReg reg, const TypeRef &t) const override;

    void StoreStack(LirReg dst, const TypeRef &t) const;

    void StoreA(LirReg dst, const TypeRef &t) const override {
        const auto &physicalRegisters = FramePlan().PhysicalRegisters();
        auto it = physicalRegisters.find(dst);
        if (it != physicalRegisters.end()) {
            enc.MovPhysRegRax(it->second);
            return;
        }
        StoreStack(dst, t);
    }

    void LoadChunkFromR10(const int32_t offset, const int size) const;

    void StoreChunkToR11(const int32_t offset, const int size) const;

    template <typename StoreChunk>
    void CopyAggregateFromR10(const int size, StoreChunk storeChunk) const {
        int32_t offset = 0;
        for (const int chunkSize : {8, 4, 2, 1}) {
            while (offset + chunkSize <= size) {
                LoadChunkFromR10(offset, chunkSize);
                storeChunk(offset, chunkSize);
                offset += chunkSize;
            }
        }
    }

    void CopyAggregateFromR10ToStack(const int32_t dstDisp, const int size) const;

    void StoreHiddenReturnValue(const LirReg src, const TypeRef &t) const override;

    // Build struct layouts
    void BuildLayouts() {
        for (const auto &name : packageInterfaceNames) {
            interfaceNames.insert(name);
        }
        BuildStructLayouts(structDecls, layouts, interfaceNames);
    }

    void EmitStackAlloc(int32_t bytes) const;

    // Instruction code generation
    void GenInstr(X86_64FunctionEmitter &functionEmitter, X86_64CallEmitter &callEmitter, const LirInstr &instr);

    // Resolve a symbol referenced from inline assembly to its symbol-table
    // index: a local text symbol, module data/const, an already-declared
    // extern, or a newly declared extern function.
    uint32_t ResolveAsmSymbol(const std::string &name);

    // An `asm func` is emitted as a raw blob: no prologue, epilogue or frame.
    // Its instructions are encoded directly and any symbol references become
    // ordinary text relocations.
    void GenAsmFunc(const LirFunc &func);

    // Function generation
    void GenFunc(const LirFunc &func);

    // Module generation
    void EmitVtables();

    void AppendConstElement(const std::string &literal, const TypeRef &type) {
        AppendScalarConstant(RodataData(), literal, type);
    }

    // A slice constant becomes two read-only symbols: its elements, and a
    // {data, length} header under the constant's own name whose data field is
    // relocated to point at them. Code then reaches the elements the same way
    // it reaches those of any other slice.
    void EmitConstSlice(const LirConstDecl &c);

    void EmitConstArray(const LirConstDecl &c);

    void GenModule();
};

} // namespace Rux::X86_64Detail
