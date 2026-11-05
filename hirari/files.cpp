// Host capability module: the filesystem, for the shell's own file commands.
#include "hirari/files.hpp"
#include "hirari/value_wire.hpp"
#include "hirari/world.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace hirari {
namespace {
namespace fs = std::filesystem;

struct Request {
    std::string operation;
    std::vector<fs::path> paths;
    bool recursive = false;
};

Request parse(const std::vector<Json>& arguments) {
    auto operation = arguments.empty() ? Json{} : unpack_argument(arguments[0]);
    if (!operation.is_string()) throw std::runtime_error("use help --files for its operations");
    Request request{operation.get<std::string>()};
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        auto argument = unpack_argument(arguments[i]);
        if (argument.is_string()) request.paths.push_back(text_path(argument.get<std::string>()));
        else if (argument.is_object() && argument.value("$seed", "") == "name" && argument.at("value").value("value", "") == "r") request.recursive = true;
        else throw std::runtime_error(request.operation + ": paths are quoted text, and the only flag is --r");
    }
    return request;
}

[[noreturn]] void failed(const Request& request, const fs::path& path, const std::string& why) {
    throw std::runtime_error(request.operation + ": " + path_text(path) + ": " + why);
}

std::string folded(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Json entry(const fs::directory_entry& item, std::string name) {
    std::error_code error;
    const bool directory = item.is_directory(error);
    const bool file = item.is_regular_file(error);
    return {{"name", std::move(name)}, {"kind", directory ? "directory" : file ? "file" : "other"},
            {"size", file ? item.file_size(error) : 0}};
}

Json list(const Request& request) {
    if (request.paths.size() > 1) throw std::runtime_error("list takes one path");
    auto root = request.paths.empty() ? fs::path(".") : request.paths.front();
    std::error_code error;
    if (!fs::exists(root, error)) failed(request, root, "no such file or directory");
    if (!fs::is_directory(root, error)) return Json::array({entry(fs::directory_entry(root), path_text(root.filename()))});
    Json entries = Json::array();
    for (fs::directory_iterator walk(root, fs::directory_options::skip_permission_denied, error), end; !error && walk != end; walk.increment(error))
        entries.push_back(entry(*walk, path_text(walk->path().filename())));
    if (error) failed(request, root, error.message());
    std::sort(entries.begin(), entries.end(), [](const Json& a, const Json& b) { return folded(a.at("name")) < folded(b.at("name")); });
    return entries;
}

void make_directories(const Request& request) {
    if (request.paths.empty()) throw std::runtime_error("usage: mkdir \"DIRECTORY\" ...");
    for (const auto& path : request.paths) {
        std::error_code error;
        fs::create_directories(path, error);
        if (error) failed(request, path, error.message());
    }
}

void remove(const Request& request) {
    if (request.paths.empty()) throw std::runtime_error("usage: rm [--r] \"PATH\" ...");
    for (const auto& path : request.paths) {
        std::error_code error;
        if (!fs::exists(fs::symlink_status(path, error))) failed(request, path, "no such file or directory");
        if (fs::is_directory(fs::symlink_status(path, error))) {
            if (!request.recursive) failed(request, path, "is a directory; use rm --r");
            if (!fs::absolute(path, error).lexically_normal().has_relative_path()) failed(request, path, "refusing to remove a filesystem root");
            fs::remove_all(path, error);
        } else fs::remove(path, error);
        if (error) failed(request, path, error.message());
    }
}

// The last path is the destination, and an existing directory receives the sources by name.
void transfer(const Request& request) {
    if (request.paths.size() < 2) throw std::runtime_error("usage: " + request.operation + " \"SOURCE\" ... \"DESTINATION\"");
    const auto& destination = request.paths.back();
    std::error_code error;
    const bool into = fs::is_directory(destination, error);
    if (request.paths.size() > 2 && !into) failed(request, destination, "is not a directory");
    for (std::size_t i = 0; i + 1 < request.paths.size(); ++i) {
        const auto& source = request.paths[i];
        if (!fs::exists(source, error)) failed(request, source, "no such file or directory");
        auto target = into ? destination / source.filename() : destination;
        if (request.operation == "cp") {
            if (fs::is_directory(source, error) && !request.recursive) failed(request, source, "is a directory; use cp --r");
            fs::copy(source, target, fs::copy_options::overwrite_existing | fs::copy_options::recursive, error);
        } else {
            fs::rename(source, target, error);
            // A rename can't cross volumes.
            if (error) {
                error.clear();
                fs::copy(source, target, fs::copy_options::overwrite_existing | fs::copy_options::recursive, error);
                if (!error) fs::remove_all(source, error);
            }
        }
        if (error) failed(request, source, error.message());
    }
}
}

void install_files(Values& values) {
    values.link("files",
        "The filesystem, behind the ls, mkdir, rm, cp and mv commands.\n"
        "provide --files \"list\" [\"PATH\"]\n"
        "provide --files \"mkdir\" \"DIRECTORY\" ...\n"
        "provide --files \"rm\" [--r] \"PATH\" ...\n"
        "provide --files \"cp\"|\"mv\" [--r] \"SOURCE\" ... \"DESTINATION\"\n"
        "list answers a list of records with name, kind (file, directory or other) and size; the others answer Unit. "
        "mkdir creates parents. rm and cp need --r for a directory, and rm refuses a filesystem root. Any failure names the path.",
        [](const std::vector<Json>& arguments) -> Json {
            auto request = parse(arguments);
            if (request.operation == "list") return pack_value(list(request));
            if (request.operation == "mkdir") make_directories(request);
            else if (request.operation == "rm") remove(request);
            else if (request.operation == "cp" || request.operation == "mv") transfer(request);
            else throw std::runtime_error("unknown files operation: " + request.operation + "; see help --files");
            return pack_value(nullptr);
        });
}

} // namespace hirari
