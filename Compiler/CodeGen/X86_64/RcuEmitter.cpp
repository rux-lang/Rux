// RCU code generation: lowers a LirModule to an in-memory RcuFile.

#include "CodeGen/X86_64/RcuEmitter.h"

#include "CodeGen/FloatLiteral.h"
#include "CodeGen/IntegerLiteral.h"
#include "CodeGen/Layout.h"
#include "CodeGen/RcuModuleBuilder.h"
#include "CodeGen/X86_64/Assembler.h"
#include "CodeGen/X86_64/CallAndTerminatorEmitter.h"
#include "CodeGen/X86_64/Encoder.h"
#include "CodeGen/X86_64/FramePlan.h"
#include "CodeGen/X86_64/FunctionEmitter.h"
#include "CodeGen/X86_64/RuntimeHelpers.h"
#include "Object/Rcu/RcuMetadata.h"

#include <array>
#include <cstring>
#include <format>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Rux {
using namespace Layout;

namespace {
class RcuCodeGen final : private X86_64FunctionEmitterHooks, private X86_64CallAndTerminatorHooks {
public:
    explicit RcuCodeGen(const LirModule &module, const std::vector<LirStructDecl> &inputStructDecls,
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
        , enc(moduleBuilder.SectionData(RcuModuleSection::Text))
        , runtimeHelpers(moduleBuilder, PlatformDefaultConvention(inputTargetOs, Target::Arch::X86_64)) {
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

    X86_64RuntimeHelperEmitter runtimeHelpers;

    int constIdx = 0;

    // Declared extern symbols (by name → symbol index)
    std::unordered_map<std::string, uint32_t> externSyms;
    std::unordered_map<std::string, uint32_t> funcSyms;
    std::unordered_map<std::string, uint32_t> dataSyms;

    // Struct field layouts
    LayoutMap layouts;
    std::unordered_set<std::string> interfaceNames;

    const X86_64FramePlan *activeFramePlan = nullptr;
    X86_64CallEmitter *activeCallEmitter = nullptr;

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

    [[nodiscard]] bool IsAggregate(const TypeRef &t) const override {
        if (t.IsRange()) {
            return true;
        }
        switch (t.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(t.name);
            return base == "Slice" || interfaceNames.count(base) > 0 || layouts.contains(base) ||
                   (!t.inner.empty() && SizeOf(t) > 8);
        }
        default:
            return false;
        }
    }

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
        if (t.kind != TypeRef::Kind::Named) {
            return false;
        }
        const std::string base = BaseTypeName(t.name);
        return base == "Slice" || interfaceNames.count(base) > 0;
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

    uint32_t InternStr(const std::string &val) {
        if (const auto symbol = moduleBuilder.InternedLiteral("string", val)) {
            return *symbol;
        }
        auto &data = RodataData();
        const auto off = static_cast<uint32_t>(data.size());
        for (unsigned char c : val) {
            data.push_back(c);
        }
        data.push_back(0);
        std::string lbl = std::format("__str{}", constIdx++);
        const uint32_t idx = DefineDataSymbol(std::move(lbl), {}, RcuSymKind::Const, RcuSymVis::Local,
                                              RcuModuleSection::RoData, off, static_cast<uint32_t>(val.size() + 1));
        (void)moduleBuilder.RecordInternedLiteral("string", val, idx);
        return idx;
    }

    uint32_t InternF32(const std::string &val) {
        if (const auto symbol = moduleBuilder.InternedLiteral("f32", val)) {
            return *symbol;
        }
        const uint32_t off = AlignRodata(4);
        const float fv = ParseFloatLiteral<float>(val);
        uint32_t bits;
        std::memcpy(&bits, &fv, 4);
        for (int i = 0; i < 4; ++i) {
            RodataData().push_back(bits & 0xFF);
            bits >>= 8;
        }
        std::string lbl = std::format("__f32_{}", constIdx++);
        const uint32_t idx =
            DefineDataSymbol(std::move(lbl), {}, RcuSymKind::Const, RcuSymVis::Local, RcuModuleSection::RoData, off, 4);
        (void)moduleBuilder.RecordInternedLiteral("f32", val, idx);
        return idx;
    }

    uint32_t InternF64(const std::string &val) {
        if (const auto symbol = moduleBuilder.InternedLiteral("f64", val)) {
            return *symbol;
        }
        const uint32_t off = AlignRodata(8);
        const double dv = ParseFloatLiteral<double>(val);
        uint64_t bits;
        std::memcpy(&bits, &dv, 8);
        for (int i = 0; i < 8; ++i) {
            RodataData().push_back(bits & 0xFF);
            bits >>= 8;
        }
        std::string lbl = std::format("__f64_{}", constIdx++);
        const uint32_t idx =
            DefineDataSymbol(std::move(lbl), {}, RcuSymKind::Const, RcuSymVis::Local, RcuModuleSection::RoData, off, 8);
        (void)moduleBuilder.RecordInternedLiteral("f64", val, idx);
        return idx;
    }

    uint32_t InternF32SignMask() {
        if (const auto symbol = moduleBuilder.InternedLiteral("f32", "sign-mask")) {
            return *symbol;
        }
        const uint32_t off = AlignRodata(4);
        // 0x80000000 — sign bit of f32
        constexpr std::array mask = {std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0x80}};
        (void)moduleBuilder.Append(RcuModuleSection::RoData, mask);
        const uint32_t symbol = DefineDataSymbol("__f32_sign_mask", {}, RcuSymKind::Const, RcuSymVis::Local,
                                                 RcuModuleSection::RoData, off, mask.size());
        (void)moduleBuilder.RecordInternedLiteral("f32", "sign-mask", symbol);
        return symbol;
    }

    uint32_t InternF64SignMask() {
        if (const auto symbol = moduleBuilder.InternedLiteral("f64", "sign-mask")) {
            return *symbol;
        }
        const uint32_t off = AlignRodata(8);
        // 0x8000000000000000 — sign bit of f64
        auto &data = RodataData();
        for (int i = 0; i < 7; ++i) {
            data.push_back(0x00);
        }
        data.push_back(0x80);
        const uint32_t symbol = DefineDataSymbol("__f64_sign_mask", {}, RcuSymKind::Const, RcuSymVis::Local,
                                                 RcuModuleSection::RoData, off, 8);
        (void)moduleBuilder.RecordInternedLiteral("f64", "sign-mask", symbol);
        return symbol;
    }

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
    void LoadA(const LirReg reg, const TypeRef &t) const override {
        const auto &physicalRegisters = FramePlan().PhysicalRegisters();
        auto it = physicalRegisters.find(reg);
        if (it != physicalRegisters.end()) {
            enc.MovRaxPhysReg(it->second);
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        enc.MovsxdRaxEax();
                    else if (sz == 2)
                        enc.MovsxRaxAx();
                    else
                        enc.MovsxRaxAl();
                }
                else {
                    if (sz == 4)
                        enc.MovEaxEax();
                    else if (sz == 2)
                        enc.MovzxRaxAx();
                    else
                        enc.MovzxRaxAl();
                }
            }
            return;
        }
        const int sz = SizeOfRuntime(t);
        const int runtimeSz = SizeOfRuntime(t);
        const int32_t d = Disp(reg);
        if (runtimeSz == 16) {
            enc.MovRaxLoad(d);
            enc.MovR10Load(d + 8);
            enc.Byte(0x4C);
            enc.Byte(0x89);
            enc.Byte(0xD2); // mov rdx, r10
        }
        else if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm0Load(d);
            }
            else {
                enc.MovsdXmm0Load(d);
            }
        }
        else if (sz == 8 || sz == 0) {
            enc.MovRaxLoad(d);
        }
        else if (t.IsSigned()) {
            if (sz == 4) {
                enc.MovsxdRaxDword(d);
            }
            else if (sz == 2) {
                enc.MovsxRaxWord(d);
            }
            else {
                enc.MovsxRaxByte(d);
            }
        }
        else {
            if (sz == 4) {
                enc.MovEaxLoad(d);
            }
            else if (sz == 2) {
                enc.MovzxRaxWord(d);
            }
            else {
                enc.MovzxRaxByte(d);
            }
        }
    }

    void LoadB(LirReg reg, const TypeRef &t) const override {
        const auto &physicalRegisters = FramePlan().PhysicalRegisters();
        auto it = physicalRegisters.find(reg);
        if (it != physicalRegisters.end()) {
            enc.MovR10PhysReg(it->second);
            int sz = SizeOfRuntime(t);
            if (sz > 0 && sz < 8) {
                if (t.IsSigned()) {
                    if (sz == 4)
                        enc.MovsxdR10r10d();
                    else if (sz == 2)
                        enc.MovsxR10r10w();
                    else
                        enc.MovsxR10r10b();
                }
                else {
                    if (sz == 4)
                        enc.MovR10dR10d();
                    else if (sz == 2)
                        enc.MovzxR10r10w();
                    else
                        enc.MovzxR10r10b();
                }
            }
            return;
        }
        int sz = SizeOfRuntime(t);
        int32_t d = Disp(reg);
        if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm1Load(d);
            }
            else {
                enc.MovsdXmm1Load(d);
            }
        }
        else if (sz == 8 || sz == 0) {
            enc.MovR10Load(d);
        }
        else if (t.IsSigned()) {
            if (sz == 4) {
                enc.MovsxdR10Dword(d);
            }
            else if (sz == 2) {
                enc.MovsxR10Word(d);
            }
            else {
                enc.MovsxR10Byte(d);
            }
        }
        else {
            if (sz == 4) {
                enc.MovR10dLoad(d);
            }
            else if (sz == 2) {
                enc.MovzxR10Word(d);
            }
            else {
                enc.MovzxR10Byte(d);
            }
        }
    }

    void StoreStack(LirReg dst, const TypeRef &t) const {
        int sz = SizeOfRuntime(t);
        int runtimeSz = SizeOfRuntime(t);
        int32_t d = Disp(dst);
        if (runtimeSz == 16) {
            enc.MovRaxStore(d);
            enc.Byte(0x48);
            enc.Byte(0x89);
            enc.Byte(0x95);
            enc.Dword(static_cast<uint32_t>(d + 8)); // mov [rbp+disp+8], rdx
        }
        else if (IsFloat(t)) {
            if (t.kind == TypeRef::Kind::Float32) {
                enc.MovssXmm0Store(d);
            }
            else {
                enc.MovsdXmm0Store(d);
            }
        }
        else {
            int ss = (sz > 0) ? sz : 8;
            if (ss == 8) {
                enc.MovRaxStore(d);
            }
            else if (ss == 4) {
                enc.MovEaxStore(d);
            }
            else if (ss == 2) {
                enc.MovAxStore(d);
            }
            else {
                enc.MovAlStore(d);
            }
        }
    }

    void StoreA(LirReg dst, const TypeRef &t) const override {
        const auto &physicalRegisters = FramePlan().PhysicalRegisters();
        auto it = physicalRegisters.find(dst);
        if (it != physicalRegisters.end()) {
            enc.MovPhysRegRax(it->second);
            return;
        }
        StoreStack(dst, t);
    }

    void LoadChunkFromR10(const int32_t offset, const int size) const {
        if (size == 8) {
            enc.Byte(0x49);
            enc.Byte(0x8B);
            enc.Byte(0x82); // mov rax, [r10 + disp32]
        }
        else if (size == 4) {
            enc.Byte(0x41);
            enc.Byte(0x8B);
            enc.Byte(0x82); // mov eax, [r10 + disp32]
        }
        else if (size == 2) {
            enc.Byte(0x41);
            enc.Byte(0x0F);
            enc.Byte(0xB7);
            enc.Byte(0x82); // movzx eax, word [r10 + disp32]
        }
        else {
            enc.Byte(0x41);
            enc.Byte(0x0F);
            enc.Byte(0xB6);
            enc.Byte(0x82); // movzx eax, byte [r10 + disp32]
        }
        enc.Dword(static_cast<uint32_t>(offset));
    }

    void StoreChunkToR11(const int32_t offset, const int size) const {
        if (size == 8) {
            enc.Byte(0x49);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], rax
        }
        else if (size == 4) {
            enc.Byte(0x41);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], eax
        }
        else if (size == 2) {
            enc.Byte(0x66);
            enc.Byte(0x41);
            enc.Byte(0x89);
            enc.Byte(0x83); // mov [r11 + disp32], ax
        }
        else {
            enc.Byte(0x41);
            enc.Byte(0x88);
            enc.Byte(0x83); // mov [r11 + disp32], al
        }
        enc.Dword(static_cast<uint32_t>(offset));
    }

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

    void CopyAggregateFromR10ToStack(const int32_t dstDisp, const int size) const {
        CopyAggregateFromR10(size, [&](const int32_t offset, const int chunkSize) {
            if (chunkSize == 8) {
                enc.MovRaxStore(dstDisp + offset);
            }
            else if (chunkSize == 4) {
                enc.MovEaxStore(dstDisp + offset);
            }
            else if (chunkSize == 2) {
                enc.MovAxStore(dstDisp + offset);
            }
            else {
                enc.MovAlStore(dstDisp + offset);
            }
        });
    }

    void StoreHiddenReturnValue(const LirReg src, const TypeRef &t) const override {
        enc.MovR11Load(-FramePlan().HiddenReturnOffset());
        if (IsRegPointerTo(src, t)) {
            const auto &physicalRegisters = FramePlan().PhysicalRegisters();
            auto it = physicalRegisters.find(src);
            if (it != physicalRegisters.end()) {
                enc.MovR10PhysReg(it->second);
            }
            else {
                enc.MovR10Load(Disp(src));
            }
        }
        else {
            enc.Byte(0x4C);
            enc.Byte(0x8D);
            enc.Byte(0x95);
            enc.Dword(static_cast<uint32_t>(Disp(src))); // lea r10, [rbp + disp32]
        }
        CopyAggregateFromR10(SizeOfRuntime(t),
                             [&](const int32_t offset, const int size) { StoreChunkToR11(offset, size); });
        enc.Byte(0x4C);
        enc.Byte(0x89);
        enc.Byte(0xD8); // mov rax, r11
    }

    // Build struct layouts
    void BuildLayouts() {
        for (const auto &name : packageInterfaceNames) {
            interfaceNames.insert(name);
        }
        for (const auto &s : structDecls) {
            layouts[s.name] = ComputeStructLayout(s, layouts);
        }
    }

    void EmitStackAlloc(int32_t bytes) const {
        constexpr int32_t kPageSize = 4096;
        while (bytes > kPageSize) {
            enc.SubRspImm32(kPageSize);
            enc.TouchRsp();
            bytes -= kPageSize;
        }
        if (bytes > 0) {
            enc.SubRspImm32(bytes);
            if (bytes == kPageSize) {
                enc.TouchRsp();
            }
        }
    }

    // Instruction code generation
    void GenInstr(X86_64FunctionEmitter &functionEmitter, X86_64CallEmitter &callEmitter, const LirInstr &instr) {
        if (functionEmitter.EmitArithmetic(instr) || functionEmitter.EmitMemory(instr) || callEmitter.Emit(instr)) {
            return;
        }
        switch (instr.op) {
        case LirOpcode::Assert:
        case LirOpcode::Panic: {
            const bool isAssertion = instr.op == LirOpcode::Assert;
            if (instr.srcs.size() < (isAssertion ? 2 : 1)) {
                break;
            }
            uint32_t okPatch = 0;
            if (isAssertion) {
                LoadA(instr.srcs[0], TypeRef::MakeBool());
                enc.TestRaxRax();
                enc.Jnz(okPatch);
            }

            const LirReg messageReg = instr.srcs[isAssertion ? 1 : 0];
            const std::string prefix = isAssertion ? "Assertion failed: " : "Panic: ";
            const std::string function = instr.sourceFunction.empty() ? "<unknown>" : instr.sourceFunction;
            const std::string file = instr.sourceFile.empty() ? "<unknown>" : instr.sourceFile;
            const std::string suffix =
                std::format("\n  at {} ({}:{}:{})\n", function, file, instr.sourceLine, instr.sourceColumn);

            if (targetOs == Target::OS::Windows) {
                const uint32_t getStdHandle = GetOrAddExtern("GetStdHandle", RcuSymKind::ExternFunc, "KERNEL32.DLL");
                const uint32_t writeFile = GetOrAddExtern("WriteFile", RcuSymKind::ExternFunc, "KERNEL32.DLL");

                // Shadow space, the fifth WriteFile argument, and a DWORD for
                // lpNumberOfBytesWritten. The failure path never returns.
                enc.SubRspImm32(48);
                enc.MovQwordRspImm32(32, 0);

                const auto prepareWrite = [&]() {
                    enc.MovEaxImm32(-12); // STD_ERROR_HANDLE
                    enc.MovArgWin64Rax(0);
                    uint32_t getHandleReloc;
                    enc.Call(getHandleReloc);
                    AddTextReloc(getHandleReloc, getStdHandle);
                    enc.MovArgWin64Rax(0);
                    enc.LeaR9Rsp(40);
                };
                const auto writeStatic = [&](const std::string &text) {
                    prepareWrite();
                    const uint32_t textSymbol = InternStr(text);
                    uint32_t textReloc;
                    enc.LeaRaxRip(textReloc);
                    AddTextReloc(textReloc, textSymbol);
                    enc.MovArgWin64Rax(1);
                    enc.MovEaxImm32(static_cast<int32_t>(text.size()));
                    enc.MovArgWin64Rax(2);
                    uint32_t writeReloc;
                    enc.Call(writeReloc);
                    AddTextReloc(writeReloc, writeFile);
                };

                writeStatic(prefix);

                prepareWrite();
                LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                enc.MovR10Rax();
                enc.MovRdxR10Load();
                enc.MovR8R10Load(8);
                uint32_t messageWriteReloc;
                enc.Call(messageWriteReloc);
                AddTextReloc(messageWriteReloc, writeFile);

                writeStatic(suffix);
            }
            else {
                const int syscallNumber = targetOs == Target::OS::Linux ? 1
                                        : targetOs == Target::OS::MacOS ? 0x0200'0004
                                                                        : 4;
                const auto writeStatic = [&](const std::string &text) {
                    const uint32_t textSymbol = InternStr(text);
                    uint32_t textReloc;
                    enc.LeaRaxRip(textReloc);
                    AddTextReloc(textReloc, textSymbol);
                    enc.MovRsiRax();
                    enc.MovEdxImm32(static_cast<int32_t>(text.size()));
                    enc.MovEdiImm32(2);
                    enc.MovEaxImm32(syscallNumber);
                    enc.Syscall();
                };

                writeStatic(prefix);
                LoadA(messageReg, TypeRef::MakePointer(TypeRef::MakeNamed("Slice<char8>")));
                enc.MovR10Rax();
                enc.MovRsiR10Load();
                enc.MovRdxR10Load(8);
                enc.MovEdiImm32(2);
                enc.MovEaxImm32(syscallNumber);
                enc.Syscall();
                writeStatic(suffix);
            }

            enc.Ud2();
            if (isAssertion) {
                const auto here = static_cast<int32_t>(enc.Size());
                enc.Patch32(okPatch, here - static_cast<int32_t>(okPatch + 4));
            }
            break;
        }
        default:
            break;
        }
    }

    // Resolve a symbol referenced from inline assembly to its symbol-table
    // index: a local text symbol, module data/const, an already-declared
    // extern, or a newly declared extern function.
    uint32_t ResolveAsmSymbol(const std::string &name) {
        if (const auto it = funcSyms.find(name); it != funcSyms.end()) {
            return it->second;
        }
        if (const auto it = dataSyms.find(name); it != dataSyms.end()) {
            return it->second;
        }
        if (const auto it = externSyms.find(name); it != externSyms.end()) {
            return it->second;
        }
        return GetOrAddExtern(name, RcuSymKind::ExternFunc);
    }

    // An `asm func` is emitted as a raw blob: no prologue, epilogue or frame.
    // Its instructions are encoded directly and any symbol references become
    // ordinary text relocations.
    void GenAsmFunc(const LirFunc &func) {
        const uint32_t symIdx = funcSyms.contains(func.name)
                                  ? funcSyms.at(func.name)
                                  : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                  func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        funcSyms[func.name] = symIdx;
        if (!moduleBuilder.BeginFunction(symIdx)) {
            return;
        }

        AsmAssembly asmResult = AssembleAsmFunc(func.asmBody, mod.name, TextData());
        for (const auto &fixup : asmResult.fixups) {
            (void)moduleBuilder.AddRelocation(RcuModuleSection::Text, fixup.offset, ResolveAsmSymbol(fixup.symbol),
                                              fixup.relType, fixup.addend);
        }
        for (auto &diag : asmResult.diagnostics) {
            diagnostics.push_back(std::move(diag));
        }
        (void)moduleBuilder.EndFunction(symIdx);
    }

    // Function generation
    void GenFunc(const LirFunc &func) {
        if (func.isExtern) {
            GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
            return;
        }
        if (func.isAsm) {
            GenAsmFunc(func);
            return;
        }
        const X86_64FramePlan framePlan = PlanX86_64Frame(func, layouts, interfaceNames, targetOs);
        activeFramePlan = &framePlan;
        X86_64CallEmitter callEmitter(enc, framePlan, targetOs, *this);
        X86_64TerminatorEmitter terminatorEmitter(enc, framePlan, *this);
        activeCallEmitter = &callEmitter;
        X86_64FunctionEmitter functionEmitter(enc, framePlan, runtimeHelpers, EffectiveConv(CallingConvention::Default),
                                              layouts, interfaceNames, *this);
        const auto &usedPhysicalRegisters = framePlan.UsedPhysicalRegisters();
        const auto &physicalRegisters = framePlan.PhysicalRegisters();
        const uint32_t symIdx = funcSyms.contains(func.name)
                                  ? funcSyms.at(func.name)
                                  : DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                                  func.isPublic ? RcuSymVis::Global : RcuSymVis::Local);
        funcSyms[func.name] = symIdx;
        if (!moduleBuilder.BeginFunction(symIdx)) {
            activeCallEmitter = nullptr;
            activeFramePlan = nullptr;
            return;
        }
        // Prologue
        enc.PushRbp();
        enc.MovRbpRsp();
        for (int rIdx : usedPhysicalRegisters) {
            enc.PushReg(rIdx);
        }
        const int32_t remainingFrame = framePlan.FrameSize() - static_cast<int32_t>(usedPhysicalRegisters.size() * 8);
        EmitStackAlloc(remainingFrame);
        // Spill ABI param registers to stack slots
        bool win64Func = EffectiveConv(func.callConv) == CallingConvention::Win64;
        int intIdx = 0, fltIdx = 0, sysvStackIdx = 0, win64Idx = 0;
        if (framePlan.HiddenReturnOffset() != 0) {
            if (win64Func) {
                enc.MovArgStoreWin64(0, -framePlan.HiddenReturnOffset());
                win64Idx = 1;
            }
            else {
                enc.MovArgStore(0, -framePlan.HiddenReturnOffset());
                intIdx = 1;
            }
        }
        for (const auto &p : func.params) {
            int sz = SizeOf(p.type);
            int32_t d = Disp(p.reg);
            if (win64Func) {
                // Win64: first 4 args are registers; the rest start
                // above return address + saved rbp + 32-byte home
                // space.
                if (win64Idx >= 4) {
                    const int32_t stackArgOff = 48 + (win64Idx - 4) * 8;
                    if (IsWin64AddressParam(p.type)) {
                        enc.MovRaxLoad(stackArgOff);
                        enc.MovRaxStore(d);
                    }
                    else if (IsWin64ByRefAggregate(p.type)) {
                        enc.MovR10Load(stackArgOff);
                        CopyAggregateFromR10ToStack(d, SizeOfRuntime(p.type));
                    }
                    else if (IsFloat(p.type)) {
                        if (sz == 4) {
                            enc.MovssXmm0Load(stackArgOff);
                            enc.MovssXmm0Store(d);
                        }
                        else {
                            enc.MovsdXmm0Load(stackArgOff);
                            enc.MovsdXmm0Store(d);
                        }
                    }
                    else {
                        enc.MovRaxLoad(stackArgOff);
                        StoreStack(p.reg, p.type);
                    }
                    ++win64Idx;
                    continue;
                }
                if (IsWin64AddressParam(p.type)) {
                    enc.MovRaxArgWin64(win64Idx);
                    enc.MovRaxStore(d);
                }
                else if (IsWin64ByRefAggregate(p.type)) {
                    enc.MovR10ArgWin64(win64Idx);
                    CopyAggregateFromR10ToStack(d, SizeOfRuntime(p.type));
                }
                else if (IsFloat(p.type)) {
                    // MOVSS/MOVSD [rbp+d], xmmN
                    enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                    enc.Byte(0x0F);
                    enc.Byte(0x11);
                    enc.Byte(static_cast<uint8_t>(0x80 | (win64Idx << 3) | 5));
                    enc.Dword(static_cast<uint32_t>(d));
                }
                else {
                    enc.MovRaxArgWin64(win64Idx);
                    StoreStack(p.reg, p.type);
                }
                ++win64Idx;
            }
            else {
                if (IsSysVMemoryAggregate(p.type)) {
                    const int size = AlignUp(SizeOfRuntime(p.type), 8);
                    for (int offset = 0; offset < size; offset += 8) {
                        enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                        enc.MovRaxStore(d + offset);
                    }
                }
                else if (IsFloat(p.type)) {
                    if (fltIdx < 8) {
                        // MOVSS/MOVSD [rbp+d], xmmN
                        enc.Byte(sz == 4 ? 0xF3 : 0xF2);
                        enc.Byte(0x0F);
                        enc.Byte(0x11);
                        enc.Byte(static_cast<uint8_t>(0x80 | (fltIdx << 3) | 5));
                        enc.Dword(static_cast<uint32_t>(d));
                        ++fltIdx;
                    }
                    else {
                        const int32_t stackArgOff = 16 + sysvStackIdx++ * 8;
                        if (sz == 4) {
                            enc.MovssXmm0Load(stackArgOff);
                            enc.MovssXmm0Store(d);
                        }
                        else {
                            enc.MovsdXmm0Load(stackArgOff);
                            enc.MovsdXmm0Store(d);
                        }
                    }
                }
                else if (IsAggregate(p.type) && sz == 16) {
                    if (intIdx <= 4) {
                        enc.MovArgStore(intIdx++, d);
                        enc.MovArgStore(intIdx++, d + 8);
                    }
                    else {
                        enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                        enc.MovRaxStore(d);
                        enc.MovRaxLoad(16 + sysvStackIdx++ * 8);
                        enc.MovRaxStore(d + 8);
                    }
                }
                else {
                    if (intIdx < 6) {
                        enc.MovArgStore(intIdx, d);
                        ++intIdx;
                    }
                    else {
                        const int32_t stackArgOff = 16 + sysvStackIdx++ * 8;
                        enc.MovRaxLoad(stackArgOff);
                        StoreStack(p.reg, p.type);
                    }
                }
            }
        }
        // Load params into their allocated physical registers
        for (const auto &p : func.params) {
            auto it = physicalRegisters.find(p.reg);
            if (it != physicalRegisters.end()) {
                int sz = IsWin64AddressParam(p.type) ? 8 : SizeOfRuntime(p.type);
                int32_t d = Disp(p.reg);
                if (sz == 8 || sz == 0) {
                    enc.MovRaxLoad(d);
                }
                else if (p.type.IsSigned()) {
                    if (sz == 4)
                        enc.MovsxdRaxDword(d);
                    else if (sz == 2)
                        enc.MovsxRaxWord(d);
                    else
                        enc.MovsxRaxByte(d);
                }
                else {
                    if (sz == 4)
                        enc.MovEaxLoad(d);
                    else if (sz == 2)
                        enc.MovzxRaxWord(d);
                    else
                        enc.MovzxRaxByte(d);
                }
                enc.MovPhysRegRax(it->second);
            }
        }
        // Basic blocks
        terminatorEmitter.Begin(func.blocks.size());
        for (uint32_t bi = 0; bi < func.blocks.size(); ++bi) {
            terminatorEmitter.MarkBlock(bi);
            const auto &block = func.blocks[bi];
            for (const auto &instr : block.instrs) {
                GenInstr(functionEmitter, callEmitter, instr);
            }
            if (block.term) {
                terminatorEmitter.Emit(bi, *block.term);
            }
        }
        terminatorEmitter.PatchJumps();
        (void)moduleBuilder.EndFunction(symIdx);
        activeCallEmitter = nullptr;
        activeFramePlan = nullptr;
    }

    // Module generation
    void EmitVtables() {
        for (const auto &vt : mod.vtables) {
            const uint32_t vtableOff = AlignRodata(8);
            const uint32_t vtSym = DeclareSymbol(vt.label, {}, RcuSymKind::Const, RcuSymVis::Global);
            dataSyms[vt.label] = vtSym;

            for (const auto &method : vt.methods) {
                const uint32_t slotOff = static_cast<uint32_t>(RodataData().size());
                for (int i = 0; i < 8; ++i) {
                    RodataData().push_back(0);
                }

                uint32_t methodSym;
                if (const auto it = funcSyms.find(method); it != funcSyms.end()) {
                    methodSym = it->second;
                }
                else {
                    methodSym = GetOrAddExtern(method, RcuSymKind::ExternFunc);
                }
                AddRodataReloc(slotOff, methodSym, RcuRelType::Abs64);
            }
            (void)moduleBuilder.DefineSymbol(vtSym, RcuModuleSection::RoData, vtableOff,
                                             static_cast<uint32_t>(vt.methods.size() * 8));
        }
    }

    // Appends one element of a constant array to .rodata, little-endian.
    void AppendConstElement(const std::string &literal, const TypeRef &type) {
        const int size = SizeOf(type);
        std::uint64_t bits = 0;
        if (type.kind == TypeRef::Kind::Float64) {
            const double value = ParseFloatLiteral<double>(literal);
            std::memcpy(&bits, &value, 8);
        }
        else if (type.kind == TypeRef::Kind::Float32) {
            const float value = ParseFloatLiteral<float>(literal);
            std::uint32_t narrow = 0;
            std::memcpy(&narrow, &value, 4);
            bits = narrow;
        }
        else if (type.IsBool()) {
            bits = (literal == "true" || literal == "1") ? 1 : 0;
        }
        else if (literal.starts_with('-')) {
            const std::uint64_t magnitude = ParseIntegerLiteralBits(literal.substr(1)).value_or(0);
            bits = static_cast<std::uint64_t>(-static_cast<std::int64_t>(magnitude));
        }
        else {
            bits = ParseIntegerLiteralBits(literal).value_or(0);
        }
        for (int i = 0; i < size; ++i) {
            RodataData().push_back(bits & 0xFF);
            bits >>= 8;
        }
    }

    // A slice constant becomes two read-only symbols: its elements, and a
    // {data, length} header under the constant's own name whose data field is
    // relocated to point at them. Code then reaches the elements the same way
    // it reaches those of any other slice.
    void EmitConstSlice(const LirConstDecl &c) {
        const int elemSize = std::max(SizeOf(c.elementType), 1);
        const uint32_t elemsOff = AlignRodata(std::min(elemSize, 8));
        std::uint64_t length = 0;
        if (c.isTextSlice) {
            for (const unsigned char byte : c.text) {
                RodataData().push_back(byte);
            }
            RodataData().push_back(0); // keep C interop's terminator
            length = c.text.size();
        }
        else {
            for (const auto &element : c.elements) {
                AppendConstElement(element, c.elementType);
            }
            length = c.elements.size();
        }

        const uint32_t elemsSym =
            DefineDataSymbol(c.name + "$elements", {}, RcuSymKind::Const, RcuSymVis::Local, RcuModuleSection::RoData,
                             elemsOff, static_cast<uint32_t>(RodataData().size() - elemsOff));

        const uint32_t headerOff = AlignRodata(8);
        for (int i = 0; i < 16; ++i) {
            RodataData().push_back(0);
        }
        AddRodataReloc(headerOff, elemsSym, RcuRelType::Abs64);
        for (int i = 0; i < 8; ++i) {
            RodataData()[headerOff + 8 + i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF);
        }

        dataSyms[c.name] = DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                                            c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::RoData,
                                            headerOff, 16);
    }

    void EmitConstArray(const LirConstDecl &c) {
        const int elemSize = std::max(SizeOf(c.elementType), 1);
        const uint32_t arrayOff = AlignRodata(AlignOf(c.elementType));
        for (const auto &element : c.elements) {
            AppendConstElement(element, c.elementType);
        }

        dataSyms[c.name] =
            DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                             c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::RoData, arrayOff,
                             static_cast<uint32_t>(c.elements.size() * static_cast<std::size_t>(elemSize)));
    }

    void GenModule() {
        BuildLayouts();
        PredeclareFunctions();
        // Extern vars
        for (const auto &ev : mod.externVars) {
            GetOrAddExtern(ev.name, RcuSymKind::ExternData);
        }
        // Module constants → .data symbols
        for (const auto &c : mod.consts) {
            // A constant of slice type is addressed, not inlined, so it needs
            // real contents behind its name rather than a placeholder.
            if (c.hasSequenceData) {
                if (c.type.kind == TypeRef::Kind::Array) {
                    EmitConstArray(c);
                }
                else {
                    EmitConstSlice(c);
                }
                continue;
            }
            const uint32_t offset = static_cast<uint32_t>(DataData().size());
            // Emit 8 placeholder bytes in .data
            for (int i = 0; i < 8; ++i) {
                DataData().push_back(0);
            }
            dataSyms[c.name] =
                DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                                 c.isPublic ? RcuSymVis::Global : RcuSymVis::Local, RcuModuleSection::Data, offset, 8);
        }
        EmitVtables();
        // Functions
        for (const auto &func : mod.funcs) {
            GenFunc(func);
        }
        runtimeHelpers.EmitRequested();
    }
};

// RcuCodeGen::Generate
RcuFile RcuCodeGen::Generate() {
    GenModule();
    auto built = moduleBuilder.Finalize();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(built.diagnostics.begin()),
                       std::make_move_iterator(built.diagnostics.end()));
    return built.file ? std::move(*built.file) : RcuFile{};
}
} // namespace

RcuFile GenerateRcuModule(const LirModule &mod, const std::vector<LirStructDecl> &structDecls,
                          const std::vector<std::string> &interfaceNames, const std::string &packageName,
                          const Target::OS targetOs, const BuildInfo &buildInfo, std::vector<Diagnostic> &diagnostics) {
    RcuCodeGen gen(mod, structDecls, interfaceNames, packageName, targetOs, buildInfo, diagnostics);
    return gen.Generate();
}

RcuEmitter::RcuEmitter(const LirPackage &package, std::string inputPackageName, const Target::OS inputTargetOs,
                       BuildInfo inputBuildInfo)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , targetOs(inputTargetOs)
    , buildInfo(std::move(inputBuildInfo)) {
}

std::vector<RcuFile> RcuEmitter::Generate() const {
    std::vector<RcuFile> result;
    result.reserve(lir.modules.size());
    std::vector<LirStructDecl> structDecls;
    std::vector<std::string> interfaceNames;
    for (const auto &module : lir.modules) {
        structDecls.insert(structDecls.end(), module.structs.begin(), module.structs.end());
        interfaceNames.insert(interfaceNames.end(), module.interfaceNames.begin(), module.interfaceNames.end());
    }
    for (const auto &module : lir.modules) {
        result.push_back(
            GenerateRcuModule(module, structDecls, interfaceNames, packageName, targetOs, buildInfo, diagnostics));
    }
    return result;
}
} // namespace Rux
