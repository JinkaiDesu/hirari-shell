#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#include <utility>
namespace hirari::win32 {
// =============================================================================
// UTF
// =============================================================================

inline std::wstring widen(
    std::string_view text
) {
    if (text.find('\0') != std::string_view::npos) throw std::runtime_error("NUL is not valid in a Windows argument");
    if (text.empty()) {
        return {};
    }

    const int size =
        MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()
            ),
            nullptr,
            0
        );

    if (size <= 0) {
        throw std::runtime_error(
            "invalid UTF-8"
        );
    }

    std::wstring result(
        static_cast<std::size_t>(
            size
        ),
        L'\0'
    );

    if (
        !MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()
            ),
            result.data(),
            size
        )
    ) {
        throw std::runtime_error(
            "UTF-8 conversion failed"
        );
    }

    return result;
}

inline std::string narrow(
    std::wstring_view text
) {
    if (text.empty()) {
        return {};
    }

    const int size =
        WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()
            ),
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (size <= 0) {
        throw std::runtime_error(
            "invalid UTF-16"
        );
    }

    std::string result(
        static_cast<std::size_t>(
            size
        ),
        '\0'
    );

    if (
        !WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(
                text.size()
            ),
            result.data(),
            size,
            nullptr,
            nullptr
        )
    ) {
        throw std::runtime_error(
            "UTF-16 conversion failed"
        );
    }

    return result;
}

// =============================================================================
// Errors
// =============================================================================

inline std::string windows_error(
    DWORD error
) {
    wchar_t* message =
        nullptr;

    const DWORD length =
        FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            0,
            reinterpret_cast<wchar_t*>(
                &message
            ),
            0,
            nullptr
        );

    if (
        !length ||
        !message
    ) {
        return
            "Windows error " +
            std::to_string(
                error
            );
    }

    std::wstring text(
        message,
        length
    );

    LocalFree(
        message
    );

    while (
        !text.empty() &&
        (
            text.back() == L'\r' ||
            text.back() == L'\n'
        )
    ) {
        text.pop_back();
    }

    return narrow(
        text
    );
}

// =============================================================================
// HANDLE
// =============================================================================

class Handle {
public:
    Handle() = default;

    explicit Handle(
        HANDLE value
    )
        : value_(
            value
        ) {}

    ~Handle() {
        reset();
    }

    Handle(
        const Handle&
    ) = delete;

    Handle& operator=(
        const Handle&
    ) = delete;

    Handle(
        Handle&& other
    ) noexcept
        : value_(
            other.release()
        ) {}

    Handle& operator=(
        Handle&& other
    ) noexcept {
        if (
            this != &other
        ) {
            reset(
                other.release()
            );
        }

        return *this;
    }

    HANDLE get() const noexcept {
        return value_;
    }

    HANDLE release() noexcept {
        HANDLE result =
            value_;

        value_ =
            nullptr;

        return result;
    }

    void reset(
        HANDLE value = nullptr
    ) noexcept {
        if (
            value_ &&
            value_ !=
                INVALID_HANDLE_VALUE
        ) {
            CloseHandle(
                value_
            );
        }

        value_ =
            value;
    }

    explicit operator bool() const noexcept {
        return
            value_ &&
            value_ !=
                INVALID_HANDLE_VALUE;
    }

private:
    HANDLE value_ =
        nullptr;
};

// =============================================================================
// argv quoting
// =============================================================================

inline std::wstring quote_argument(
    std::wstring_view argument
) {
    // Standard Windows escaping preserves the same argv for native programs.
    std::wstring out;

    out.push_back(
        L'"'
    );

    std::size_t slashes =
        0;

    for (
        wchar_t c :
        argument
    ) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }

        if (c == L'"') {
            out.append(
                slashes * 2 + 1,
                L'\\'
            );

            out.push_back(
                L'"'
            );

            slashes = 0;
            continue;
        }

        out.append(
            slashes,
            L'\\'
        );

        slashes = 0;

        out.push_back(
            c
        );
    }

    out.append(
        slashes * 2,
        L'\\'
    );

    out.push_back(
        L'"'
    );

    return out;
}

// Words made only of these stay bare, because cmd.exe misreads a quoted "/c".
// Everything else is quoted: MSYS/Cygwin treats bare apostrophes and globs as syntax.
inline bool bare_argument(
    std::wstring_view argument
) {
    if (argument.empty()) {
        return false;
    }

    for (
        wchar_t c :
        argument
    ) {
        const bool alphanumeric =
            (c >= L'a' && c <= L'z') ||
            (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9');

        if (
            !alphanumeric &&
            std::wstring_view(L"_-./:=+,@%\\").find(c) == std::wstring_view::npos
        ) {
            return false;
        }
    }

    return true;
}

inline std::wstring make_command_line(
    const std::wstring& executable,
    const std::vector<
        std::string
    >& arguments
) {
    std::wstring result =
        quote_argument(
            executable
        );

    for (
        const std::string& argument :
        arguments
    ) {
        result.push_back(
            L' '
        );

        const std::wstring wide =
            widen(
                argument
            );

        result +=
            bare_argument(wide)
                ? wide
                : quote_argument(wide);
    }

    return result;
}


} // namespace hirari::win32
#endif
