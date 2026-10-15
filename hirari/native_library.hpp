#pragma once
#include <vector>
#include "hirari/vendor/json.hpp"
#include <memory>
#include <string>
namespace hirari {
class Host;
// Optional, provider-neutral C ABI. No evaluator or application-specific types.
class NativeLibrary {
public:
    explicit NativeLibrary(const std::string& path);
    ~NativeLibrary();
    nlohmann::json invoke(const std::vector<nlohmann::json>& arguments, Host& host);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
