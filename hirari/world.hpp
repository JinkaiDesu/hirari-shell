#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
namespace hirari {
struct WorldDescriptor { std::string name; unsigned version=1; std::filesystem::path root,entry; };
std::string path_text(const std::filesystem::path&);
std::filesystem::path text_path(const std::string&);
std::string read_source(const std::filesystem::path&);
WorldDescriptor read_world(const std::filesystem::path&);
class WorldHost {
public:
    void open(const std::filesystem::path&,const std::function<void(const std::filesystem::path&)>& entry);
    const std::optional<WorldDescriptor>& current()const noexcept{return current_;}
    std::string status()const;
private:
    std::optional<WorldDescriptor> current_;
};
}
