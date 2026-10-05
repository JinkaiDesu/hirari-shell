#pragma once

#include <cstdint>
#include <string_view>

namespace hirari {

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