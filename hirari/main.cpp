#include "hirari/shell.hpp"
#include "hirari/installation.hpp"
#include <iostream>
#include <atomic>
#include <vector>
#include "hirari/win32.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#endif

namespace {
#ifdef _WIN32
std::atomic_bool interrupted{false};
BOOL WINAPI console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true);
    return TRUE;
}
#endif

class ConsoleHost final : public hirari::Host {
public:
    void write(std::string_view bytes) override {
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
    }
    void error(std::string_view bytes) override {
        std::cerr.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cerr.flush();
    }
    bool interrupted() override {
#ifdef _WIN32
        return ::interrupted.exchange(false);
#else
        return false;
#endif
    }
    bool terminal_capable() const override {
#ifdef _WIN32
        DWORD mode;
        return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
               GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
#else
        return false;
#endif
    }
};
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleCtrlHandler(console_control, TRUE);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    DWORD mode;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(output, &mode))
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    ConsoleHost host;
    try {
        hirari::initialize_installation();
        hirari::Shell shell(host);
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
#ifdef _WIN32
            args.push_back(hirari::win32::narrow(argv[i]));
#else
            args.emplace_back(argv[i]);
#endif
        }
        if (args.empty()) return shell.run();
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
            host.write("Usage: hirari [--eval SOURCE | --file PATH [arguments]]\n"
                       "No arguments starts the interactive shell. Scripts are UTF-8 Seed expressions; open groups and leading-pipe lines continue the current expression.\n");
            return 0;
        }
        if (args.size() < 2 || (args[0] != "--eval" && args[0] != "--file" && args[0] != "--stream-file") || (args[0] == "--eval" && args.size() != 2)) {
            host.error("usage: hirari [--eval SOURCE | --file PATH [arguments]]\n");
            return 2;
        }
        if (args[0] == "--eval") shell.execute(args[1]);
        else {shell.processes().streaming(args[0]=="--stream-file");shell.execute_file(hirari::text_path(args[1]), {args.begin() + 2, args.end()});}
        return shell.status();
    } catch (const std::exception& error) {
        host.error(std::string("error: ") + error.what() + '\n');
        return 1;
    }
}
