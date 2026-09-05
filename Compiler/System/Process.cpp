#include "System/Process.h"

#include "System/WinApi.h"
#include "Target/Platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <mutex>
#include <vector>

#if !RUX_OS_WINDOWS
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char **environ;
#endif

namespace Rux::System {
namespace {
#if RUX_OS_WINDOWS
class Handle {
public:
    explicit Handle(HANDLE input = nullptr)
        : value(input) {
    }

    ~Handle() {
        Reset();
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    HANDLE Get() const {
        return value;
    }

    bool Valid() const {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE input = nullptr) {
        if (Valid())
            CloseHandle(value);
        value = input;
    }

private:
    HANDLE value;
};

// Quote argv according to the Windows C runtime rules, including quotes and trailing backslashes.
std::string CommandLine(const std::filesystem::path &exe, const std::span<const std::string_view> args) {
    std::string result;
    auto Append = [&](const std::string_view argument) {
        if (!result.empty())
            result += ' ';
        result += '"';
        std::size_t slashes = 0;
        for (const char ch : argument) {
            if (ch == '\\') {
                ++slashes;
                continue;
            }
            result.append(ch == '"' ? slashes * 2 + 1 : slashes, '\\');
            result += ch;
            slashes = 0;
        }
        result.append(slashes * 2, '\\');
        result += '"';
    };
    Append(exe.string());
    for (const auto argument : args)
        Append(argument);
    return result;
}

bool Fail(const DWORD error, std::error_code *destination) {
    if (destination != nullptr)
        *destination = {static_cast<int>(error), std::system_category()};
    return false;
}

// All launch sites use an explicit handle list. Another thread's inheritable capture pipe must never reach this child.
bool Launch(const std::filesystem::path &exe, const std::span<const std::string_view> args,
            const std::array<HANDLE, 3> &streams, PROCESS_INFORMATION &process, std::error_code *error) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        return Fail(GetLastError(), error);
    std::vector<HANDLE> inherited;
    for (HANDLE stream : streams) {
        if (std::find(inherited.begin(), inherited.end(), stream) == inherited.end())
            inherited.push_back(stream);
    }
    const bool updated = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                                   inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    STARTUPINFOEXA startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = streams[0];
    startup.StartupInfo.hStdOutput = streams[1];
    startup.StartupInfo.hStdError = streams[2];
    startup.lpAttributeList = attributes;
    std::string command = CommandLine(exe, args);
    const bool launched =
        updated && CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                                  nullptr, nullptr, &startup.StartupInfo, &process) != FALSE;
    const DWORD nativeError = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    return launched || Fail(nativeError, error);
}

int Wait(HANDLE process) {
    WaitForSingleObject(process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process, &code);
    return static_cast<int>(code);
}
#else
// A portable pipe()+fcntl() pair must be atomic with respect to every launch in this component. Child execution and
// capture run outside the lock.
std::mutex launchMutex;

bool Fail(const int error, std::error_code *destination) {
    if (destination != nullptr)
        *destination = {error, std::generic_category()};
    return false;
}

// The descriptors a child receives: the standard streams its launch site selects and nothing else, mirroring the
// explicit Windows handle list. Artifact streams are not opened close-on-exec, so an inherited descriptor could
// otherwise keep a freshly linked executable open for writing while another worker launches it (ETXTBSY), or outlive
// the compiler inside a long-running `rux run` child.
class ChildDescriptors {
public:
    ChildDescriptors() {
        error = posix_spawn_file_actions_init(&actions);
        if (error != 0)
            return;
        actionsReady = true;
        error = posix_spawnattr_init(&attributes);
        attributesReady = error == 0;
    }

    ~ChildDescriptors() {
        if (attributesReady)
            posix_spawnattr_destroy(&attributes);
        if (actionsReady)
            posix_spawn_file_actions_destroy(&actions);
    }

    ChildDescriptors(const ChildDescriptors &) = delete;
    ChildDescriptors &operator=(const ChildDescriptors &) = delete;

    void Open(const int fd, const char *path, const int flags) {
        Apply([&] { return posix_spawn_file_actions_addopen(&actions, fd, path, flags, 0); });
    }

    void Duplicate(const int from, const int to) {
        Apply([&] { return posix_spawn_file_actions_adddup2(&actions, from, to); });
    }

    // Keep a standard stream the parent has open; one the parent closed stays closed in the child.
    void Inherit([[maybe_unused]] const int fd) {
    #if RUX_OS_MACOS
        if (fcntl(fd, F_GETFD) >= 0)
            Apply([&] { return posix_spawn_file_actions_addinherit_np(&actions, fd); });
    #endif
    }

    // Close every descriptor the actions above did not select.
    void CloseOthers() {
    #if RUX_OS_MACOS
        // Darwin has no closefrom; this attribute closes whatever the file actions do not describe.
        Apply([&] { return posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT); });
    #else
        Apply([&] { return posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1); });
    #endif
    }

    int Error() const {
        return error;
    }

    const posix_spawn_file_actions_t *Actions() const {
        return &actions;
    }

    const posix_spawnattr_t *Attributes() const {
        return &attributes;
    }

private:
    void Apply(const auto &action) {
        if (error == 0)
            error = action();
    }

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actionsReady = false;
    bool attributesReady = false;
    int error = 0;
};

bool Launch(const std::filesystem::path &exe, const std::span<const std::string_view> args,
            const ChildDescriptors &descriptors, pid_t &pid, std::error_code *error) {
    std::vector<std::string> strings{exe.string()};
    for (const auto argument : args)
        strings.emplace_back(argument);
    std::vector<char *> argv;
    for (auto &argument : strings)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    // spawnp resolves a bare tool name through PATH, without a shell or allocator work in a forked child.
    const int status = posix_spawnp(&pid, strings.front().c_str(), descriptors.Actions(), descriptors.Attributes(),
                                    argv.data(), environ);
    return status == 0 || Fail(status, error);
}

int Wait(const pid_t pid) {
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    }
    while (result < 0 && errno == EINTR);
    return result >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif
} // namespace

std::optional<int> RunInherited(const std::filesystem::path &exe, const std::span<const std::string_view> args,
                                std::error_code *launchError) {
    if (launchError != nullptr)
        launchError->clear();
#if RUX_OS_WINDOWS
    std::array<Handle, 3> owned;
    std::array<HANDLE, 3> streams{};
    constexpr std::array<DWORD, 3> names{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (std::size_t i = 0; i < streams.size(); ++i) {
        const HANDLE original = GetStdHandle(names[i]);
        HANDLE copy = nullptr;
        if (original != nullptr && original != INVALID_HANDLE_VALUE) {
            if (!DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &copy, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS)) {
                Fail(GetLastError(), launchError);
                return std::nullopt;
            }
        }
        else {
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            copy = CreateFileA("NUL", i == 0 ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (copy == INVALID_HANDLE_VALUE) {
                Fail(GetLastError(), launchError);
                return std::nullopt;
            }
        }
        owned[i].Reset(copy);
        streams[i] = copy;
    }
    PROCESS_INFORMATION process{};
    if (!Launch(exe, args, streams, process, launchError))
        return std::nullopt;
    const Handle processHandle(process.hProcess);
    const Handle threadHandle(process.hThread);
    return Wait(processHandle.Get());
#else
    pid_t pid;
    {
        const std::lock_guard lock(launchMutex);
        ChildDescriptors descriptors;
        constexpr std::array<int, 3> standard{STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
        for (const int fd : standard)
            descriptors.Inherit(fd);
        descriptors.CloseOthers();
        if (descriptors.Error() != 0) {
            Fail(descriptors.Error(), launchError);
            return std::nullopt;
        }
        if (!Launch(exe, args, descriptors, pid, launchError))
            return std::nullopt;
    }
    return Wait(pid);
#endif
}

std::optional<RunResult> RunCaptured(const std::filesystem::path &exe, const std::span<const std::string_view> args,
                                     std::error_code *launchError) {
    if (launchError != nullptr)
        launchError->clear();
#if RUX_OS_WINDOWS
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &security, 0)) {
        Fail(GetLastError(), launchError);
        return std::nullopt;
    }
    Handle readPipe(read);
    Handle writePipe(write);
    if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) {
        Fail(GetLastError(), launchError);
        return std::nullopt;
    }
    const Handle input(CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.Valid()) {
        Fail(GetLastError(), launchError);
        return std::nullopt;
    }
    PROCESS_INFORMATION process{};
    if (!Launch(exe, args, {input.Get(), write, write}, process, launchError))
        return std::nullopt;
    const Handle processHandle(process.hProcess);
    const Handle threadHandle(process.hThread);
    writePipe.Reset();
    RunResult result;
    char buffer[4096];
    DWORD count = 0;
    while (ReadFile(readPipe.Get(), buffer, sizeof(buffer), &count, nullptr) && count > 0)
        result.output.append(buffer, count);
    result.exitCode = Wait(processHandle.Get());
    return result;
#else
    pid_t pid;
    int fds[2];
    {
        const std::lock_guard lock(launchMutex);
        if (pipe(fds) != 0) {
            Fail(errno, launchError);
            return std::nullopt;
        }
        // Move pipes above the standard descriptors even when our caller has closed one of its streams.
        for (int &fd : fds) {
            const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            const int error = errno;
            if (copy < 0) {
                close(fds[0]);
                close(fds[1]);
                Fail(error, launchError);
                return std::nullopt;
            }
            close(fd);
            fd = copy;
        }
        ChildDescriptors descriptors;
        descriptors.Open(STDIN_FILENO, "/dev/null", O_RDONLY);
        descriptors.Duplicate(fds[1], STDOUT_FILENO);
        descriptors.Duplicate(fds[1], STDERR_FILENO);
        descriptors.CloseOthers();
        const bool launched = descriptors.Error() == 0 && Launch(exe, args, descriptors, pid, launchError);
        close(fds[1]);
        if (!launched) {
            close(fds[0]);
            if (descriptors.Error() != 0)
                Fail(descriptors.Error(), launchError);
            return std::nullopt;
        }
    }
    RunResult result;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count > 0)
            result.output.append(buffer, static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    close(fds[0]);
    result.exitCode = Wait(pid);
    return result;
#endif
}
} // namespace Rux::System
