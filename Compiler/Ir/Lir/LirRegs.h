#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Rux {
// Virtual register
using LirReg = std::uint32_t;
constexpr LirReg LirNoReg = ~LirReg{};

/// The source registers of one LIR instruction. Almost every instruction reads at most four, and those live inside the
/// instruction itself; only a longer list, such as a call's arguments, goes to the heap. Building, copying and moving
/// an instruction then allocates nothing for its sources, and reading them touches no other memory. The members are
/// named after std::vector's, whose interface the LIR code relies on, so a list reads and writes like one.
class LirRegs {
public:
    using value_type = LirReg;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = LirReg &;
    using const_reference = const LirReg &;
    using pointer = LirReg *;
    using const_pointer = const LirReg *;
    using iterator = LirReg *;
    using const_iterator = const LirReg *;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    static constexpr std::uint32_t InlineCapacity = 4;

    LirRegs() noexcept = default;

    LirRegs(const std::initializer_list<LirReg> regs) {
        assign(regs.begin(), regs.end());
    }

    template <std::forward_iterator Iterator>
    LirRegs(const Iterator first, const Iterator last) {
        assign(first, last);
    }

    explicit LirRegs(const std::vector<LirReg> &regs) {
        assign(regs.begin(), regs.end());
    }

    LirRegs(const LirRegs &other) {
        assign(other.begin(), other.end());
    }

    LirRegs(LirRegs &&other) noexcept {
        Take(other);
    }

    ~LirRegs() {
        Release();
    }

    LirRegs &operator=(const LirRegs &other) {
        if (this != &other) {
            assign(other.begin(), other.end());
        }
        return *this;
    }

    LirRegs &operator=(LirRegs &&other) noexcept {
        if (this != &other) {
            Release();
            Take(other);
        }
        return *this;
    }

    LirRegs &operator=(const std::initializer_list<LirReg> regs) {
        assign(regs.begin(), regs.end());
        return *this;
    }

    LirRegs &operator=(const std::vector<LirReg> &regs) {
        assign(regs.begin(), regs.end());
        return *this;
    }

    /// A vector's registers are copied: the list keeps its own storage, so there is nothing to take over.
    LirRegs &operator=(std::vector<LirReg> &&regs) {
        assign(regs.begin(), regs.end());
        return *this;
    }

    template <std::forward_iterator Iterator>
    void assign(const Iterator first, const Iterator last) {
        const auto size = static_cast<std::size_t>(std::distance(first, last));
        count = 0;
        reserve(size);
        std::copy(first, last, data());
        count = static_cast<std::uint32_t>(size);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count;
    }

    [[nodiscard]] bool empty() const noexcept {
        return count == 0;
    }

    [[nodiscard]] LirReg *data() noexcept {
        return OnHeap() ? storage.heap : storage.local;
    }

    [[nodiscard]] const LirReg *data() const noexcept {
        return OnHeap() ? storage.heap : storage.local;
    }

    [[nodiscard]] iterator begin() noexcept {
        return data();
    }

    [[nodiscard]] iterator end() noexcept {
        return data() + count;
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return data() + count;
    }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        return begin();
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        return end();
    }

    [[nodiscard]] reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    [[nodiscard]] reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    [[nodiscard]] const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    [[nodiscard]] LirReg &operator[](const std::size_t index) noexcept {
        return data()[index];
    }

    [[nodiscard]] const LirReg &operator[](const std::size_t index) const noexcept {
        return data()[index];
    }

    [[nodiscard]] LirReg &at(const std::size_t index) {
        CheckIndex(index);
        return data()[index];
    }

    [[nodiscard]] const LirReg &at(const std::size_t index) const {
        CheckIndex(index);
        return data()[index];
    }

    [[nodiscard]] LirReg &front() noexcept {
        return data()[0];
    }

    [[nodiscard]] const LirReg &front() const noexcept {
        return data()[0];
    }

    [[nodiscard]] LirReg &back() noexcept {
        return data()[count - 1];
    }

    [[nodiscard]] const LirReg &back() const noexcept {
        return data()[count - 1];
    }

    void reserve(const std::size_t wanted) {
        if (wanted <= capacity) {
            return;
        }
        const std::size_t grown = std::max<std::size_t>(wanted, std::size_t{capacity} * 2);
        auto *heap = new LirReg[grown];
        std::copy_n(data(), count, heap);
        Release();
        storage.heap = heap;
        capacity = static_cast<std::uint32_t>(grown);
    }

    void push_back(const LirReg reg) {
        if (count == capacity) {
            reserve(std::size_t{count} + 1);
        }
        data()[count++] = reg;
    }

    LirReg &emplace_back(const LirReg reg) {
        push_back(reg);
        return back();
    }

    void pop_back() noexcept {
        --count;
    }

    iterator insert(const const_iterator position, const LirReg reg) {
        const auto index = position - begin();
        push_back(reg);
        std::rotate(begin() + index, end() - 1, end());
        return begin() + index;
    }

    iterator erase(const const_iterator position) {
        return erase(position, position + 1);
    }

    iterator erase(const const_iterator first, const const_iterator last) {
        const auto index = first - begin();
        const auto removed = last - first;
        std::copy(begin() + index + removed, end(), begin() + index);
        count -= static_cast<std::uint32_t>(removed);
        return begin() + index;
    }

    void resize(const std::size_t size, const LirReg fill = 0) {
        reserve(size);
        if (size > count) {
            std::fill(data() + count, data() + size, fill);
        }
        count = static_cast<std::uint32_t>(size);
    }

    void clear() noexcept {
        count = 0;
    }

    friend bool operator==(const LirRegs &left, const LirRegs &right) noexcept {
        return std::equal(left.begin(), left.end(), right.begin(), right.end());
    }

    friend bool operator==(const LirRegs &left, const std::vector<LirReg> &right) noexcept {
        return std::equal(left.begin(), left.end(), right.begin(), right.end());
    }

private:
    union Storage {
        LirReg local[InlineCapacity]{};
        LirReg *heap;
    };

    [[nodiscard]] bool OnHeap() const noexcept {
        return capacity > InlineCapacity;
    }

    void CheckIndex(const std::size_t index) const {
        if (index >= count) {
            throw std::out_of_range("LirRegs index out of range");
        }
    }

    /// Frees the heap block, if any. The caller sets storage and capacity again.
    void Release() const noexcept {
        if (OnHeap()) {
            delete[] storage.heap;
        }
    }

    /// Takes other's registers, the heap block included, and leaves it empty and inline.
    void Take(LirRegs &other) noexcept {
        storage = other.storage;
        count = std::exchange(other.count, 0);
        capacity = std::exchange(other.capacity, InlineCapacity);
    }

    Storage storage;
    std::uint32_t count = 0;
    std::uint32_t capacity = InlineCapacity;
};
} // namespace Rux
