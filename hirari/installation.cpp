#include "hirari/installation.hpp"
#include "hirari/platform.hpp"
#include "hirari/world.hpp"
#include <stdexcept>
namespace hirari {
void initialize_installation() {
    // Whatever starts hirari selects PATH, providers, profiles and writable state directories.
    // A standalone shell only identifies its own installation.
    if(platform::environment("HIRARI_HOME"))return;
    auto executable=platform::own_executable();
    if(executable.empty())throw std::runtime_error("cannot locate Hirari executable");
    platform::set_environment("HIRARI_HOME",path_text(executable.parent_path()));
}
}
