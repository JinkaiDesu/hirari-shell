#pragma once
#include "hirari/host.hpp"
#include <string>
#include <string_view>
#include <vector>
namespace hirari {
// `hirari --update [--check]`: answers the process's exit status.
int run_update(const std::vector<std::string>& options, std::string_view version, Host& host);
}
