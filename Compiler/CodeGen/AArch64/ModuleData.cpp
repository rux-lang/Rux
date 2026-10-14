#include "CodeGen/AArch64/ModuleEmitter.h"

namespace Rux::AArch64Detail {

// Every function this module defines gets its symbol before any body is
// emitted, so a call can name a function declared further down the file and
// a body can be placed at whatever offset it turns out to start at. An
// extern declaration is predeclared too, for the same reason and one more:
// the library it names belongs to its symbol, and a call site reached before
// the declaration would otherwise create that symbol without it.
void AArch64ModuleEmitter::PredeclareFunctions() {
    for (const auto &func : mod.funcs) {
        if (func.isExtern) {
            GetOrAddExtern(func.name, RcuSymKind::ExternFunc, func.dll);
            continue;
        }
        if (funcSyms.contains(func.name)) {
            continue;
        }
        funcSyms[func.name] = DeclareSymbol(func.name, func.returnType.ToString(), RcuSymKind::Func,
                                            RcuSymVis::ForDefinition(func.isPublic, func.isExported));
    }
}

// `bytes` is already encoded at its element width; the terminator is one
// more element of zeroes, which AlignRodata's zero fill cannot be relied on
// to supply.
std::uint32_t AArch64ModuleEmitter::InternStringLiteral(const std::string &bytes) {
    if (const auto symbol = moduleBuilder.InternedLiteral("string", bytes)) {
        return *symbol;
    }
    auto &data = RodataData();
    const auto offset = static_cast<std::uint32_t>(data.size());
    for (const unsigned char byte : bytes) {
        data.push_back(byte);
    }
    data.push_back(0);
    const std::uint32_t idx = AddRodataConst(std::format("__str{}", constIdx++), offset);
    (void)moduleBuilder.RecordInternedLiteral("string", bytes, idx);
    return idx;
}

std::uint32_t AArch64ModuleEmitter::InternF32(const std::string &literal) {
    if (const auto symbol = moduleBuilder.InternedLiteral("f32", literal)) {
        return *symbol;
    }
    const std::uint32_t offset = AlignRodata(4);
    const float value = ParseFloatLiteral<float>(literal);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    for (int i = 0; i < 4; ++i) {
        RodataData().push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
    }
    const std::uint32_t idx = AddRodataConst(std::format("__f32_{}", constIdx++), offset);
    (void)moduleBuilder.RecordInternedLiteral("f32", literal, idx);
    return idx;
}

std::uint32_t AArch64ModuleEmitter::InternF64(const std::string &literal) {
    if (const auto symbol = moduleBuilder.InternedLiteral("f64", literal)) {
        return *symbol;
    }
    const std::uint32_t offset = AlignRodata(8);
    const double value = ParseFloatLiteral<double>(literal);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, 8);
    for (int i = 0; i < 8; ++i) {
        RodataData().push_back(static_cast<std::uint8_t>(bits >> (8 * i) & 0xFFU));
    }
    const std::uint32_t idx = AddRodataConst(std::format("__f64_{}", constIdx++), offset);
    (void)moduleBuilder.RecordInternedLiteral("f64", literal, idx);
    return idx;
}

// Module-level data
//
// A vtable is a run of function pointers, each of which is a whole address
// rather than a field of an instruction, so these are the one place this
// back end emits the architecture-neutral Abs64 the x86-64 one uses
// everywhere.
void AArch64ModuleEmitter::EmitVtables() {
    for (const auto &vt : mod.vtables) {
        const std::uint32_t vtableOff = AlignRodata(8);
        const std::uint32_t vtableSym = DeclareSymbol(vt.label, {}, RcuSymKind::Const, RcuSymVis::Global);
        dataSyms[vt.label] = vtableSym;

        for (const auto &method : vt.methods) {
            const auto slotOff = static_cast<std::uint32_t>(RodataData().size());
            RodataData().insert(RodataData().end(), 8, 0);
            const auto it = funcSyms.find(method);
            AddRodataReloc(slotOff, it != funcSyms.end() ? it->second : GetOrAddExtern(method, RcuSymKind::ExternFunc),
                           RcuRelType::Abs64);
        }
        (void)moduleBuilder.DefineSymbol(vtableSym, RcuModuleSection::RoData, vtableOff,
                                         static_cast<std::uint32_t>(vt.methods.size() * 8));
    }
}

// A slice constant becomes two read-only symbols: its elements, and a
// {data, length} header under the constant's own name whose data field is
// relocated to point at them. Code then reaches the elements the same way
// it reaches those of any other slice.
void AArch64ModuleEmitter::EmitConstSlice(const LirConstDecl &c) {
    const int elemSize = std::max(SizeOf(c.elementType), 1);
    const std::uint32_t elemsOff = AlignRodata(AlignOf(c.elementType));
    std::uint64_t length = 0;
    if (c.isTextSlice) {
        // Text is emitted in the constant's own encoding, and its length counts that encoding's code units.
        // Writing the value's UTF-8 bytes under a wider element type would publish a different text, at a length
        // that does not describe it.
        for (const unsigned char byte : EncodeStringLiteral(c.text, elemSize)) {
            RodataData().push_back(byte);
        }
        RodataData().push_back(0); // completes the terminator EncodeStringLiteral left room for
        length = CodeUnitCount(c.text, elemSize).value_or(0);
    }
    else {
        for (const auto &element : c.elements) {
            AppendConstElement(element, c.elementType);
        }
        length = c.elements.size();
    }
    const std::uint32_t elemsSym = AddRodataConst(c.name + "$elements", elemsOff);

    const std::uint32_t headerOff = AlignRodata(8);
    RodataData().insert(RodataData().end(), 16, 0);
    AddRodataReloc(headerOff, elemsSym, RcuRelType::Abs64);
    for (int i = 0; i < 8; ++i) {
        RodataData()[headerOff + 8 + i] = static_cast<std::uint8_t>(length >> (8 * i) & 0xFFU);
    }

    dataSyms[c.name] =
        DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                         RcuSymVis::ForDefinition(c.isPublic, c.isExported), RcuModuleSection::RoData, headerOff, 16);
}

void AArch64ModuleEmitter::GenModule() {
    PredeclareFunctions();
    for (const auto &ev : mod.externVars) {
        GetOrAddExtern(ev.name, RcuSymKind::ExternData);
    }
    for (const auto &c : mod.consts) {
        // A constant of sequence type is addressed rather than inlined, so
        // it needs its contents behind its name and not a placeholder.
        if (!c.hasSequenceData) {
            EmitConstValue(c);
        }
        else if (c.type.kind == TypeRef::Kind::Array) {
            EmitConstArray(c);
        }
        else {
            EmitConstSlice(c);
        }
    }
    for (const auto &global : mod.globals) {
        EmitGlobal(global);
    }
    EmitVtables();
    PredeclareFunctionTables();
    for (const auto &func : mod.funcs) {
        const auto start = static_cast<std::uint32_t>(enc.Size());
        GenFunc(func);
        const auto end = static_cast<std::uint32_t>(enc.Size());
        if (backtrace.enabled && !func.isExtern && end > start) {
            if (const auto symbol = funcSyms.find(func.name); symbol != funcSyms.end()) {
                functionRanges.push_back(
                    {symbol->second, end - start, func.displayName.empty() ? func.name : func.displayName});
            }
        }
    }
    EmitFunctionTables();
}

void AArch64ModuleEmitter::PredeclareFunctionTables() {
    if (!backtrace.enabled) {
        return;
    }
    dataSyms[backtrace.table] = DeclareSymbol(backtrace.table, {}, RcuSymKind::Const, RcuSymVis::Global);
    if (backtrace.IsProgram()) {
        const std::string tables(kFunctionTablesSymbol);
        dataSyms[tables] = DeclareSymbol(tables, {}, RcuSymKind::Const, RcuSymVis::Global);
    }
}

// Each entry is the function's address, its length in bytes, and its name as an address and a length; a count leads.
void AArch64ModuleEmitter::EmitFunctionTables() {
    if (!backtrace.enabled) {
        return;
    }
    const auto pushWord = [&](const std::uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            RodataData().push_back(static_cast<std::uint8_t>(value >> (index * 8)));
        }
    };
    std::vector<std::uint32_t> names;
    names.reserve(functionRanges.size());
    for (const FunctionRange &range : functionRanges) {
        names.push_back(InternStringLiteral(range.name));
    }
    const std::uint32_t tableOff = AlignRodata(8);
    pushWord(functionRanges.size());
    for (std::size_t index = 0; index < functionRanges.size(); ++index) {
        const auto slot = static_cast<std::uint32_t>(RodataData().size());
        pushWord(0);
        AddRodataReloc(slot, functionRanges[index].symbol, RcuRelType::Abs64);
        pushWord(functionRanges[index].size);
        const auto nameSlot = static_cast<std::uint32_t>(RodataData().size());
        pushWord(0);
        AddRodataReloc(nameSlot, names[index], RcuRelType::Abs64);
        pushWord(functionRanges[index].name.size());
    }
    (void)moduleBuilder.DefineSymbol(dataSyms.at(backtrace.table), RcuModuleSection::RoData,
                                     tableOff, static_cast<std::uint32_t>(RodataData().size()) - tableOff);
    if (!backtrace.IsProgram()) {
        return;
    }
    const std::uint32_t tablesOff = AlignRodata(8);
    pushWord(backtrace.programTables.size());
    for (const std::string &name : backtrace.programTables) {
        const auto slot = static_cast<std::uint32_t>(RodataData().size());
        pushWord(0);
        const auto known = dataSyms.find(name);
        AddRodataReloc(slot, known != dataSyms.end() ? known->second : GetOrAddExtern(name, RcuSymKind::ExternData),
                       RcuRelType::Abs64);
    }
    (void)moduleBuilder.DefineSymbol(dataSyms.at(std::string(kFunctionTablesSymbol)), RcuModuleSection::RoData,
                                     tablesOff, static_cast<std::uint32_t>(RodataData().size()) - tablesOff);
}
} // namespace Rux::AArch64Detail
