#include "Semantic/Detail/SemanticAnalyzerContext.h"
#include "Semantic/Detail/TypePropertyClassifier.h"

namespace Rux {
std::string_view ValueConsumptionKindName(const ValueConsumptionKind kind) noexcept {
    switch (kind) {
    case ValueConsumptionKind::Initialization:
        return "initialization";
    case ValueConsumptionKind::Argument:
        return "argument";
    case ValueConsumptionKind::Receiver:
        return "receiver";
    case ValueConsumptionKind::Return:
        return "return";
    case ValueConsumptionKind::Assignment:
        return "assignment";
    case ValueConsumptionKind::Aggregate:
        return "aggregate";
    case ValueConsumptionKind::ConditionalArm:
        return "conditional-arm";
    case ValueConsumptionKind::ExplicitMove:
        return "explicit";
    }
    return "unknown";
}
} // namespace Rux

namespace Rux::SemanticDetail {
bool SemanticAnalyzerContext::IsSpecialOperationName(const std::string_view name) {
    return name == "=" || name == "<-";
}

bool SemanticAnalyzerContext::IsDestructorName(const std::string_view name) {
    return name.size() > 1 && name.front() == '~';
}

void SemanticAnalyzerContext::ValidateConstructor(const FuncDecl &method, const TypeRef &extendedType) {
    const std::string typeName = NamedBaseTypeName(extendedType);
    if (typeName.empty() || method.name != typeName) {
        return;
    }
    if (method.Receiver()) {
        EmitError(method.location, std::format("constructor '{}' must not declare a 'self' receiver", typeName));
    }
    if (!method.typeParams.empty()) {
        EmitError(method.location, std::format("constructor '{}' cannot declare function type parameters", typeName),
                  {}, "write type arguments on the constructed type instead");
    }
    const TypeRef returnType = ResolveMethodReturnType(extendedType, method);
    if (!method.returnType || returnType != extendedType) {
        EmitError(method.location,
                  std::format("constructor '{}' must return exactly '{}'", typeName, extendedType.ToString()));
    }
    if (!method.body) {
        EmitError(method.location, std::format("constructor '{}' must have a body", typeName));
    }
}

bool SemanticAnalyzerContext::IsConstructorCandidate(const FuncDecl &method, const TypeRef &type) {
    const std::string typeName = NamedBaseTypeName(type);
    return !typeName.empty() && method.name == typeName && !method.Receiver() && method.typeParams.empty() &&
           method.body && method.returnType && ResolveMethodReturnType(type, method) == type;
}

std::vector<const FuncDecl *> SemanticAnalyzerContext::ConstructorCandidates(const TypeRef &type) {
    const std::string typeName = NamedBaseTypeName(type);
    const auto methods = methodsByType.find(typeName);
    if (methods == methodsByType.end()) {
        return {};
    }
    const auto named = methods->second.find(typeName);
    if (named == methods->second.end()) {
        return {};
    }
    std::vector<const FuncDecl *> result;
    for (const FuncDecl *method : named->second) {
        const auto source = functionDeclFiles.find(method);
        const bool accessible = method->isPublic || source == functionDeclFiles.end() || source->second == currentFile;
        if (accessible && IsConstructorCandidate(*method, type)) {
            result.push_back(method);
        }
    }
    return result;
}

void SemanticAnalyzerContext::ValidateDestructor(const FuncDecl &method, const TypeRef &extendedType) {
    if (!IsDestructorName(method.name)) {
        return;
    }

    const std::string typeName = NamedBaseTypeName(extendedType);
    const std::string expectedName = "~" + typeName;
    if (method.name != expectedName) {
        EmitError(method.location,
                  std::format("destructor '{}' must be named '{}' for type '{}'", method.name, expectedName, typeName));
    }

    const bool canonical = [&] {
        if (!method.typeParams.empty() || method.params.size() != 1 || method.returnType ||
            method.params[0].name != "self" || method.params[0].isMut || method.params[0].isVariadic ||
            method.params[0].defaultValue) {
            return false;
        }
        TypeRef receiver = ResolveType(*method.params[0].type);
        TypeRef expected = extendedType;
        if (receiver.kind != TypeRef::Kind::Reference || receiver.inner.empty() || !receiver.inner.front().isMut) {
            return false;
        }
        receiver.inner.front().isMut = false;
        expected.isMut = false;
        return receiver.inner.front() == expected;
    }();
    if (!canonical) {
        EmitError(method.location, std::format("destructor for type '{}' must have signature 'func {}(self: &var {})'",
                                               typeName, expectedName, extendedType.ToString()));
    }
    if (!method.body) {
        EmitError(method.location, std::format("destructor '{}' must have a body", expectedName));
    }
}

void SemanticAnalyzerContext::ValidateSpecialOperation(const FuncDecl &method, const TypeRef &extendedType) {
    if (!IsSpecialOperationName(method.name)) {
        return;
    }

    const auto sameValueType = [](TypeRef left, TypeRef right) {
        left.isMut = false;
        right.isMut = false;
        return left == right;
    };
    const bool copy = method.name == "=";
    const auto operationSignature = [&] {
        if (!method.typeParams.empty() || method.params.size() != 2 || method.returnType ||
            method.params[0].name != "self" || method.params[1].name != "other" || method.params[0].isMut ||
            method.params[1].isMut || method.params[0].isVariadic || method.params[1].isVariadic ||
            method.params[0].defaultValue || method.params[1].defaultValue) {
            return false;
        }

        const TypeRef receiver = ResolveType(*method.params[0].type);
        const TypeRef other = ResolveType(*method.params[1].type);
        if (receiver.kind != TypeRef::Kind::Reference || receiver.inner.empty() || !receiver.inner.front().isMut ||
            !sameValueType(receiver.inner.front(), extendedType)) {
            return false;
        }
        if (!copy) {
            return other.kind != TypeRef::Kind::Reference && sameValueType(other, extendedType);
        }
        if (other.kind != TypeRef::Kind::Reference || other.inner.empty() || other.inner.front().isMut) {
            return false;
        }
        // A body-bearing copy overload may construct T from another source type. Only the exact T-from-T signature
        // has the bodyless meaning "copy prohibited".
        return method.body || sameValueType(other.inner.front(), extendedType);
    }();
    if (operationSignature) {
        return;
    }

    const std::string type = extendedType.ToString();
    const std::string expected = copy && method.body ? std::format("func =(self: &var {0}, other: &Source)", type)
                               : copy                ? std::format("func =(self: &var {0}, other: &{0})", type)
                                                     : std::format("func <-(self: &var {0}, other: {0})", type);
    EmitError(method.location, std::format("{} special operation for type '{}' must have signature '{}'",
                                           copy ? "copy" : "move", type, expected));
}

TypeProperties SemanticAnalyzerContext::ClassifyTypeProperties(const TypeRef &type) {
    const std::string key = type.ToString();
    if (const auto known = typeProperties.find(key); known != typeProperties.end() && known->second.IsResolved()) {
        return known->second;
    }

    TypePropertyClassifier classifier(
        structDecls, enumDecls, unionDecls, interfaceDecls, typeImplementsInterfaces, methodsByType,
        [this](const TypeExpr &type, const TypePropertyClassifier::Substitutions &substitutions) {
            if (!typeNodeTypes.contains(&type)) {
                return TypeRef::MakeUnknown();
            }
            return ResolveTypeWithSubstitution(type, substitutions);
        },
        [this](const std::string &name) { return ParseTypeArgsFromTypeName(name); });
    const TypeProperties properties = classifier.Classify(type);
    if (properties.IsResolved()) {
        typeProperties.insert_or_assign(key, properties);
    }
    return properties;
}

void SemanticAnalyzerContext::RecordResolvedTypeProperties() {
    const auto record = [&](const TypeRef &type) {
        const TypeProperties properties = ClassifyTypeProperties(type);
        if (!properties.IsResolved()) {
            typeProperties.insert_or_assign(type.ToString(), properties);
        }
    };
    for (const auto &[_, type] : expressionTypes) {
        record(type);
    }
    for (const auto &[_, type] : typeNodeTypes) {
        record(type);
    }
    for (const auto &[_, type] : patternTypes) {
        record(type);
    }
    for (const auto &[name, declaration] : structDecls) {
        if (declaration->typeParams.empty()) {
            record(TypeRef::MakeNamed(name));
        }
    }
    for (const auto &[name, declaration] : enumDecls) {
        if (declaration->typeParams.empty()) {
            record(TypeRef::MakeNamed(name));
        }
    }
    for (const auto &[name, _] : unionDecls) {
        record(TypeRef::MakeNamed(name));
    }
}
} // namespace Rux::SemanticDetail
