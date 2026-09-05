#include "System/OutputFile.h"
#include "System/WinApi.h"
#include "Target/Platform.h"

#include <chrono>
#include <doctest.h>
#include <future>
#include <iterator>
#include <string>

using namespace Rux::System;

TEST_CASE("binary output replaces an existing artifact and reports missing parents") {
    const auto path = std::filesystem::path(RUX_TEST_BIN_DIR) / "output-file-test.bin";
    { OpenBinaryOutput(path) << "long original content"; }
    { OpenBinaryOutput(path) << "new"; }
    std::ifstream input(path, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(input), {}) == "new");
    input.close();
    CHECK(std::filesystem::remove(path));
    CHECK_FALSE(OpenBinaryOutput(path / "missing-parent" / "output.bin"));
}

#if RUX_OS_WINDOWS
TEST_CASE("binary output waits for a Windows mapped image to be released") {
    const auto path = std::filesystem::path(RUX_TEST_BIN_DIR) / "output-file-mapped.bin";
    { OpenBinaryOutput(path) << "original"; }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    REQUIRE(mapping != nullptr);
    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    REQUIRE(view != nullptr);
    std::promise<void> started;
    auto completion = std::async(std::launch::async, [&] {
        started.set_value();
        auto output = OpenBinaryOutput(path);
        output << "replacement";
        return static_cast<bool>(output);
    });
    started.get_future().wait();
    CHECK(completion.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    UnmapViewOfFile(view);
    CHECK(completion.get());
    CHECK(std::filesystem::remove(path));
}

TEST_CASE("binary output waits for a transient Windows sharing lock") {
    const auto path = std::filesystem::path(RUX_TEST_BIN_DIR) / "output-file-locked.bin";
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    std::promise<void> started;
    auto completion = std::async(std::launch::async, [&] {
        started.set_value();
        auto output = OpenBinaryOutput(path);
        output << "replacement";
        return static_cast<bool>(output);
    });
    started.get_future().wait();
    CHECK(completion.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CloseHandle(locked);
    CHECK(completion.get());
    CHECK(std::filesystem::remove(path));
}

TEST_CASE("binary output stops retrying a persistent Windows sharing lock") {
    const auto path = std::filesystem::path(RUX_TEST_BIN_DIR) / "output-file-persistent-lock.bin";
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(locked != INVALID_HANDLE_VALUE);
    const bool opened = static_cast<bool>(OpenBinaryOutput(path, std::chrono::milliseconds(50)));
    CloseHandle(locked);
    CHECK_FALSE(opened);
    CHECK(std::filesystem::remove(path));
}
#endif
