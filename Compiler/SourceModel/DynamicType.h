#pragma once

#include <atomic>
#include <typeinfo>

namespace Rux {
/// The dynamic type of a polymorphic AST or HIR node, looked up once and kept in the node's `cachedType`, so the casts
/// that dispatch on node types compare one pointer instead of asking the runtime every time. The compiler is one
/// statically linked image, which holds exactly one `type_info` per type, so comparing addresses compares types.
template <typename Node>
[[nodiscard]] inline const std::type_info *DynamicTypeOf(const Node &node) noexcept {
    // Casts run on several threads at once; whichever stores first stores the same pointer the others would.
    std::atomic_ref<const std::type_info *> cached(node.cachedType);
    const std::type_info *type = cached.load(std::memory_order_relaxed);
    if (type == nullptr) {
        type = &typeid(node);
        cached.store(type, std::memory_order_relaxed);
    }
    return type;
}
} // namespace Rux
