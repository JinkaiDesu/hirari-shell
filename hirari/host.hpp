#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace hirari {

// Cancellation requested by the user or host. It is not an operation failure,
// so recovery (`attempt`) never absorbs it.
struct Interruption : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// `exit [STATUS]`. Deliberately not a std::exception: failure handlers and
// script diagnostics must never absorb or rewrite an explicit exit.
struct ExitRequest {
    int status = 0;
};

class Host {
public:
    virtual ~Host() = default;

    virtual void write(
        std::string_view bytes
    ) = 0;

    virtual void error(
        std::string_view bytes
    ) = 0;

    virtual bool interrupted() { return false; }

    // True when the host can forward raw foreground terminal input.
    virtual bool terminal_capable() const {
        return false;
    }

    // Called when a foreground ConPTY application
    // enters/leaves control of the terminal.
    //
    virtual void foreground(
        bool active,
        int status
    ) {
        (void)active;
        (void)status;
    }
};

} // namespace hirari