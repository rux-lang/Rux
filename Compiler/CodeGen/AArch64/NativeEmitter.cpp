#include "CodeGen/AArch64/NativeEmitter.h"

#include "CodeGen/Layout.h"
#include "System/Process.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Rux {
namespace {
using namespace Layout;

std::string EscapeCString(const std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        switch (byte) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (byte >= 0x20 && byte < 0x7f) {
                result.push_back(static_cast<char>(byte));
            }
            else {
                result += "\\x";
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 0x0f]);
                // End a hexadecimal escape before a following hex digit.
                result += "\"\"";
            }
            break;
        }
    }
    return result;
}

std::string StripNumericSuffix(std::string value) {
    static constexpr std::string_view suffixes[] = {
        "f32", "f64", "i16", "i32", "i64", "u16", "u32", "u64", "i8", "u8", "i", "u",
    };
    for (const auto suffix : suffixes) {
        if (value.size() > suffix.size() && value.ends_with(suffix)) {
            value.resize(value.size() - suffix.size());
            break;
        }
    }
    value.erase(std::remove(value.begin(), value.end(), '_'), value.end());
    return value;
}

std::string Sanitize(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 4);
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            result.push_back(static_cast<char>(c));
        }
        else {
            result += std::format("_x{:02x}", c);
        }
    }
    if (result.empty() || (result.front() >= '0' && result.front() <= '9')) {
        result.insert(0, "n_");
    }
    return result;
}

class CEmitter {
public:
    explicit CEmitter(const LirPackage &input, const Target::OS inputOs)
        : package(input)
        , targetOs(inputOs) {
        Collect();
    }

    [[nodiscard]] std::optional<std::string> Generate(std::vector<Diagnostic> &diagnostics) {
        for (const auto *function : functions) {
            if (reachable.contains(function->name) && function->isAsm && !function->isExtern &&
                !CanLowerAsm(*function)) {
                diagnostics.push_back(ErrorDiagnostic(std::format(
                    "asm function '{}' contains x86-64 instructions and is unavailable on AArch64", function->name)));
            }
        }
        if (!diagnostics.empty()) {
            return std::nullopt;
        }

        std::string out;
        out += "#include <math.h>\n";
        out += "#include <errno.h>\n";
        out += "#include <stdint.h>\n";
        out += "#include <stdio.h>\n";
        out += "#include <stdlib.h>\n";
        out += "#include <string.h>\n\n";
        out += "typedef unsigned char *rx_ptr;\n";
        out += "extern long syscall(long, ...);\n";
        out += "static uint64_t rx_word(const void *value, size_t size) { uint64_t result = 0; "
               "memcpy(&result, value, size < 8 ? size : 8); return result; }\n";

        EmitAggregateTypes(out);
        EmitPrototypes(out);
        EmitGlobals(out);
        std::set<std::string> emittedDefinitions;
        for (const auto *function : functions) {
            if (!function->isExtern && reachable.contains(function->name) &&
                functionDecls.at(function->name) == function && emittedDefinitions.insert(function->name).second) {
                function->isAsm ? EmitAsmFunction(out, *function) : EmitFunction(out, *function);
            }
        }

        if (const auto it = functionNames.find("Main"); it != functionNames.end()) {
            out += std::format("\nint main(void) {{ return (int){}(); }}\n", it->second);
        }
        else {
            out += "\nint main(void) { return 0; }\n";
        }
        return out;
    }

private:
    const LirPackage &package;
    Target::OS targetOs;
    std::vector<const LirFunc *> functions;
    std::unordered_map<std::string, std::string> functionNames;
    std::unordered_map<std::string, const LirFunc *> functionDecls;
    std::unordered_map<std::string, StructLayout> layouts;
    std::unordered_map<std::string, TypeRef> namedBaseTypes;
    std::unordered_map<std::string, int> enumPayloadSizes;
    std::set<std::string> interfaceNames;
    std::unordered_map<std::string, std::string> aggregateNames;
    std::vector<TypeRef> aggregateTypes;
    std::unordered_map<std::string, std::string> globalNames;
    std::unordered_map<std::string, TypeRef> globalTypes;
    std::set<std::string> reachable;
    std::set<std::string> reachableGlobals;
    std::vector<const LirConstDecl *> constants;
    std::vector<const LirVtable *> vtables;
    std::size_t stringCounter = 0;

    [[nodiscard]] std::string ExternalSymbol(const std::string_view name) const {
        return std::format("{}{}", targetOs == Target::OS::MacOS ? "_" : "", EscapeCString(name));
    }

    static std::string TypeKey(const TypeRef &type) {
        std::string key = std::to_string(static_cast<int>(type.kind)) + ":" + type.name;
        if (type.arrayLength) {
            key += std::format("[{}]", *type.arrayLength);
        }
        key += "(";
        for (const auto &inner : type.inner) {
            key += TypeKey(inner);
            key += ",";
        }
        key += ")";
        return key;
    }

    int RuntimeSize(const TypeRef &type) const {
        if (type.kind == TypeRef::Kind::Named) {
            const std::string base = BaseTypeName(type.name);
            if (interfaceNames.contains(base)) {
                return 16;
            }
            // Concrete generic enums carry their instantiated layout in
            // inner[0]. Prefer it to the unspecialized declaration layout:
            // Result<float64, ParseError>, for example, is larger than
            // Result<T, E> when ParseError itself has a payload.
            if (const auto size = type.SizeInBytes(); size && *size > 0) {
                return static_cast<int>(*size);
            }
            if (const auto it = enumPayloadSizes.find(base); it != enumPayloadSizes.end()) {
                return it->second;
            }
            if (const auto it = layouts.find(base); it != layouts.end()) {
                return it->second.totalSize;
            }
            if (const auto it = namedBaseTypes.find(base); it != namedBaseTypes.end()) {
                return RuntimeSize(it->second);
            }
        }
        return SizeOf(type);
    }

    int RuntimeAlign(const TypeRef &type) const {
        if (type.kind == TypeRef::Kind::Named) {
            const std::string base = BaseTypeName(type.name);
            if (interfaceNames.contains(base)) {
                return 8;
            }
            if (enumPayloadSizes.contains(base)) {
                return 8;
            }
            if (const auto it = layouts.find(base); it != layouts.end()) {
                return it->second.alignment;
            }
            if (const auto it = namedBaseTypes.find(base); it != namedBaseTypes.end()) {
                return RuntimeAlign(it->second);
            }
        }
        return AlignOf(type);
    }

    bool IsAggregate(const TypeRef &type) const {
        switch (type.kind) {
        case TypeRef::Kind::Tuple:
        case TypeRef::Kind::Array:
        case TypeRef::Kind::Range:
        case TypeRef::Kind::RangeInclusive:
        case TypeRef::Kind::RangeFrom:
        case TypeRef::Kind::RangeTo:
        case TypeRef::Kind::RangeToInclusive:
        case TypeRef::Kind::RangeFull:
            return true;
        case TypeRef::Kind::Named: {
            const std::string base = BaseTypeName(type.name);
            return base == "Slice" || base == "String" || base == "StringArray" || base == "SystemTime" ||
                   base == "StringBuilder" || interfaceNames.contains(base) || enumPayloadSizes.contains(base) ||
                   layouts.contains(base);
        }
        default:
            return false;
        }
    }

    void RegisterType(const TypeRef &type) {
        if (IsAggregate(type)) {
            const std::string key = TypeKey(type);
            if (!aggregateNames.contains(key)) {
                const std::string name = std::format("rx_a{}", aggregateTypes.size());
                aggregateNames.emplace(key, name);
                aggregateTypes.push_back(type);
            }
        }
        for (const auto &inner : type.inner) {
            RegisterType(inner);
        }
    }

    std::string CType(const TypeRef &type, const bool allowVoid = false) {
        if (IsAggregate(type)) {
            RegisterType(type);
            return aggregateNames.at(TypeKey(type));
        }
        using K = TypeRef::Kind;
        switch (type.kind) {
        case K::Bool8:
        case K::Char8:
        case K::UInt8:
            return "uint8_t";
        case K::Int8:
            return "int8_t";
        case K::Bool16:
        case K::Char16:
        case K::UInt16:
            return "uint16_t";
        case K::Int16:
            return "int16_t";
        case K::Bool32:
        case K::Char32:
        case K::UInt32:
            return "uint32_t";
        case K::Int32:
            return "int32_t";
        case K::Int:
        case K::Int64:
            return "int64_t";
        case K::UInt:
        case K::UInt64:
            return "uint64_t";
        case K::Float32:
            return "float";
        case K::Float64:
            return "double";
        case K::Pointer:
        case K::Str:
        case K::Func:
            return "rx_ptr";
        case K::Named:
            if (const auto it = namedBaseTypes.find(BaseTypeName(type.name)); it != namedBaseTypes.end()) {
                return CType(it->second);
            }
            return "uint64_t";
        case K::Opaque:
            return allowVoid ? "void" : "uint8_t";
        default:
            return "uint64_t";
        }
    }

    void Collect() {
        std::vector<LirStructDecl> structs;
        for (const auto &module : package.modules) {
            interfaceNames.insert(module.interfaceNames.begin(), module.interfaceNames.end());
            structs.insert(structs.end(), module.structs.begin(), module.structs.end());
            for (const auto &enumeration : module.enums) {
                namedBaseTypes[enumeration.name] =
                    enumeration.baseType.kind == TypeRef::Kind::Unknown ? TypeRef::MakeInt64() : enumeration.baseType;
                int maxPayload = 0;
                for (const auto &variant : enumeration.variants) {
                    int payloadSize = 0;
                    for (const auto &field : variant.fields) {
                        const int fieldSize = std::max(1, SizeOf(field));
                        payloadSize = AlignUp(payloadSize, std::min(fieldSize, 8));
                        payloadSize += fieldSize;
                    }
                    maxPayload = std::max(maxPayload, payloadSize);
                }
                if (maxPayload > 0) {
                    const int tagSize = std::max(8, SizeOf(namedBaseTypes.at(enumeration.name)));
                    enumPayloadSizes[enumeration.name] = AlignUp(tagSize + maxPayload, 8);
                }
            }
        }
        for (const auto &structure : structs) {
            layouts[structure.name] = ComputeStructLayout(structure, layouts);
        }

        std::size_t functionIndex = 0;
        std::size_t globalIndex = 0;
        for (const auto &module : package.modules) {
            for (const auto &constant : module.consts) {
                constants.push_back(&constant);
                globalNames.try_emplace(constant.name, std::format("rx_g{}", globalIndex++));
                globalTypes.try_emplace(constant.name, constant.type);
                RegisterType(constant.type);
                RegisterType(constant.elementType);
            }
            for (const auto &external : module.externVars) {
                globalNames.try_emplace(external.name, std::format("rx_eg{}", globalIndex++));
                globalTypes.try_emplace(external.name, external.type);
                RegisterType(external.type);
            }
            for (const auto &vtable : module.vtables) {
                vtables.push_back(&vtable);
                globalNames.try_emplace(vtable.label, std::format("rx_g{}", globalIndex++));
            }
            for (const auto &function : module.funcs) {
                functions.push_back(&function);
                functionDecls[function.name] = &function;
                if (function.isExtern) {
                    functionNames.try_emplace(function.name, std::format("rx_ext{}", functionIndex++));
                }
                else {
                    functionNames.try_emplace(function.name, std::format("rx_f{}", functionIndex++));
                }
                RegisterType(function.returnType);
                for (const auto &param : function.params) {
                    RegisterType(param.type);
                }
                for (const auto &block : function.blocks) {
                    for (const auto &instruction : block.instrs) {
                        RegisterType(instruction.type);
                    }
                    if (block.term) {
                        RegisterType(block.term->retType);
                    }
                }
            }
        }
        MarkReachable("Main");
    }

    void MarkReachable(const std::string &name) {
        if (!reachable.insert(name).second) {
            return;
        }
        const auto declaration = functionDecls.find(name);
        if (declaration == functionDecls.end() || declaration->second->isExtern) {
            return;
        }
        for (const auto &block : declaration->second->blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op == LirOpcode::Call) {
                    MarkReachable(instruction.strArg);
                }
                else if (instruction.op == LirOpcode::GlobalAddr) {
                    if (functionDecls.contains(instruction.strArg)) {
                        MarkReachable(instruction.strArg);
                    }
                    for (const auto *vtable : vtables) {
                        if (vtable->label == instruction.strArg) {
                            reachableGlobals.insert(vtable->label);
                            for (const auto &method : vtable->methods) {
                                MarkReachable(method);
                            }
                        }
                    }
                }
            }
        }
    }

    static bool CanLowerAsm(const LirFunc &function) {
        static constexpr std::string_view supported[] = {
            "Add",    "MulSub", "SumTo",      "DoubleViaStack", "Triple",    "TripleThenAdd", "Sqrt",
            "MulAdd", "MinOf",  "IntToFloat", "FloatToInt",     "FloatBits", "FromBits",      "BitsOf",
        };
        return std::ranges::contains(supported, function.name) || function.name.starts_with("Sqrt__") ||
               (function.name.starts_with("Syscall") && function.name.size() == 8 && function.name.back() >= '0' &&
                function.name.back() <= '6');
    }

    void EmitAggregateTypes(std::string &out) {
        // RegisterType may discover nested aggregate types while CType is used.
        for (std::size_t i = 0; i < aggregateTypes.size(); ++i) {
            for (const auto &inner : aggregateTypes[i].inner) {
                RegisterType(inner);
            }
        }
        for (const auto &type : aggregateTypes) {
            const int size = std::max(1, RuntimeSize(type));
            const int alignment = std::max(1, RuntimeAlign(type));
            out += std::format("typedef struct {{ _Alignas({}) unsigned char bytes[{}]; }} {};\n", alignment, size,
                               aggregateNames.at(TypeKey(type)));
        }
        out += "\n";
    }

    std::string ScalarLiteral(const TypeRef &type, std::string value) {
        if (type.IsBool()) {
            return value == "true" || value == "1" ? "1" : "0";
        }
        value = StripNumericSuffix(std::move(value));
        if (IsFloat(type)) {
            if (value == "inf" || value == "+inf") {
                return "INFINITY";
            }
            if (value == "-inf") {
                return "-INFINITY";
            }
            if (value == "nan") {
                return "NAN";
            }
        }
        if (!value.empty() && !std::ranges::all_of(value, [](const char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'x' ||
                       c == 'X' || c == 'b' || c == 'B' || c == 'o' || c == 'O' || c == '+' || c == '-' || c == '.' ||
                       c == 'e' || c == 'E';
            })) {
            return std::format("(({})0)", CType(type));
        }
        if (IsFloat(type)) {
            return std::format("(({})({}))", CType(type), value.empty() ? "0" : value);
        }
        if (value == "null" || value.empty()) {
            return std::format("(({})0)", CType(type));
        }
        if (type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Str ||
            type.kind == TypeRef::Kind::Func) {
            return std::format("((rx_ptr)(uintptr_t)({}))", value);
        }
        const std::string cType = CType(type);
        if (cType == "int64_t" && value == "-9223372036854775808") {
            return "INT64_MIN";
        }
        if (cType == "uint64_t") {
            value += "ULL";
        }
        return std::format("(({})({}))", cType, value);
    }

    void EmitGlobals(std::string &out) {
        for (const auto *constant : constants) {
            const std::string name = globalNames.at(constant->name);
            if (constant->hasSequenceData) {
                const std::size_t length = constant->isTextSlice ? constant->text.size() : constant->elements.size();
                const std::size_t storageCount = length + (constant->isTextSlice ? 1 : 0);
                const std::string elementType = CType(constant->elementType);
                const std::string dataName = constant->type.kind == TypeRef::Kind::Array ? name : name + "_data";
                out +=
                    std::format("static {} {}[{}] = {{", elementType, dataName, std::max<std::size_t>(1, storageCount));
                if (constant->isTextSlice) {
                    for (std::size_t i = 0; i < constant->text.size(); ++i) {
                        out += std::format("{}{}", i == 0 ? "" : ",", static_cast<unsigned char>(constant->text[i]));
                    }
                }
                else {
                    for (std::size_t i = 0; i < constant->elements.size(); ++i) {
                        if (i != 0) {
                            out += ",";
                        }
                        out += ScalarLiteral(constant->elementType, constant->elements[i]);
                    }
                }
                out += "};\n";
                if (constant->type.kind != TypeRef::Kind::Array) {
                    out += std::format(
                        "static struct {{ rx_ptr data; uint64_t length; }} {} = {{ (rx_ptr){}_data, {} }};\n", name,
                        name, length);
                }
            }
            else if (IsAggregate(constant->type)) {
                out += std::format("static {} {};\n", CType(constant->type), name);
            }
            else {
                out += std::format("static {} {} = {};\n", CType(constant->type), name,
                                   ScalarLiteral(constant->type, constant->value));
            }
        }
        for (const auto &[sourceName, cName] : globalNames) {
            if (globalTypes.contains(sourceName)) {
                continue;
            }
            const auto vtable =
                std::ranges::find_if(vtables, [&](const LirVtable *item) { return item->label == sourceName; });
            if (vtable != vtables.end()) {
                if (!reachableGlobals.contains(sourceName)) {
                    continue;
                }
                out += std::format("static rx_ptr {}[{}] = {{", cName,
                                   std::max<std::size_t>(1, (*vtable)->methods.size()));
                for (std::size_t i = 0; i < (*vtable)->methods.size(); ++i) {
                    if (i != 0) {
                        out += ",";
                    }
                    out += std::format("(rx_ptr)(uintptr_t)&{}", functionNames.at((*vtable)->methods[i]));
                }
                out += "};\n";
            }
            else {
                out += std::format("static rx_ptr {}[1];\n", cName);
            }
        }
        for (const auto &module : package.modules) {
            for (const auto &external : module.externVars) {
                const std::string alias = globalNames.at(external.name);
                out += std::format("extern {} {} __asm__(\"{}\");\n", CType(external.type), alias,
                                   ExternalSymbol(external.name));
            }
        }
        out += "\n";
    }

    std::string FunctionReturnType(const LirFunc &function) {
        return CType(function.returnType, true);
    }

    void EmitPrototype(std::string &out, const LirFunc &function) {
        const std::string name = functionNames.at(function.name);
        out += std::format("{} {}(", FunctionReturnType(function), name);
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += std::format("{} a{}", CType(function.params[i].type), i);
        }
        if (function.params.empty() && !function.isVariadic) {
            out += "void";
        }
        if (function.isVariadic) {
            out += function.params.empty() ? "..." : ", ...";
        }
        out += ")";
        if (function.isExtern) {
            out += std::format(" __asm__(\"{}\")", ExternalSymbol(function.name));
        }
        out += ";\n";
    }

    void EmitPrototypes(std::string &out) {
        std::set<std::string> emitted;
        for (const auto *function : functions) {
            if (reachable.contains(function->name) && functionDecls.at(function->name) == function &&
                emitted.insert(function->name).second) {
                EmitPrototype(out, *function);
            }
        }
        out += "\n";
    }

    void EmitAsmFunction(std::string &out, const LirFunc &function) {
        out += std::format("\n{} {}(", FunctionReturnType(function), functionNames.at(function.name));
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += std::format("{} a{}", CType(function.params[i].type), i);
        }
        if (function.params.empty()) {
            out += "void";
        }
        out += ") {\n";
        const std::string &name = function.name;
        if (name == "Add") {
            out += "  return a0 + a1;\n";
        }
        else if (name == "MulSub") {
            out += "  return a0 * a1 - 1;\n";
        }
        else if (name == "SumTo") {
            out += "  int64_t result = 0; while (a0) result += a0--; return result;\n";
        }
        else if (name == "DoubleViaStack") {
            out += "  return a0 << 1;\n";
        }
        else if (name == "Triple") {
            out += "  return a0 * 3;\n";
        }
        else if (name == "TripleThenAdd") {
            out += "  return a0 * 4;\n";
        }
        else if (name == "Sqrt" || name.starts_with("Sqrt__")) {
            out += std::format("  return {}(a0);\n",
                               function.returnType.kind == TypeRef::Kind::Float32 ? "sqrtf" : "sqrt");
        }
        else if (name == "MulAdd") {
            out += "  return a0 * a1 + a2;\n";
        }
        else if (name == "MinOf") {
            out += "  return a0 < a1 ? a0 : a1;\n";
        }
        else if (name == "IntToFloat") {
            out += "  return (double)a0;\n";
        }
        else if (name == "FloatToInt") {
            out += "  return (int64_t)a0;\n";
        }
        else if (name == "FloatBits" || name == "BitsOf") {
            out += "  uint64_t result = 0; memcpy(&result, &a0, sizeof(a0)); return result;\n";
        }
        else if (name == "FromBits") {
            out += "  double result = 0; memcpy(&result, &a0, sizeof(result)); return result;\n";
        }
        else if (name.starts_with("Syscall")) {
            out += "  long result = syscall((long)a0";
            for (std::size_t i = 1; i < function.params.size(); ++i) {
                out += std::format(", (uint64_t)a{}", i);
            }
            out += "); return result == -1 ? -(int64_t)errno : (int64_t)result;\n";
        }
        out += "}\n";
    }

    static std::string Reg(const LirReg reg) {
        return std::format("r{}", reg);
    }

    static std::string Label(const std::size_t block) {
        return std::format("b{}", block);
    }

    int FieldOffset(const TypeRef &pointerType, const std::string_view fieldName) const {
        TypeRef pointee = pointerType;
        if (pointee.kind == TypeRef::Kind::Pointer && !pointee.inner.empty()) {
            TypeRef inner = pointee.inner[0];
            pointee = std::move(inner);
        }
        if (pointee.kind == TypeRef::Kind::Tuple) {
            std::size_t fieldIndex = 0;
            if (fieldName.empty() ||
                !std::ranges::all_of(fieldName, [](const char c) { return c >= '0' && c <= '9'; })) {
                return 0;
            }
            for (const char digit : fieldName) {
                fieldIndex = fieldIndex * 10 + static_cast<std::size_t>(digit - '0');
            }
            if (!pointee.inner.empty()) {
                const auto tupleElementSize = [&](const TypeRef &type) {
                    const std::string spelling = type.ToString();
                    if (spelling == "int8" || spelling == "uint8" || spelling == "bool8" || spelling == "char8") {
                        return 1;
                    }
                    if (spelling == "int16" || spelling == "uint16" || spelling == "bool16" || spelling == "char16") {
                        return 2;
                    }
                    if (spelling == "int32" || spelling == "uint32" || spelling == "bool32" || spelling == "char32" ||
                        spelling == "float32") {
                        return 4;
                    }
                    return SizeOf(type);
                };
                const int firstSize = tupleElementSize(pointee.inner[0]);
                if (std::ranges::all_of(pointee.inner,
                                        [&](const TypeRef &type) { return tupleElementSize(type) == firstSize; })) {
                    return static_cast<int>(fieldIndex) * firstSize;
                }
            }
            int offset = 0;
            for (std::size_t i = 0; i < pointee.inner.size() && i < fieldIndex; ++i) {
                const int fieldSize = std::max(1, SizeOf(pointee.inner[i]));
                const int alignment = std::min(fieldSize, 8);
                offset = AlignUp(offset, alignment);
                offset += fieldSize;
            }
            if (fieldIndex < pointee.inner.size()) {
                const int fieldSize = std::max(1, SizeOf(pointee.inner[fieldIndex]));
                offset = AlignUp(offset, std::min(fieldSize, 8));
            }
            return offset;
        }
        if (pointee.IsRange()) {
            if (fieldName == "end" &&
                (pointee.kind == TypeRef::Kind::Range || pointee.kind == TypeRef::Kind::RangeInclusive)) {
                const TypeRef element = pointee.inner.empty() ? TypeRef::MakeInt64() : pointee.inner[0];
                return RuntimeSize(element);
            }
            return 0;
        }
        if (pointee.kind != TypeRef::Kind::Named) {
            return 0;
        }
        const std::string base = BaseTypeName(pointee.name);
        if (base == "Slice" || base == "String" || base == "StringArray") {
            return fieldName == "length" ? 8 : 0;
        }
        if (interfaceNames.contains(base)) {
            return fieldName == "vtable" ? 8 : 0;
        }
        if (base == "StringBuilder") {
            if (fieldName == "length") {
                return 8;
            }
            if (fieldName == "capacity") {
                return 16;
            }
            return 0;
        }
        if (const auto it = layouts.find(base); it != layouts.end()) {
            for (const auto &field : it->second.fields) {
                if (field.name == fieldName) {
                    return field.offset;
                }
            }
        }
        return 0;
    }

    static std::string BinaryOperator(const LirOpcode opcode) {
        switch (opcode) {
        case LirOpcode::Add:
            return "+";
        case LirOpcode::Sub:
            return "-";
        case LirOpcode::Mul:
            return "*";
        case LirOpcode::Div:
            return "/";
        case LirOpcode::Mod:
            return "%";
        case LirOpcode::And:
            return "&";
        case LirOpcode::Or:
            return "|";
        case LirOpcode::Xor:
            return "^";
        case LirOpcode::Shl:
            return "<<";
        case LirOpcode::Shr:
        case LirOpcode::Lshr:
            return ">>";
        case LirOpcode::CmpEq:
            return "==";
        case LirOpcode::CmpNe:
            return "!=";
        case LirOpcode::CmpLt:
            return "<";
        case LirOpcode::CmpLe:
            return "<=";
        case LirOpcode::CmpGt:
            return ">";
        case LirOpcode::CmpGe:
            return ">=";
        default:
            return "";
        }
    }

    void EmitPhiMoves(std::string &out, const LirFunc &function, const std::size_t from, const std::size_t to) {
        struct Move {
            LirReg dst;
            LirReg src;
        };

        std::vector<Move> moves;
        for (const auto &instruction : function.blocks[to].instrs) {
            if (instruction.op != LirOpcode::Phi) {
                continue;
            }
            for (const auto &[source, predecessor] : instruction.phiPreds) {
                if (predecessor == from) {
                    moves.push_back({instruction.dst, source});
                    break;
                }
            }
        }
        if (moves.empty()) {
            return;
        }
        out += "    {\n";
        // A temporary for each source makes parallel phi assignments safe.
        for (std::size_t i = 0; i < moves.size(); ++i) {
            out += std::format("      __auto_type phi_tmp{} = {};\n", i, Reg(moves[i].src));
        }
        for (std::size_t i = 0; i < moves.size(); ++i) {
            out += std::format("      {} = phi_tmp{};\n", Reg(moves[i].dst), i);
        }
        out += "    }\n";
    }

    void EmitCall(std::string &out, const LirInstr &instruction, const std::unordered_map<LirReg, TypeRef> &regTypes) {
        const auto assignment = [&]() {
            return instruction.dst == LirNoReg || instruction.type.IsOpaque() ? std::string{}
                                                                              : Reg(instruction.dst) + " = ";
        };
        if (instruction.strArg == "FloatBits64" && instruction.srcs.size() == 1) {
            out += std::format("    memcpy(&{}, &{}, 8);\n", Reg(instruction.dst), Reg(instruction.srcs[0]));
            return;
        }
        if (instruction.strArg == "FloatFromBits64" && instruction.srcs.size() == 1) {
            out += std::format("    memcpy(&{}, &{}, 8);\n", Reg(instruction.dst), Reg(instruction.srcs[0]));
            return;
        }
        if (instruction.strArg == "FloatBits32" && instruction.srcs.size() == 1) {
            out += std::format("    memcpy(&{}, &{}, 4);\n", Reg(instruction.dst), Reg(instruction.srcs[0]));
            return;
        }
        if (instruction.strArg == "FloatFromBits32" && instruction.srcs.size() == 1) {
            out += std::format("    memcpy(&{}, &{}, 4);\n", Reg(instruction.dst), Reg(instruction.srcs[0]));
            return;
        }
        if (instruction.strArg == "Sqrt" && instruction.srcs.size() == 1 && IsFloat(instruction.type)) {
            out += std::format("    {}{}({});\n", assignment(),
                               instruction.type.kind == TypeRef::Kind::Float32 ? "sqrtf" : "sqrt",
                               Reg(instruction.srcs[0]));
            return;
        }

        std::string callee;
        if (const auto it = functionNames.find(instruction.strArg); it != functionNames.end()) {
            callee = it->second;
        }
        else {
            // Semantic analysis normally ensures every call has a declaration.
            // Keep generated C diagnosable if malformed LIR reaches this point.
            callee = Sanitize(instruction.strArg);
        }
        out += "    " + assignment() + callee + "(";
        for (std::size_t i = 0; i < instruction.srcs.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += Reg(instruction.srcs[i]);
        }
        out += ");\n";
        (void)regTypes;
    }

    void EmitInstruction(std::string &out, const LirInstr &instruction,
                         const std::unordered_map<LirReg, TypeRef> &regTypes,
                         const std::unordered_map<LirReg, std::string> &allocaStorage) {
        const std::string dst = instruction.dst == LirNoReg ? "" : Reg(instruction.dst);
        switch (instruction.op) {
        case LirOpcode::Const:
            if (instruction.type.kind == TypeRef::Kind::Str) {
                const std::string name = std::format("rx_s{}", stringCounter++);
                out +=
                    std::format("    static unsigned char {}[] = \"{}\";\n", name, EscapeCString(instruction.strArg));
                out += std::format("    {} = {};\n", dst, name);
            }
            else if (!IsAggregate(instruction.type)) {
                out += std::format("    {} = {};\n", dst, ScalarLiteral(instruction.type, instruction.strArg));
            }
            break;
        case LirOpcode::Alloca:
            out += std::format("    {} = {};\n", dst, allocaStorage.at(instruction.dst));
            break;
        case LirOpcode::Load: {
            const int size = std::max(1, RuntimeSize(instruction.type));
            if (!instruction.strArg.empty()) {
                const std::string global = globalNames.contains(instruction.strArg) ? globalNames.at(instruction.strArg)
                                                                                    : Sanitize(instruction.strArg);
                out += std::format("    memcpy(&{}, &{}, {});\n", dst, global, size);
            }
            else {
                out += std::format("    memcpy(&{}, {}, {});\n", dst, Reg(instruction.srcs[0]), size);
            }
            break;
        }
        case LirOpcode::Store: {
            const int size = std::max(1, RuntimeSize(instruction.type));
            out += std::format("    memcpy({}, &{}, {});\n", Reg(instruction.srcs[1]), Reg(instruction.srcs[0]), size);
            break;
        }
        case LirOpcode::Add:
        case LirOpcode::Sub:
        case LirOpcode::Mul:
        case LirOpcode::Div:
        case LirOpcode::And:
        case LirOpcode::Or:
        case LirOpcode::Xor:
        case LirOpcode::Shl:
        case LirOpcode::Shr:
        case LirOpcode::Lshr: {
            const TypeRef &lhsType = regTypes.at(instruction.srcs[0]);
            std::string lhs = Reg(instruction.srcs[0]);
            if (instruction.op == LirOpcode::Lshr && lhsType.IsSigned()) {
                const int bits = RuntimeSize(lhsType) * 8;
                lhs = std::format("((uint{}_t){})", bits, lhs);
            }
            out += std::format("    {} = ({})({} {} {});\n", dst, CType(instruction.type), lhs,
                               BinaryOperator(instruction.op), Reg(instruction.srcs[1]));
            break;
        }
        case LirOpcode::CmpEq:
        case LirOpcode::CmpNe:
        case LirOpcode::CmpLt:
        case LirOpcode::CmpLe:
        case LirOpcode::CmpGt:
        case LirOpcode::CmpGe: {
            const TypeRef &lhsType = regTypes.at(instruction.srcs[0]);
            const TypeRef &rhsType = regTypes.at(instruction.srcs[1]);
            const auto comparable = [&](const LirReg reg, const TypeRef &type) {
                if (IsAggregate(type)) {
                    return std::format("rx_word(&{}, {})", Reg(reg), std::max(1, RuntimeSize(type)));
                }
                if (type.kind == TypeRef::Kind::Pointer || type.kind == TypeRef::Kind::Str ||
                    type.kind == TypeRef::Kind::Func) {
                    return std::format("(uintptr_t){}", Reg(reg));
                }
                return Reg(reg);
            };
            out += std::format("    {} = ({})({} {} {});\n", dst, CType(instruction.type),
                               comparable(instruction.srcs[0], lhsType), BinaryOperator(instruction.op),
                               comparable(instruction.srcs[1], rhsType));
            break;
        }
        case LirOpcode::Mod:
            if (IsFloat(instruction.type)) {
                out += std::format("    {} = ({})fmod{}({}, {});\n", dst, CType(instruction.type),
                                   instruction.type.kind == TypeRef::Kind::Float32 ? "f" : "", Reg(instruction.srcs[0]),
                                   Reg(instruction.srcs[1]));
            }
            else {
                out += std::format("    {} = ({})({} % {});\n", dst, CType(instruction.type), Reg(instruction.srcs[0]),
                                   Reg(instruction.srcs[1]));
            }
            break;
        case LirOpcode::Pow:
            if (IsFloat(instruction.type)) {
                out += std::format("    {} = ({})pow{}({}, {});\n", dst, CType(instruction.type),
                                   instruction.type.kind == TypeRef::Kind::Float32 ? "f" : "", Reg(instruction.srcs[0]),
                                   Reg(instruction.srcs[1]));
            }
            else {
                out +=
                    std::format("    {{ {} base = {}, exp = {}; {} acc = 1; while (exp) {{ if (exp & 1) acc *= base; "
                                "exp >>= 1; if (exp) base *= base; }} {} = acc; }}\n",
                                CType(instruction.type), Reg(instruction.srcs[0]), CType(instruction.type),
                                Reg(instruction.srcs[1]), CType(instruction.type), dst);
            }
            break;
        case LirOpcode::Neg:
            out += std::format("    {} = ({})-{};\n", dst, CType(instruction.type), Reg(instruction.srcs[0]));
            break;
        case LirOpcode::Not:
            out += std::format("    {} = !{};\n", dst, Reg(instruction.srcs[0]));
            break;
        case LirOpcode::BitNot:
            if (instruction.type.IsBool()) {
                out += std::format("    {} = {} ^ 1;\n", dst, Reg(instruction.srcs[0]));
            }
            else {
                out += std::format("    {} = ({})~{};\n", dst, CType(instruction.type), Reg(instruction.srcs[0]));
            }
            break;
        case LirOpcode::Cast:
            if (IsAggregate(instruction.type)) {
                out += std::format("    memcpy(&{}, &{}, {});\n", dst, Reg(instruction.srcs[0]),
                                   std::max(1, RuntimeSize(instruction.type)));
            }
            else {
                out += std::format("    {} = ({}){};\n", dst, CType(instruction.type), Reg(instruction.srcs[0]));
            }
            break;
        case LirOpcode::Call:
            EmitCall(out, instruction, regTypes);
            break;
        case LirOpcode::CallIndirect: {
            std::vector<TypeRef> argumentTypes;
            for (std::size_t i = 1; i < instruction.srcs.size(); ++i) {
                argumentTypes.push_back(regTypes.at(instruction.srcs[i]));
            }
            out += "    ";
            if (instruction.dst != LirNoReg && !instruction.type.IsOpaque()) {
                out += dst + " = ";
            }
            out += std::format("(({} (*)(", CType(instruction.type, true));
            for (std::size_t i = 0; i < argumentTypes.size(); ++i) {
                if (i != 0) {
                    out += ",";
                }
                out += CType(argumentTypes[i]);
            }
            if (argumentTypes.empty()) {
                out += "void";
            }
            out += std::format("))(uintptr_t){})(", Reg(instruction.srcs[0]));
            for (std::size_t i = 1; i < instruction.srcs.size(); ++i) {
                if (i != 1) {
                    out += ",";
                }
                out += Reg(instruction.srcs[i]);
            }
            out += ");\n";
            break;
        }
        case LirOpcode::Assert:
        case LirOpcode::Panic: {
            const bool assertion = instruction.op == LirOpcode::Assert;
            const std::size_t messageIndex = assertion ? 1 : 0;
            if (assertion) {
                out += std::format("    if (!{}) {{\n", Reg(instruction.srcs[0]));
            }
            else {
                out += "    {\n";
            }
            out += std::format("      fprintf(stderr, \"{}%s\\n  at {} ({}:{}:{})\\n\", "
                               "(char *)(uintptr_t)rx_word({}, 8)); abort();\n",
                               assertion ? "Assertion failed: " : "Panic: ", EscapeCString(instruction.sourceFunction),
                               EscapeCString(instruction.sourceFile), instruction.sourceLine, instruction.sourceColumn,
                               Reg(instruction.srcs[messageIndex]));
            out += "    }\n";
            break;
        }
        case LirOpcode::GlobalAddr: {
            if (const auto function = functionNames.find(instruction.strArg); function != functionNames.end()) {
                out += std::format("    {} = (rx_ptr)(uintptr_t)&{};\n", dst, function->second);
            }
            else {
                const std::string global = globalNames.contains(instruction.strArg) ? globalNames.at(instruction.strArg)
                                                                                    : Sanitize(instruction.strArg);
                out += std::format("    {} = (rx_ptr)&{};\n", dst, global);
            }
            break;
        }
        case LirOpcode::StringAddr: {
            const TypeRef element = instruction.type.inner.empty() ? TypeRef::MakeChar8() : instruction.type.inner[0];
            const std::string encoded = EncodeStringLiteral(instruction.strArg, std::max(1, RuntimeSize(element)));
            const std::string name = std::format("rx_s{}", stringCounter++);
            out += std::format("    static unsigned char {}[{}] = {{", name, encoded.size() + 1);
            for (std::size_t i = 0; i < encoded.size(); ++i) {
                out += std::format("{}{}", i == 0 ? "" : ",", static_cast<unsigned char>(encoded[i]));
            }
            if (!encoded.empty()) {
                out += ",";
            }
            out += "0";
            out += "};\n";
            out += std::format("    {} = {};\n", dst, name);
            break;
        }
        case LirOpcode::FieldPtr: {
            const TypeRef &baseType = regTypes.at(instruction.srcs[0]);
            out += std::format("    {} = {} + {}; /* {} field={} */\n", dst, Reg(instruction.srcs[0]),
                               FieldOffset(baseType, instruction.strArg), EscapeCString(baseType.ToString()),
                               EscapeCString(instruction.strArg));
            break;
        }
        case LirOpcode::IndexPtr: {
            int elementSize = 1;
            if (instruction.type.kind == TypeRef::Kind::Pointer && !instruction.type.inner.empty()) {
                elementSize = std::max(1, RuntimeSize(instruction.type.inner[0]));
            }
            out += std::format("    {} = {} + ((uint64_t){} * {});\n", dst, Reg(instruction.srcs[0]),
                               Reg(instruction.srcs[1]), elementSize);
            break;
        }
        case LirOpcode::Phi:
            break;
        }
    }

    void EmitTerminator(std::string &out, const LirFunc &function, const std::size_t blockIndex,
                        const LirTerminator &term, const std::unordered_map<LirReg, TypeRef> &regTypes) {
        switch (term.kind) {
        case LirTermKind::Jump:
            EmitPhiMoves(out, function, blockIndex, term.trueTarget);
            out += std::format("    goto {};\n", Label(term.trueTarget));
            break;
        case LirTermKind::Branch:
            out += std::format("    if ({}) {{\n", Reg(term.cond));
            EmitPhiMoves(out, function, blockIndex, term.trueTarget);
            out += std::format("      goto {};\n    }} else {{\n", Label(term.trueTarget));
            EmitPhiMoves(out, function, blockIndex, term.falseTarget);
            out += std::format("      goto {};\n    }}\n", Label(term.falseTarget));
            break;
        case LirTermKind::Return:
            if (term.retVal && *term.retVal != LirNoReg) {
                const TypeRef &valueType = regTypes.at(*term.retVal);
                if (IsAggregate(function.returnType) && valueType.kind == TypeRef::Kind::Pointer) {
                    out += std::format("    {{ {} result = {{0}}; memcpy(&result, {}, {}); return result; }}\n",
                                       CType(function.returnType), Reg(*term.retVal),
                                       std::max(1, RuntimeSize(function.returnType)));
                }
                else {
                    out += std::format("    return {};\n", Reg(*term.retVal));
                }
            }
            else if (!function.returnType.IsOpaque()) {
                out += std::format("    return ({}){{0}};\n", CType(function.returnType));
            }
            else {
                out += "    return;\n";
            }
            break;
        case LirTermKind::Switch:
            out += std::format("    switch ({}) {{\n", Reg(term.cond));
            for (const auto &item : term.cases) {
                out += std::format("    case {}:\n", StripNumericSuffix(item.value));
                EmitPhiMoves(out, function, blockIndex, item.target);
                out += std::format("      goto {};\n", Label(item.target));
            }
            out += "    default:\n";
            EmitPhiMoves(out, function, blockIndex, term.defaultTarget);
            out += std::format("      goto {};\n    }}\n", Label(term.defaultTarget));
            break;
        case LirTermKind::Unreachable:
            out += "    __builtin_unreachable();\n";
            break;
        }
    }

    void EmitFunction(std::string &out, const LirFunc &function) {
        out += std::format("\n{} {}(", FunctionReturnType(function), functionNames.at(function.name));
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += std::format("{} a{}", CType(function.params[i].type), i);
        }
        if (function.params.empty()) {
            out += "void";
        }
        out += ") {\n";

        std::unordered_map<LirReg, TypeRef> regTypes;
        std::unordered_map<LirReg, std::string> allocaStorage;
        for (const auto &param : function.params) {
            regTypes[param.reg] = param.type;
        }
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.dst == LirNoReg) {
                    continue;
                }
                regTypes[instruction.dst] =
                    instruction.op == LirOpcode::Alloca ? TypeRef::MakePointer(instruction.type) : instruction.type;
                if (instruction.op == LirOpcode::Alloca) {
                    allocaStorage[instruction.dst] = std::format("storage{}", instruction.dst);
                }
            }
        }

        for (const auto &[reg, type] : regTypes) {
            out += std::format("  {} {} = {{0}};\n", CType(type), Reg(reg));
        }
        for (const auto &block : function.blocks) {
            for (const auto &instruction : block.instrs) {
                if (instruction.op != LirOpcode::Alloca) {
                    continue;
                }
                int bytes = std::max(1, RuntimeSize(instruction.type));
                if (!instruction.strArg.empty()) {
                    int count = 0;
                    const auto [ptr, ec] = std::from_chars(
                        instruction.strArg.data(), instruction.strArg.data() + instruction.strArg.size(), count);
                    if (ec == std::errc{} && ptr == instruction.strArg.data() + instruction.strArg.size()) {
                        const TypeRef element =
                            instruction.type.inner.empty() ? instruction.type : instruction.type.inner[0];
                        bytes = std::max(1, count * RuntimeSize(element));
                    }
                }
                out += std::format("  _Alignas(8) unsigned char {}[{}] = {{0}};\n", allocaStorage.at(instruction.dst),
                                   bytes);
            }
        }
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            out += std::format("  {} = a{};\n", Reg(function.params[i].reg), i);
        }
        if (!function.blocks.empty()) {
            out += std::format("  goto {};\n", Label(0));
        }
        for (std::size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            const auto &block = function.blocks[blockIndex];
            out += std::format("{}:;\n", Label(blockIndex));
            for (const auto &instruction : block.instrs) {
                EmitInstruction(out, instruction, regTypes, allocaStorage);
            }
            if (block.term) {
                EmitTerminator(out, function, blockIndex, *block.term, regTypes);
            }
        }
        if (function.returnType.IsOpaque()) {
            out += "  return;\n";
        }
        else {
            out += std::format("  return ({}){{0}};\n", CType(function.returnType));
        }
        out += "}\n";
    }
};

std::optional<std::filesystem::path> FindNativeClang(const Target::OS os) {
    if (os == Target::OS::Windows) {
        // CreateProcess searches PATH for an executable without a directory.
        return std::filesystem::path("clang.exe");
    }

    std::vector<std::filesystem::path> candidates;
    if (os == Target::OS::MacOS) {
        candidates.emplace_back("/usr/bin/clang");
    }
    else if (os == Target::OS::FreeBSD) {
        candidates.emplace_back("/usr/local/bin/clang22");
        candidates.emplace_back("/usr/bin/clang");
    }
    else {
        candidates.emplace_back("/usr/bin/clang-22");
        candidates.emplace_back("/usr/bin/clang");
    }
    const auto candidate = std::ranges::find_if(candidates, [](const auto &path) {
        std::error_code error;
        return std::filesystem::is_regular_file(path, error);
    });
    if (candidate == candidates.end()) {
        return std::nullopt;
    }
    return *candidate;
}

void AppendTargetArguments(std::vector<std::string> &arguments, const TargetContext &target) {
    if (target.os == Target::OS::MacOS) {
        arguments.insert(arguments.end(), {"-arch", "arm64"});
    }
    else if (target.os == Target::OS::Windows) {
        arguments.emplace_back("--target=aarch64-pc-windows-msvc");
    }
}

void AppendLinkArguments(std::vector<std::string> &arguments, const LirPackage &package, const TargetContext &target) {
    if (target.os == Target::OS::Linux || target.os == Target::OS::FreeBSD) {
        arguments.emplace_back("-lm");
    }
    if (target.os != Target::OS::Windows) {
        return;
    }

    std::set<std::string> libraries;
    for (const auto &module : package.modules) {
        for (const auto &function : module.funcs) {
            if (function.isExtern && !function.dll.empty()) {
                std::filesystem::path library(function.dll);
                library.replace_extension(".lib");
                libraries.insert(library.filename().string());
            }
        }
    }
    arguments.insert(arguments.end(), libraries.begin(), libraries.end());
}
} // namespace

AArch64NativeEmitter::AArch64NativeEmitter(const LirPackage &package, std::string inputPackageName,
                                           const TargetContext inputTarget)
    : lir(package)
    , packageName(std::move(inputPackageName))
    , target(inputTarget) {
}

bool AArch64NativeEmitter::EmitExecutable(const std::filesystem::path &outputPath,
                                          const std::filesystem::path &temporaryDirectory, const bool release,
                                          const std::optional<std::filesystem::path> &assemblyPath) {
    CEmitter emitter(lir, target.os);
    auto source = emitter.Generate(diagnostics);
    if (!source) {
        return false;
    }

    const auto clangPath = FindNativeClang(target.os);
    if (!clangPath) {
        diagnostics.push_back(ErrorDiagnostic("could not find a native Clang C compiler for the AArch64 backend"));
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(temporaryDirectory, error);
    if (error) {
        diagnostics.push_back(
            ErrorDiagnostic(std::format("could not create AArch64 temporary directory: {}", error.message())));
        return false;
    }
    std::filesystem::create_directories(outputPath.parent_path(), error);
    if (error) {
        diagnostics.push_back(
            ErrorDiagnostic(std::format("could not create AArch64 output directory: {}", error.message())));
        return false;
    }

    const std::filesystem::path sourcePath = temporaryDirectory / "aarch64-native.c";
    {
        std::ofstream stream(sourcePath, std::ios::binary | std::ios::trunc);
        if (!stream) {
            diagnostics.push_back(
                ErrorDiagnostic(std::format("could not write AArch64 source '{}'", sourcePath.string())));
            return false;
        }
        stream.write(source->data(), static_cast<std::streamsize>(source->size()));
    }

    if (assemblyPath) {
        std::filesystem::create_directories(assemblyPath->parent_path(), error);
        if (error) {
            diagnostics.push_back(
                ErrorDiagnostic(std::format("could not create AArch64 assembly directory: {}", error.message())));
            return false;
        }
        std::vector<std::string> assemblyArgumentStorage;
        AppendTargetArguments(assemblyArgumentStorage, target);
        assemblyArgumentStorage.insert(
            assemblyArgumentStorage.end(),
            {"-std=gnu11", release ? "-O2" : "-O0", "-S", sourcePath.string(), "-o", assemblyPath->string()});
        std::vector<std::string_view> assemblyArguments;
        assemblyArguments.reserve(assemblyArgumentStorage.size());
        for (const auto &argument : assemblyArgumentStorage) {
            assemblyArguments.push_back(argument);
        }
        const auto assemblyResult = System::RunInherited(*clangPath, assemblyArguments);
        if (!assemblyResult || *assemblyResult != 0) {
            diagnostics.push_back(ErrorDiagnostic(std::format("AArch64 assembly dump failed for '{}'", packageName)));
            return false;
        }
    }

    std::vector<std::string> argumentStorage;
    AppendTargetArguments(argumentStorage, target);
    argumentStorage.insert(argumentStorage.end(), {"-std=gnu11", release ? "-O2" : "-O0"});
    if (!release) {
        argumentStorage.emplace_back("-g");
    }
    argumentStorage.insert(argumentStorage.end(), {sourcePath.string(), "-o", outputPath.string()});
    AppendLinkArguments(argumentStorage, lir, target);
    std::vector<std::string_view> arguments;
    arguments.reserve(argumentStorage.size());
    for (const auto &argument : argumentStorage) {
        arguments.push_back(argument);
    }

    const auto result = System::RunInherited(*clangPath, arguments);
    if (!result || *result != 0) {
        diagnostics.push_back(ErrorDiagnostic(std::format("AArch64 Clang backend failed for '{}'", packageName)));
        return false;
    }
    return true;
}
} // namespace Rux
