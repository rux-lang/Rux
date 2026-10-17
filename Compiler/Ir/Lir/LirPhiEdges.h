#pragma once

#include "Ir/Lir/LirRegs.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace Rux {
/// A phi's incoming values, one (register, predecessor block index) pair per edge. Only phis have edges, so the list
/// lives behind a pointer: every other instruction carries 8 bytes for it rather than an empty vector, and moves it as
/// one pointer. The members are named after std::vector's, whose interface the LIR code relies on, so a list reads and
/// writes like one; iterators are pointers into it and are invalidated as a vector's would be.
class LirPhiEdges {
public:
    using Edge = std::pair<LirReg, std::uint32_t>;
    using value_type = Edge;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = Edge &;
    using const_reference = const Edge &;
    using iterator = Edge *;
    using const_iterator = const Edge *;

    LirPhiEdges() noexcept = default;

    LirPhiEdges(const std::initializer_list<Edge> edges) {
        *this = edges;
    }

    LirPhiEdges(const LirPhiEdges &other)
        : list(other.empty() ? nullptr : std::make_unique<std::vector<Edge>>(*other.list)) {
    }

    LirPhiEdges(LirPhiEdges &&other) noexcept = default;
    ~LirPhiEdges() = default;

    LirPhiEdges &operator=(const LirPhiEdges &other) {
        if (this != &other) {
            if (other.empty()) {
                clear();
            }
            else {
                Edit() = *other.list;
            }
        }
        return *this;
    }

    LirPhiEdges &operator=(LirPhiEdges &&other) noexcept = default;

    LirPhiEdges &operator=(const std::initializer_list<Edge> edges) {
        Edit().assign(edges.begin(), edges.end());
        return *this;
    }

    LirPhiEdges &operator=(const std::vector<Edge> &edges) {
        Edit() = edges;
        return *this;
    }

    LirPhiEdges &operator=(std::vector<Edge> &&edges) {
        Edit() = std::move(edges);
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return list ? list->size() : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return !list || list->empty();
    }

    [[nodiscard]] iterator begin() noexcept {
        return list ? list->data() : nullptr;
    }

    [[nodiscard]] iterator end() noexcept {
        return list ? list->data() + list->size() : nullptr;
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return list ? list->data() : nullptr;
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return list ? list->data() + list->size() : nullptr;
    }

    [[nodiscard]] Edge &operator[](const std::size_t index) noexcept {
        return (*list)[index];
    }

    [[nodiscard]] const Edge &operator[](const std::size_t index) const noexcept {
        return (*list)[index];
    }

    [[nodiscard]] Edge &front() noexcept {
        return list->front();
    }

    [[nodiscard]] const Edge &front() const noexcept {
        return list->front();
    }

    template <typename... Arguments>
    Edge &emplace_back(Arguments &&...arguments) {
        return Edit().emplace_back(std::forward<Arguments>(arguments)...);
    }

    void push_back(const Edge &edge) {
        Edit().push_back(edge);
    }

    void clear() noexcept {
        if (list) {
            list->clear();
        }
    }

    /// Removes every edge `predicate` accepts, as std::erase_if does for a vector, and returns how many it removed.
    template <typename Predicate>
    std::size_t EraseIf(Predicate predicate) {
        return list ? std::erase_if(*list, predicate) : 0;
    }

    friend bool operator==(const LirPhiEdges &left, const LirPhiEdges &right) {
        return std::ranges::equal(left, right);
    }

    friend bool operator==(const LirPhiEdges &left, const std::vector<Edge> &right) {
        return std::ranges::equal(left, right);
    }

private:
    [[nodiscard]] std::vector<Edge> &Edit() {
        if (!list) {
            list = std::make_unique<std::vector<Edge>>();
        }
        return *list;
    }

    std::unique_ptr<std::vector<Edge>> list;
};
} // namespace Rux
