#include "hirari/platform.hpp"
#include "hirari/win32.hpp"

#include <cwchar>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace hirari::platform {
namespace fs = std::filesystem;
using namespace win32;

namespace {
std::wstring search_path_once(
    std::wstring_view name,
    const wchar_t* extension
) {
    std::vector<wchar_t>
        buffer(32768);

    const std::wstring owned(
        name
    );

    // Search the caller's PATH explicitly. NULL uses Windows' registry-dependent
    // system order, which can select System32/sort.exe ahead of bundled tools.
    DWORD capacity = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring environment_path;
    while (capacity != 0) {
        std::vector<wchar_t> path_buffer(capacity);
        DWORD count = GetEnvironmentVariableW(L"PATH", path_buffer.data(), capacity);
        if (count == 0) break;
        if (count >= capacity) { capacity = count + 1; continue; }
        environment_path.assign(path_buffer.data(), count);
        break;
    }
    const wchar_t* lookup_path = environment_path.empty() ? nullptr : environment_path.c_str();

    DWORD length =
        SearchPathW(
            lookup_path,
            owned.c_str(),
            extension,
            static_cast<DWORD>(
                buffer.size()
            ),
            buffer.data(),
            nullptr
        );

    if (length == 0) {
        return {};
    }

    if (
        length >=
        buffer.size()
    ) {
        buffer.resize(
            static_cast<std::size_t>(
                length
            ) + 1
        );

        length =
            SearchPathW(
                lookup_path,
                owned.c_str(),
                extension,
                static_cast<DWORD>(
                    buffer.size()
                ),
                buffer.data(),
                nullptr
            );

        if (
            length == 0 ||
            length >=
                buffer.size()
        ) {
            return {};
        }
    }

    auto result = std::wstring(
        buffer.data(),
        length
    );
    // SearchPath can return a directory (for example PowerShell's "tr" locale).
    // It is not a launchable extensionless program; continue with .exe lookup.
    DWORD attributes = GetFileAttributesW(result.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return {};
    return result;
}

std::wstring find_program(
    std::string_view name
) {
    const std::wstring wide =
        widen(
            name
        );

    if (
        std::wstring path =
            search_path_once(
                wide,
                nullptr
            );
        !path.empty()
    ) {
        return path;
    }

    if (
        std::wstring path =
            search_path_once(
                wide,
                L".exe"
            );
        !path.empty()
    ) {
        return path;
    }

    return {};
}

fs::path system_directory() {
    std::wstring directory(MAX_PATH, L'\0');
    const auto count = GetSystemDirectoryW(directory.data(), MAX_PATH);
    if (!count || count >= MAX_PATH) return {};
    directory.resize(count);
    return fs::path(directory);
}
}

std::string argument_text(ArgumentText argument) { return narrow(argument); }
fs::path native_path(const std::string& text) { return fs::path(widen(text)); }
void require_text(std::string_view text) { (void)widen(text); }

std::optional<std::string> environment(const std::string& name) {
    const auto key = widen(name);
    SetLastError(ERROR_SUCCESS);
    DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (!size) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string{};
    }
    std::wstring buffer(size, L'\0');
    DWORD count = GetEnvironmentVariableW(key.c_str(), buffer.data(), size);
    if (count >= size) throw std::runtime_error("environment variable changed while reading: " + name);
    buffer.resize(count);
    return narrow(buffer);
}

void set_environment(const std::string& name, const std::optional<std::string>& value) {
    // _wputenv_s keeps the C runtime and the Win32 environment block in step.
    if (_wputenv_s(widen(name).c_str(), value ? widen(*value).c_str() : L"") != 0)
        throw std::runtime_error("cannot set environment variable: " + name);
}

fs::path own_executable() {
    std::wstring module(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size()) return {};
    module.resize(count);
    return fs::path(module);
}

std::string home_directory() { return environment("USERPROFILE").value_or(std::string{}); }

fs::path default_state_directory() {
    auto local = environment("LOCALAPPDATA").value_or(std::string{});
    return local.empty() ? fs::path{} : native_path(local) / "Hirari" / "vocabulary";
}

unsigned long process_id() { return GetCurrentProcessId(); }

std::string find_executable(std::string_view name) { return narrow(find_program(name)); }

std::string program_name(const fs::directory_entry& entry) {
    auto extension = entry.path().extension().wstring();
    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
    if (extension == L".exe" || extension == L".cmd" || extension == L".bat" || extension == L".com") {
        const auto stem = entry.path().stem().u8string();
        return std::string(reinterpret_cast<const char*>(stem.data()), stem.size());
    }
    return {};
}

fs::path system_tool(const char* name) {
    auto folder = system_directory();
    if (folder.empty()) return {};
    return folder / (widen(name) + L".exe");
}

Ran run_program(const fs::path& program, const std::vector<std::string>& arguments, const fs::path& output, unsigned timeout_ms) {
    const auto shown = [](const fs::path& path) { const auto text = path.generic_u8string(); return std::string(text.begin(), text.end()); };
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle sink(CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle nothing(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    if (!sink || !nothing) throw std::runtime_error("cannot open " + shown(output) + ": " + windows_error(GetLastError()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nothing.get();
    startup.hStdOutput = sink.get();
    startup.hStdError = sink.get();
    auto line = make_command_line(program.wstring(), arguments);
    PROCESS_INFORMATION started{};
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &started))
        throw std::runtime_error("cannot run " + shown(program) + ": " + windows_error(GetLastError()));
    Handle process(started.hProcess), thread(started.hThread);
    if (WaitForSingleObject(process.get(), timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), INFINITE);
        throw std::runtime_error(shown(program) + " did not finish in time");
    }
    DWORD status = 1;
    GetExitCodeProcess(process.get(), &status);
    sink.reset();
    std::ifstream in(output, std::ios::binary);
    return {static_cast<int>(status), std::string(std::istreambuf_iterator<char>(in), {})};
}

struct HeldFile::Impl { Handle handle; };
HeldFile::HeldFile() : impl_(std::make_unique<Impl>()) {}
HeldFile::~HeldFile() = default;
bool HeldFile::acquire(const fs::path& file, std::string& failure) {
    impl_->handle = Handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (impl_->handle) return true;
    const auto error = GetLastError();
    if (error != ERROR_SHARING_VIOLATION) failure = windows_error(error);
    return false;
}

} // namespace hirari::platform
