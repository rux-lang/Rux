#include "System/Allocator.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <doctest.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace Rux::System;

TEST_CASE("the compiler allocator serves small blocks from its slabs with room for the request") {
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{16}, std::size_t{17}, std::size_t{255},
                                   std::size_t{1000}, std::size_t{4097}, std::size_t{8192}}) {
        void *block = Allocator::Allocate(size);
        REQUIRE(block != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(block) % 16 == 0);
        if (Allocator::OwnsBlock(block))
            CHECK(Allocator::BlockSize(block) >= std::max<std::size_t>(size, 1));
        std::memset(block, 0xAB, size);
        Allocator::Deallocate(block);
    }
    void *large = Allocator::Allocate(100000);
    REQUIRE(large != nullptr);
    CHECK_FALSE(Allocator::OwnsBlock(large));
    Allocator::Deallocate(large);
    Allocator::Deallocate(nullptr);
}

TEST_CASE("the compiler allocator reuses freed blocks and keeps live ones apart") {
    std::vector<void *> blocks;
    for (int index = 0; index < 5000; ++index) {
        auto *block = static_cast<int *>(Allocator::Allocate(48));
        *block = index;
        blocks.push_back(block);
    }
    for (int index = 0; index < 5000; ++index)
        CHECK(*static_cast<int *>(blocks[static_cast<std::size_t>(index)]) == index);
    std::vector<void *> sorted = blocks;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
    for (void *block : blocks)
        Allocator::Deallocate(block);
}

TEST_CASE("blocks freed on another thread are reused without corrupting live ones") {
    constexpr int kThreads = 4;
    constexpr int kBlocks = 20000;
    std::vector<std::vector<std::string *>> produced(kThreads);
    std::vector<std::thread> producers;
    for (int thread = 0; thread < kThreads; ++thread) {
        producers.emplace_back([&, thread] {
            for (int index = 0; index < kBlocks; ++index)
                produced[static_cast<std::size_t>(thread)].push_back(
                    new std::string(std::to_string(thread) + ":" + std::to_string(index) + std::string(40, 'x')));
        });
    }
    for (auto &producer : producers)
        producer.join();
    std::atomic<int> mismatches = 0;
    std::vector<std::thread> consumers;
    for (int thread = 0; thread < kThreads; ++thread) {
        consumers.emplace_back([&, thread] {
            // Each thread frees another thread's blocks and allocates fresh ones of the same class in between.
            auto &mine = produced[static_cast<std::size_t>((thread + 1) % kThreads)];
            std::vector<std::unique_ptr<std::string>> fresh;
            for (std::size_t index = 0; index < mine.size(); ++index) {
                const auto expected = std::to_string((thread + 1) % kThreads) + ":" + std::to_string(index);
                if (mine[index]->rfind(expected, 0) != 0)
                    ++mismatches;
                delete mine[index];
                if (index % 3 == 0)
                    fresh.push_back(std::make_unique<std::string>(std::string(48, static_cast<char>('a' + thread))));
            }
            for (const auto &text : fresh)
                if (*text != std::string(48, static_cast<char>('a' + thread)))
                    ++mismatches;
        });
    }
    for (auto &consumer : consumers)
        consumer.join();
    CHECK(mismatches.load() == 0);
}

TEST_CASE("over-aligned allocations keep their alignment") {
    struct alignas(64) Wide {
        std::uint8_t bytes[64];
    };

    auto wide = std::make_unique<Wide>();
    CHECK(reinterpret_cast<std::uintptr_t>(wide.get()) % 64 == 0);
    auto *array = new Wide[3];
    CHECK(reinterpret_cast<std::uintptr_t>(array) % 64 == 0);
    delete[] array;
}
