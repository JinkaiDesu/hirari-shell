#pragma once
#include "hirari/host.hpp"
#include "hirari/wire.hpp"
#include <memory>
namespace hirari {
class UnityClient {
public:
    explicit UnityClient(Host&,std::string endpoint={},unsigned timeout_ms=5000);
    ~UnityClient();
    UnityClient(const UnityClient&)=delete;
    UnityClient& operator=(const UnityClient&)=delete;
    void connect();
    void disconnect() noexcept;
    bool connected() const noexcept;
    const std::string& endpoint() const noexcept;
    const std::string& session() const noexcept;
    wire::Message request(wire::Message);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
