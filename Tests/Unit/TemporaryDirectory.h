#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Rux::Testing {
/// A directory of this test process's own under the system's temporary directory, for files tests write and remove.
///
/// Tests name their files with fixed names, which two test processes running at once would share: one could replace
/// or delete the other's executable between its link and its run. Each process reserves a fresh directory atomically
/// the first time it asks, so the fixed names only have to be distinct within one process. The directory belongs to
/// this process alone, so it is removed at exit with whatever the tests left in it.
inline const std::filesystem::path &TestTemporaryDirectory() {
    struct Reserved {
        std::filesystem::path path;

        Reserved() {
            static std::atomic_uint64_t sequence = 0;
            const std::filesystem::path base = std::filesystem::temp_directory_path();
            for (unsigned attempt = 0; attempt < 100; ++attempt) {
                const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
                path = base / ("rux-unit-" + std::to_string(stamp) + "-" + std::to_string(++sequence));
                std::error_code error;
                if (std::filesystem::create_directory(path, error)) {
                    return;
                }
            }
            throw std::runtime_error("could not reserve a temporary directory for the unit tests");
        }

        Reserved(const Reserved &) = delete;
        Reserved &operator=(const Reserved &) = delete;

        ~Reserved() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
    static const Reserved reserved;
    return reserved.path;
}
} // namespace Rux::Testing
