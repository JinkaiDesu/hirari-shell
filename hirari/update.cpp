#include "hirari/update.hpp"
#include "hirari/sha256.hpp"
#include "hirari/vendor/json.hpp"
#include "hirari/win32.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#error "self-update has no host layer for this platform yet: see the Platform section of update.cpp"
#endif
namespace hirari {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Version = std::array<int, 3>;

constexpr int status_update_available = 10;
constexpr std::string_view release_host = "https://downloads.emergencetech.co/";
constexpr std::string_view receipt_name = "release.receipt.json";
constexpr std::string_view work_prefix = ".hirari-update-";

[[noreturn]] void refuse(const std::string& why) { throw std::runtime_error("update: " + why); }
std::string text(const fs::path& path) { const auto bytes = path.generic_u8string(); return {bytes.begin(), bytes.end()}; }
fs::path to_path(std::string_view value) { return fs::path(std::u8string(value.begin(), value.end())); }

// =============================================================================
// Platform. A port supplies these and nothing else in this file changes.
// =============================================================================
constexpr std::string_view platform_name = "windows-x64";
constexpr std::string_view latest_manifest = "https://downloads.emergencetech.co/hirari/manifest.json";

fs::path own_executable() {
    std::wstring module(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!count || count >= module.size()) refuse("cannot locate this executable");
    module.resize(count);
    return fs::path(module);
}
// The system's own tools, by full path: a program of the same name earlier on PATH never handles a release.
fs::path system_tool(const wchar_t* name) {
    std::wstring directory(MAX_PATH, L'\0');
    const auto count = GetSystemDirectoryW(directory.data(), MAX_PATH);
    if (!count || count >= MAX_PATH) refuse("cannot locate the system directory");
    directory.resize(count);
    return fs::path(directory) / name;
}
std::string process_id() { return std::to_string(GetCurrentProcessId()); }
std::optional<std::string> environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    if (!count || count >= value.size()) return {};
    value.resize(count);
    return win32::narrow(value);
}
fs::path downloader() { return system_tool(L"curl.exe"); }
fs::path unpacker() { return system_tool(L"tar.exe"); }

struct Ran { int status; std::string output; };
// Runs a program with its stdout and stderr gathered, killing it after the timeout.
Ran run_program(const fs::path& program, const std::vector<std::string>& arguments, const fs::path& output, DWORD timeout_ms) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    win32::Handle sink(CreateFileW(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    win32::Handle nothing(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    if (!sink || !nothing) refuse("cannot open " + text(output) + ": " + win32::windows_error(GetLastError()));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nothing.get();
    startup.hStdOutput = sink.get();
    startup.hStdError = sink.get();
    auto line = win32::make_command_line(program.wstring(), arguments);
    PROCESS_INFORMATION started{};
    if (!CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &started))
        refuse("cannot run " + text(program) + ": " + win32::windows_error(GetLastError()));
    win32::Handle process(started.hProcess), thread(started.hThread);
    if (WaitForSingleObject(process.get(), timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), INFINITE);
        refuse(text(program) + " did not finish in time");
    }
    DWORD status = 1;
    GetExitCodeProcess(process.get(), &status);
    sink.reset();
    std::ifstream in(output, std::ios::binary);
    return {static_cast<int>(status), std::string(std::istreambuf_iterator<char>(in), {})};
}
// Held for the life of the process, whichever way it ends: the system releases and deletes it.
class Lock {
public:
    explicit Lock(const fs::path& file) : handle_(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr)) {
        if (handle_) return;
        const auto error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) refuse("another update of this installation is running");
        refuse("cannot write to " + text(file.parent_path()) + ": " + win32::windows_error(error));
    }
private:
    win32::Handle handle_;
};

// =============================================================================
// The manifest
// =============================================================================
Version parse_version(const std::string& value) {
    Version parts{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto start = at;
        while (at < value.size() && value[at] >= '0' && value[at] <= '9' && at - start < 9) parts[i] = parts[i] * 10 + (value[at++] - '0');
        if (at == start || (i + 1 < parts.size() ? at >= value.size() || value[at++] != '.' : at != value.size()))
            refuse("\"" + value + "\" is not a version");
    }
    return parts;
}
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) refuse("cannot read " + text(path));
    return {std::istreambuf_iterator<char>(in), {}};
}
bool is_sha256(const std::string& value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::string field(const Json& record, const char* name) { return record.at(name).get<std::string>(); }
void fetch(const std::string& url, const fs::path& to, const fs::path& log, const std::string& protocols) {
    const auto ran = run_program(downloader(), {"--silent", "--show-error", "--fail", "--location", "--proto", protocols, "--proto-redir", "=https",
        "--connect-timeout", "30", "--max-time", "900", "--output", text(to), url}, log, 960000);
    if (ran.status) refuse("cannot download " + url + ": " + ran.output);
}
struct Manifest { std::string version, url, sha256; std::uint64_t bytes; };
Manifest read_manifest(const std::string& json, bool pinned_host) {
    try {
        const auto named = Json::parse(json);
        if (field(named, "schema") != "emergence.manifest/1" || field(named, "id") != "hirari") refuse("the manifest does not describe hirari");
        if (field(named, "platform") != platform_name) refuse("the manifest is for " + field(named, "platform") + ", not " + std::string(platform_name));
        const auto& artifact = named.at("artifact");
        Manifest out{field(named, "version"), field(artifact, "url"), field(artifact, "sha256"), artifact.at("bytes").get<std::uint64_t>()};
        parse_version(out.version);
        if (!is_sha256(out.sha256)) refuse("the manifest's sha256 is malformed");
        if (pinned_host && !out.url.starts_with(release_host)) refuse("the manifest names an archive outside " + std::string(release_host));
        return out;
    } catch (const Json::exception& error) {
        refuse(std::string("the manifest is malformed: ") + error.what());
    }
}

// =============================================================================
// Receipts
// =============================================================================
struct Receipt { std::string version; std::set<std::string> files; };
// A receipt only ever names files strictly inside the installation.
void check_relative(const std::string& path) {
    if (path.empty() || path == receipt_name || path.find_first_of("\\:") != std::string::npos) refuse("the receipt names an unsafe path: " + path);
    std::stringstream parts(path);
    for (std::string part; std::getline(parts, part, '/');)
        if (part.empty() || part == "." || part == "..") refuse("the receipt names an unsafe path: " + path);
    if (path.back() == '/') refuse("the receipt names an unsafe path: " + path);
}
Receipt read_receipt(const fs::path& root, bool check_files) {
    try {
        const auto named = Json::parse(read_file(root / receipt_name));
        if (named.at("schemaVersion").get<int>() != 1 || field(named, "name") != "hirari") refuse("the receipt does not describe hirari");
        Receipt out{field(named, "version"), {}};
        for (const auto& file : named.at("files")) {
            const auto path = field(file, "path");
            check_relative(path);
            if (!out.files.insert(path).second) refuse("the receipt names a file twice: " + path);
            if (!check_files) continue;
            const auto bytes = read_file(root / to_path(path));
            if (bytes.size() != file.at("size").get<std::uint64_t>() || sha256(bytes) != field(file, "sha256"))
                refuse("the archive's " + path + " does not match its receipt");
        }
        return out;
    } catch (const Json::exception& error) {
        refuse(std::string("the receipt is malformed: ") + error.what());
    }
}
// Exactly the receipt's files are there: nothing extra, and nothing that isn't a plain file.
void check_only_listed(const fs::path& root, const Receipt& receipt) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink()) refuse("the archive holds a link: " + text(entry.path()));
        if (entry.is_directory()) continue;
        const auto relative = text(fs::relative(entry.path(), root));
        if (relative != receipt_name && (!entry.is_regular_file() || !receipt.files.contains(relative))) refuse("the archive holds a file its receipt does not name: " + relative);
    }
}

// =============================================================================
// Replacing the installation
// =============================================================================
// Both a running executable and the files beside it can be renamed. A failure restores what was there.
void swap_installation(const fs::path& home, const fs::path& incoming, const fs::path& outgoing, const Receipt& current, const Receipt& next) {
    for (const auto& path : next.files)
        if (!current.files.contains(path) && fs::exists(home / to_path(path))) refuse(path + " is already here and is not part of this release; it is left as it is");
    std::vector<fs::path> moved, placed;
    try {
        // The receipt leaves first and arrives last, so an interrupted swap never looks like a whole installation.
        fs::create_directories(outgoing);
        fs::rename(home / receipt_name, outgoing / receipt_name);
        moved.push_back(to_path(receipt_name));
        for (const auto& path : current.files) {
            if (!fs::exists(home / to_path(path))) continue;
            const auto relative = to_path(path);
            fs::create_directories((outgoing / relative).parent_path());
            fs::rename(home / relative, outgoing / relative);
            moved.push_back(relative);
        }
        for (const auto& path : next.files) {
            const auto relative = to_path(path);
            fs::create_directories((home / relative).parent_path());
            fs::rename(incoming / relative, home / relative);
            placed.push_back(relative);
        }
        fs::rename(incoming / receipt_name, home / receipt_name);
        placed.push_back(to_path(receipt_name));
    } catch (const std::exception&) {
        for (auto at = placed.rbegin(); at != placed.rend(); ++at) { std::error_code ignored; fs::remove(home / *at, ignored); }
        for (auto at = moved.rbegin(); at != moved.rend(); ++at) { fs::create_directories((home / *at).parent_path()); fs::rename(outgoing / *at, home / *at); }
        throw;
    }
    // Folders the old release emptied go too; a folder still holding anything stays.
    for (const auto& path : current.files)
        for (auto folder = (home / to_path(path)).parent_path(); folder != home && folder.has_relative_path(); folder = folder.parent_path()) {
            std::error_code stopped;
            if (!fs::remove(folder, stopped)) break;
        }
}
// Work folders of earlier updates. A replaced executable that was still running couldn't be deleted then.
void sweep(const fs::path& home) {
    for (const auto& entry : fs::directory_iterator(home)) {
        const auto name = text(entry.path().filename());
        std::error_code ignored;
        if (name.starts_with(work_prefix) && entry.is_directory()) fs::remove_all(entry.path(), ignored);
    }
}
}

int run_update(const std::vector<std::string>& options, std::string_view installed, Host& host) {
    const bool check = options.size() == 1 && options[0] == "--check";
    if (!options.empty() && !check) {
        host.error("usage: hirari --update [--check]\n");
        return 2;
    }
    const auto executable = own_executable();
    const auto home = executable.parent_path();
    const auto scratch = fs::temp_directory_path() / (std::string(work_prefix) + process_id());
    fs::create_directories(scratch);
    struct Scratch { fs::path path; ~Scratch() { std::error_code ignored; fs::remove_all(path, ignored); } } scratch_guard{scratch};

    // HIRARI_UPDATE_MANIFEST reads another manifest, and only then may its archive be elsewhere.
    const auto override_url = environment(L"HIRARI_UPDATE_MANIFEST");
    const std::string protocols = override_url ? "=https,file" : "=https";
    fetch(override_url.value_or(std::string(latest_manifest)), scratch / "manifest.json", scratch / "log", protocols);
    const auto latest = read_manifest(read_file(scratch / "manifest.json"), !override_url);
    const std::string current(installed);
    if (parse_version(latest.version) <= parse_version(current)) {
        host.write("UP-TO-DATE " + current + "\n");
        return 0;
    }
    if (check) {
        host.write("UPDATE-AVAILABLE " + current + " " + latest.version + "\n");
        return status_update_available;
    }

    if (!fs::exists(home / receipt_name)) refuse(text(home) + " holds no " + std::string(receipt_name) + ": it is not a release installation, or an earlier update was interrupted (see any " + std::string(work_prefix) + "* folder there)");
    const auto present = read_receipt(home, false);
    const auto own_name = text(executable.filename());
    if (!present.files.contains(own_name)) refuse("this installation's receipt does not name " + own_name);
    // A folder named after its version belongs to whatever pinned that version there.
    if (text(home.filename()) == current) refuse(text(home) + " is pinned to its version by its folder; whatever installed it updates it");

    Lock lock(home / (std::string(work_prefix) + "lock"));
    sweep(home);
    const auto work = home / (std::string(work_prefix) + process_id());
    const auto archive = work / "archive";
    const auto incoming = work / "new";
    fs::create_directories(incoming);
    struct Work { fs::path path; ~Work() { std::error_code ignored; fs::remove_all(path, ignored); } } work_guard{work};

    fetch(latest.url, archive, scratch / "log", protocols);
    const auto bytes = read_file(archive);
    if (bytes.size() != latest.bytes || sha256(bytes) != latest.sha256) refuse("the archive does not match the manifest's size and sha256");
    const auto unpacked = run_program(unpacker(), {"-xf", text(archive), "-C", text(incoming)}, scratch / "log", 300000);
    if (unpacked.status) refuse("cannot unpack the archive: " + unpacked.output);
    const auto next = read_receipt(incoming, true);
    if (next.version != latest.version) refuse("the archive's receipt names " + next.version + ", not " + latest.version);
    check_only_listed(incoming, next);
    if (!next.files.contains(own_name)) refuse("the archive holds no " + own_name);

    // The new shell runs from where it will not stay, before anything is replaced.
    const auto reported = run_program(incoming / to_path(own_name), {"--version"}, scratch / "log", 30000);
    if (reported.status || !reported.output.starts_with("hirari " + latest.version)) refuse("the new executable does not report " + latest.version + ": " + reported.output);
    const auto evaluated = run_program(incoming / to_path(own_name), {"--eval", "(1 + 2)"}, scratch / "log", 30000);
    if (evaluated.status || evaluated.output.find('3') == std::string::npos) refuse("the new executable does not evaluate Seed: " + evaluated.output);

    swap_installation(home, incoming, work / "old", present, next);
    host.write("UPDATED " + current + " " + latest.version + "\n");
    return 0;
}
}
