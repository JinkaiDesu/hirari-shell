#include "hirari/shell.hpp"
#include "hirari/installation.hpp"
#include "hirari/update.hpp"
#include "hirari/console.hpp"
#include "hirari/platform.hpp"
#include <iostream>
#include <vector>

namespace {
class ConsoleHost final : public hirari::Host {
public:
    void write(std::string_view bytes) override {
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        if (!std::cout) hirari::console::output_lost();
    }
    void error(std::string_view bytes) override {
        std::cerr.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cerr.flush();
    }
    bool interrupted() override { return hirari::console::interrupted(); }
    bool terminal_capable() const override { return hirari::console::is_terminal(); }
};
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    hirari::console::start();
    ConsoleHost host;
    try {
        hirari::initialize_installation();
        hirari::Shell shell(host);
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            args.push_back(hirari::platform::argument_text(argv[i]));
        }
        if (args.empty()) return shell.run();
        if (args[0] == "--update") return hirari::run_update({args.begin() + 1, args.end()}, HIRARI_VERSION, host);
        if (args.size() == 1 && args[0] == "--version") {
            host.write(std::string("hirari ") + HIRARI_VERSION + "\n");
            return 0;
        }
        constexpr std::string_view usage =
            "usage: hirari [FILE.seed [ARGUMENTS ...] | --file FILE [ARGUMENTS ...] | --eval SOURCE | --update [--check] | --version | --help]\n";
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
            host.write(usage);
            host.write("With no arguments, starts the interactive shell. A file is UTF-8 Seed: each statement runs in\n"
                       "turn and the first failure stops it with FILE:LINE and a nonzero status.\n\n"
                       "HIRARI_PATH   more folders to find .seed commands in\n"
                       "HIRARI_STATE  the folder for saved definitions and history; - keeps none\n\n"
                       "--update installs the latest release over this installation; --check only reports, with status 10 when one is available.\n\n"
                       "Inside the shell, `help` lists what's there and `help --seed` teaches the language.\n");
            return 0;
        }
        // A file's name is a first argument that isn't an option, as a #! line passes it.
        if (!args[0].starts_with("-")) args.insert(args.begin(), "--file");
        if (args.size() < 2 || (args[0] != "--eval" && args[0] != "--file" && args[0] != "--stream-file") || (args[0] == "--eval" && args.size() != 2)) {
            host.error(usage);
            return 2;
        }
        if (args[0] == "--eval") shell.execute(args[1]);
        else {shell.processes().streaming(args[0]=="--stream-file");shell.execute_file(hirari::text_path(args[1]), {args.begin() + 2, args.end()});}
        return shell.status();
    } catch (const hirari::ExitRequest& request) {
        return request.status;
    } catch (const std::exception& error) {
        host.error(std::string("error: ") + error.what() + '\n');
        return 1;
    }
}
