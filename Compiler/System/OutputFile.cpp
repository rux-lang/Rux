#include "System/OutputFile.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <chrono>
#include <cstdlib>

namespace Rux::System {
std::ofstream OpenBinaryOutput(const std::filesystem::path &path,
                               [[maybe_unused]] const std::chrono::milliseconds retryBudget) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
#if RUX_OS_WINDOWS
    const auto deadline = std::chrono::steady_clock::now() + retryBudget;
    while (!output) {
        unsigned long error = 0;
        if (_get_doserrno(&error) != 0 ||
            (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_USER_MAPPED_FILE))
            break;
        std::error_code statusError;
        const bool regular = std::filesystem::is_regular_file(path, statusError);
        if ((!regular && !statusError) || std::chrono::steady_clock::now() >= deadline)
            break;
        Sleep(10);
        output.clear();
        output.open(path, std::ios::binary | std::ios::trunc);
    }
#endif
    return output;
}
} // namespace Rux::System
