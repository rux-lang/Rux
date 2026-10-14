#include "CodeGen/X86_64/ModuleEmitter.h"

namespace Rux::X86_64Detail {

[[nodiscard]] bool X86_64ModuleEmitter::IsAggregate(const TypeRef &t) const {
    if (IsWideValue(t)) {
        return true;
    }
    if (t.IsRange()) {
        return true;
    }
    // A slice, text included, is a 16-byte {data, length} view whose shape the runtime fixes rather than a declaration.
    if (t.IsSlice()) {
        return true;
    }
    switch (t.kind) {
    case TypeRef::Kind::Tuple:
    case TypeRef::Kind::Array:
    // A native sum, optional, or fallible is a tag followed by its widest payload.
    case TypeRef::Kind::Sum:
    case TypeRef::Kind::Optional:
    case TypeRef::Kind::Fallible:
        return true;
    case TypeRef::Kind::Named: {
        const std::string base = BaseTypeName(t.name);
        return interfaceNames.count(base) > 0 || layouts.contains(base) || (!t.inner.empty() && SizeOf(t) > 8);
    }
    default:
        return false;
    }
}

uint32_t X86_64ModuleEmitter::InternStr(const std::string &val) {
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

uint32_t X86_64ModuleEmitter::InternF32(const std::string &val) {
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

uint32_t X86_64ModuleEmitter::InternF64(const std::string &val) {
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

uint32_t X86_64ModuleEmitter::InternF32SignMask() {
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

uint32_t X86_64ModuleEmitter::InternF64SignMask() {
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
    const uint32_t symbol =
        DefineDataSymbol("__f64_sign_mask", {}, RcuSymKind::Const, RcuSymVis::Local, RcuModuleSection::RoData, off, 8);
    (void)moduleBuilder.RecordInternedLiteral("f64", "sign-mask", symbol);
    return symbol;
}

// Module generation
void X86_64ModuleEmitter::EmitVtables() {
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

// A slice constant becomes two read-only symbols: its elements, and a
// {data, length} header under the constant's own name whose data field is
// relocated to point at them. Code then reaches the elements the same way
// it reaches those of any other slice.
void X86_64ModuleEmitter::EmitConstSlice(const LirConstDecl &c) {
    const int elemSize = std::max(SizeOf(c.elementType), 1);
    const uint32_t elemsOff = AlignRodata(AlignOf(c.elementType));
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

    dataSyms[c.name] =
        DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                         RcuSymVis::ForDefinition(c.isPublic, c.isExported), RcuModuleSection::RoData, headerOff, 16);
}

void X86_64ModuleEmitter::EmitConstArray(const LirConstDecl &c) {
    const int elemSize = std::max(SizeOf(c.elementType), 1);
    const uint32_t arrayOff = AlignRodata(AlignOf(c.elementType));
    for (const auto &element : c.elements) {
        AppendConstElement(element, c.elementType);
    }

    dataSyms[c.name] =
        DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const,
                         RcuSymVis::ForDefinition(c.isPublic, c.isExported), RcuModuleSection::RoData, arrayOff,
                         static_cast<uint32_t>(c.elements.size() * static_cast<std::size_t>(elemSize)));
}

// Any other constant is inlined at its uses, but its symbol is what `@C`, inline assembly, and another object or
// package reach it through, so it holds the value itself: the constant's bytes at its type's size and alignment, in
// read-only data.
void X86_64ModuleEmitter::EmitConstValue(const LirConstDecl &c) {
    const auto visibility = RcuSymVis::ForDefinition(c.isPublic, c.isExported);
    if (!c.hasValueData) {
        // A value with no layout here is never addressed through its symbol, which stays a zeroed placeholder.
        const auto offset = static_cast<uint32_t>(DataData().size());
        DataData().insert(DataData().end(), 8, 0);
        dataSyms[c.name] = DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const, visibility,
                                            RcuModuleSection::Data, offset, 8);
        return;
    }
    const std::vector<std::uint8_t> bytes = ConstantValueBytes(c, layouts, interfaceNames);
    const uint32_t offset = AlignRodata(ConstantValueAlignment(c, layouts, interfaceNames));
    (void)moduleBuilder.Append(RcuModuleSection::RoData, bytes);
    dataSyms[c.name] = DefineDataSymbol(c.name, c.type.ToString(), RcuSymKind::Const, visibility,
                                        RcuModuleSection::RoData, offset, static_cast<uint32_t>(bytes.size()));
}

// A module variable is writable storage under its own name, starting as its initial value.
void X86_64ModuleEmitter::EmitGlobal(const LirGlobal &global) {
    const PlacedGlobal placed = PlaceGlobal(moduleBuilder, global, layouts, interfaceNames);
    dataSyms[global.name] = DefineDataSymbol(global.name, global.type.ToString(), RcuSymKind::Data,
                                             RcuSymVis::ForDefinition(global.isPublic, global.isExported),
                                             placed.section, placed.offset, placed.size);
}

void X86_64ModuleEmitter::GenModule() {
    PredeclareFunctions();
    // Extern vars
    for (const auto &ev : mod.externVars) {
        GetOrAddExtern(ev.name, RcuSymKind::ExternData);
    }
    // Module constants → read-only data symbols
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
        EmitConstValue(c);
    }
    for (const auto &global : mod.globals) {
        EmitGlobal(global);
    }
    EmitVtables();
    PredeclareFunctionTables();
    // Functions
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

void X86_64ModuleEmitter::PredeclareFunctionTables() {
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
void X86_64ModuleEmitter::EmitFunctionTables() {
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
        names.push_back(InternStr(range.name));
    }
    const uint32_t tableOff = AlignRodata(8);
    pushWord(functionRanges.size());
    for (std::size_t index = 0; index < functionRanges.size(); ++index) {
        const auto slot = static_cast<uint32_t>(RodataData().size());
        pushWord(0);
        AddRodataReloc(slot, functionRanges[index].symbol, RcuRelType::Abs64);
        pushWord(functionRanges[index].size);
        const auto nameSlot = static_cast<uint32_t>(RodataData().size());
        pushWord(0);
        AddRodataReloc(nameSlot, names[index], RcuRelType::Abs64);
        pushWord(functionRanges[index].name.size());
    }
    (void)moduleBuilder.DefineSymbol(dataSyms.at(backtrace.table), RcuModuleSection::RoData, tableOff,
                                     static_cast<uint32_t>(RodataData().size()) - tableOff);
    if (!backtrace.IsProgram()) {
        return;
    }
    const uint32_t tablesOff = AlignRodata(8);
    pushWord(backtrace.programTables.size());
    for (const std::string &name : backtrace.programTables) {
        const auto slot = static_cast<uint32_t>(RodataData().size());
        pushWord(0);
        const auto known = dataSyms.find(name);
        AddRodataReloc(slot, known != dataSyms.end() ? known->second : GetOrAddExtern(name, RcuSymKind::ExternData),
                       RcuRelType::Abs64);
    }
    (void)moduleBuilder.DefineSymbol(dataSyms.at(std::string(kFunctionTablesSymbol)), RcuModuleSection::RoData,
                                     tablesOff, static_cast<uint32_t>(RodataData().size()) - tablesOff);
}
} // namespace Rux::X86_64Detail
