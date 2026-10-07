#pragma once
#include "seed/seed.hpp"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace hirari {
// Definitions are serialized structure, never startup code or runtime pointers.
class Vocabulary {
public:
    struct Revision { std::uint64_t number; std::string name, annotation, form; };
    explicit Vocabulary(std::filesystem::path directory);
    const std::filesystem::path& directory() const { return directory_; }
    void load(seed::Runtime&);
    void save(seed::Runtime&, const std::string& name, const std::string& annotation,
              const std::function<bool(seed::Ptr)>& opaque);
    void restore(seed::Runtime&, const std::string& name, std::uint64_t revision);
    void forget(seed::Runtime&, const std::string& name);
    std::string describe(seed::Runtime&, const std::string& name = {}, bool history = false) const;
    std::string annotation(const std::string& name) const;
private:
    std::vector<Revision> read() const;
    void append(Revision);
    std::filesystem::path directory_;
};
}
