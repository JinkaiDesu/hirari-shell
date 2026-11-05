// `hirari --update` replaces a real installation with a real newer shell, read from local manifests, and leaves the
// installation exactly as it was whenever anything about the release is wrong.
#include "hirari/sha256.hpp"
#include "hirari/vendor/json.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace hirari;

#ifdef _WIN32
const std::string platform_name = "windows-x64", other_platform = "linux-x64", archive_name = ".zip";
#else
const std::string platform_name = "linux-x64", other_platform = "windows-x64", archive_name = ".tar.gz";
#endif
const std::string shell_name = platform::shell_file_name;

int passed = 0;
fs::path root;

void check(bool okay, const std::string& name) {
    if (!okay) throw std::runtime_error("FAIL: " + name);
    ++passed;
    std::cout << "PASS " << name << '\n';
}
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
void text(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << value;
}
std::string relative_text(const fs::path& path, const fs::path& base) {
    const auto bytes = fs::relative(path, base).generic_u8string();
    return {bytes.begin(), bytes.end()};
}
void write_receipt(const fs::path& folder, const std::string& version, const std::vector<std::string>& extra = {}) {
    std::map<std::string, Json> files;
    for (const auto& entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file() || entry.path().filename() == "release.receipt.json") continue;
        const auto bytes = slurp(entry.path());
        const auto path = relative_text(entry.path(), folder);
        files[path] = Json{{"path", path}, {"size", bytes.size()}, {"sha256", sha256(bytes)}};
    }
    Json listed = Json::array();
    for (auto& [path, record] : files) listed.push_back(record);
    for (const auto& path : extra) listed.push_back(Json{{"path", path}, {"size", 0}, {"sha256", ""}});
    text(folder / "release.receipt.json", Json{{"schemaVersion", 1}, {"name", "hirari"}, {"version", version}, {"files", listed}}.dump(2));
}
fs::path new_install(const std::string& name, const std::string& version, const fs::path& current) {
    const auto folder = root / text_path(name);
    fs::create_directories(folder);
    fs::copy_file(current, folder / shell_name);
    text(folder / "Commands" / "old.seed", "\"old\"");
    text(folder / "Gone" / "only.txt", "the old release alone has this folder");
    write_receipt(folder, version);
    text(folder / "mine.txt", "not part of any release");
    return folder;
}
std::string file_url(const fs::path& path) {
    const auto bytes = fs::absolute(path).generic_u8string();
    std::string url = "file://";
    if (!bytes.empty() && bytes.front() != u8'/') url += '/';
    for (auto c : bytes) {
        if (c == u8' ') url += "%20";
        else url += static_cast<char>(c);
    }
    return url;
}
using Prepare = std::function<void(const fs::path&)>;
using Change = std::function<void(Json&)>;
// Makes an archive holding `binary` as the shell, and a manifest for it.
std::string new_release(const std::string& name, const std::string& version, const fs::path& binary,
                        const Prepare& prepare = {}, const Change& change = {}, const std::string& platform_id = platform_name) {
    const auto folder = root / (name + "-content");
    fs::create_directories(folder);
    fs::copy_file(binary, folder / shell_name);
    text(folder / "Commands" / "new.seed", "\"new\"");
    write_receipt(folder, version);
    if (prepare) prepare(folder);
    const auto archive = root / (name + archive_name);
#ifdef _WIN32
    const std::vector<std::string> packing{"-a", "-cf", path_text(archive), "-C", path_text(folder), "."};
#else
    const std::vector<std::string> packing{"-czf", path_text(archive), "-C", path_text(folder), "."};
#endif
    const auto packed = platform::run_program(platform::system_tool("tar"), packing, root / "tar.log", 60000);
    if (packed.status) throw std::runtime_error("cannot make a test archive: " + packed.output);
    const auto bytes = slurp(archive);
    Json manifest{{"schema", "emergence.manifest/1"}, {"id", "hirari"}, {"name", "hirari"}, {"edition", "pre-alpha"}, {"platform", platform_id},
                  {"version", version}, {"artifact", {{"url", file_url(archive)}, {"bytes", bytes.size()}, {"sha256", sha256(bytes)}}}};
    if (change) change(manifest);
    const auto path = root / (name + ".json");
    text(path, manifest.dump(2));
    return file_url(path);
}
std::string output;
int status = 0;
std::string trimmed(std::string value) {
    while (!value.empty() && std::string_view(" \r\n\t").find(value.back()) != std::string_view::npos) value.pop_back();
    return value;
}
void update(const fs::path& folder, const std::string& manifest, const std::vector<std::string>& options = {}) {
    platform::set_environment("HIRARI_UPDATE_MANIFEST", manifest);
    // Each run identifies its own installation, as a shell started from PATH does.
    platform::set_environment("HIRARI_HOME", std::nullopt);
    std::vector<std::string> arguments{"--update"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    const auto log = root / ("run-" + std::to_string(platform::process_id()) + ".log");
    const auto ran = platform::run_program(folder / shell_name, arguments, log, 120000);
    status = ran.status;
    output = trimmed(ran.output);
}
std::string state(const fs::path& folder) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file()) files[relative_text(entry.path(), folder)] = sha256(slurp(entry.path()));
    std::string out;
    for (const auto& [path, hash] : files) out += path + " " + hash + "\n";
    return out;
}
void refused(const std::string& name, const fs::path& folder, const std::string& manifest, const std::string& why) {
    const auto before = state(folder);
    update(folder, manifest);
    check(status == 1 && output.find(why) != std::string::npos, name + " is refused (" + std::to_string(status) + "; " + output + ")");
    check(state(folder) == before, name + " leaves the installation exactly as it was");
}

int run_tests(const std::vector<std::string>& argv) {
    if (argv.size() != 3) { std::cerr << "usage: update_tests CURRENT FIXTURE\n"; return 2; }
    root = fs::temp_directory_path() / ("hirari update " + std::to_string(platform::process_id()));
    int result = 1;
    try {
        fs::remove_all(root);
        fs::create_directories(root);
        const auto current = fs::absolute(text_path(argv[1])), fixture = fs::absolute(text_path(argv[2]));
        const auto reported = platform::run_program(current, {"--version"}, root / "version.log", 30000);
        const auto version = trimmed(reported.output).substr(std::string("hirari ").size());
        const auto install = new_install("installed hirari", version, current);
        const auto good = new_release("good", "99.0.0", fixture);

        update(install, new_release("same", version, current));
        check(status == 0 && output == "UP-TO-DATE " + version, "the latest version is up to date (" + output + ")");
        update(install, new_release("older", "0.0.1", current));
        check(status == 0 && output == "UP-TO-DATE " + version, "an older latest is never installed (" + output + ")");
        const auto before = state(install);
        update(install, good, {"--check"});
        check(status == 10 && output == "UPDATE-AVAILABLE " + version + " 99.0.0", "--check reports the update with status 10 (" + output + ")");
        check(state(install) == before, "--check changes nothing");
        update(install, good, {"--nonsense"});
        check(status == 2, "an unknown option is a usage error");

        refused("a wrong sha256", install, new_release("hash", "99.0.0", fixture, {}, [](Json& m) { m["artifact"]["sha256"] = std::string(64, '0'); }), "does not match");
        refused("a wrong size", install, new_release("size", "99.0.0", fixture, {}, [](Json& m) { m["artifact"]["bytes"] = 1; }), "does not match");
        refused("another platform", install, new_release("platform", "99.0.0", fixture, {}, {}, other_platform), "not " + platform_name);
        refused("a missing archive", install, new_release("missing", "99.0.0", fixture, {}, [](Json& m) { m["artifact"]["url"] = m["artifact"]["url"].get<std::string>() + ".absent"; }), "cannot download");
        refused("a changed file", install, new_release("tamper", "99.0.0", fixture, [](const fs::path& f) { text(f / "Commands" / "new.seed", "tampered"); }), "does not match its receipt");
        refused("an unlisted file", install, new_release("extra", "99.0.0", fixture, [](const fs::path& f) { text(f / "extra.txt", "x"); }), "does not name");
        refused("a path out of the installation", install, new_release("escape", "99.0.0", fixture, [](const fs::path& f) { write_receipt(f, "99.0.0", {"../outside.txt"}); }), "unsafe path");
        refused("a shell that lies about its version", install, new_release("lying", "99.0.0", current), "does not report 99.0.0");
        refused("a file already here that no release owns", install, new_release("collide", "99.0.0", fixture, [](const fs::path& f) { text(f / "mine.txt", "x"); write_receipt(f, "99.0.0"); }), "left as it is");

        const auto bare = root / "bare";
        fs::create_directories(bare);
        fs::copy_file(current, bare / shell_name);
        refused("a build folder with no receipt", bare, good, "not a release installation");
        refused("a folder pinned to its version", new_install(version, version, current), good, "pinned");

        // The real thing, with another copy of the old shell still running from the installation.
        std::thread running([&] { platform::run_program(install / shell_name, {"--eval", "sleep 4"}, root / "running.log", 60000); });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        update(install, good);
        running.join();
        check(status == 0 && output == "UPDATED " + version + " 99.0.0", "the installation updates, with its old shell running (" + std::to_string(status) + "; " + output + ")");
        const auto now = platform::run_program(install / shell_name, {"--version"}, root / "version.log", 30000);
        check(trimmed(now.output) == "hirari 99.0.0", "the installation runs the new shell");
        check(fs::exists(install / "Commands" / "new.seed"), "the new release is in place");
        check(!fs::exists(install / "Commands" / "old.seed"), "what the old release alone had is gone");
        check(!fs::exists(install / "Gone"), "a folder the old release emptied is gone");
        check(slurp(install / "mine.txt") == "not part of any release", "a file no release owns stays");
        check(Json::parse(slurp(install / "release.receipt.json")).at("version") == "99.0.0", "the receipt names the new version");

        update(install, good);
        check(status == 0 && output == "UP-TO-DATE 99.0.0", "the update is not repeated (" + output + ")");
        update(install, good);
        check(status == 0, "the updated shell still updates");
        std::cout << "HIRARI_UPDATE_OK tests=" << passed << '\n';
        result = 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    platform::set_environment("HIRARI_UPDATE_MANIFEST", std::nullopt);
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return result;
}
}
HIRARI_TEST_MAIN(run_tests)
