#include "ProcessProbe.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Rux::Testing {
std::optional<int> RunProcessProbe(const int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--rux-process-probe") {
        if (std::getchar() != EOF)
            return 98;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            std::fprintf(stdout, "%zu:", argument.size());
            std::fwrite(argument.data(), 1, argument.size(), stdout);
            std::fflush(stdout);
            std::fputs(";", stderr);
            std::fflush(stderr);
        }
        // More than a pipe's capacity, so capture must drain while the child is running.
        const std::string payload(128 * 1024, 'x');
        std::fwrite(payload.data(), 1, payload.size(), stdout);
        return 7;
    }
#if RUX_OS_WINDOWS
    if (argc == 3 && std::string_view(argv[1]) == "--rux-handle-probe") {
        const auto value = static_cast<std::uintptr_t>(std::strtoull(argv[2], nullptr, 10));
        DWORD flags = 0;
        return GetHandleInformation(reinterpret_cast<HANDLE>(value), &flags) ? 1 : 0;
    }
#endif
    return std::nullopt;
}
} // namespace Rux::Testing
