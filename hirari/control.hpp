#pragma once
#include "hirari/host.hpp"
#include "hirari/process.hpp"
#include "seed/seed.hpp"
namespace hirari {
// Shell control facilities composed from Seed's own `if`; the kernel is unchanged.
//   while CONDITION BODY   repeat without deepening the native stack
//   sleep SECONDS          interruptible timed wait
//   fail MESSAGE ...       raise an operation failure
//   attempt EXPRESSION     observe success, program failure or error as a record
class Control {
public:
    Control(seed::Runtime&, Host&);
private:
    static seed::Ptr loop(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr pause(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr fail(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr attempt(seed::Runtime&, seed::Node*, seed::Ptr);
    Host& host_;
};
}
