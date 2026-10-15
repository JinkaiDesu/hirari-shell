#pragma once
#include "hirari/host.hpp"
#include "hirari/values.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
namespace hirari {
class Jobs {
public:
    Jobs(Values&, Host&);
    ~Jobs();
private:
    struct Job {
        std::mutex mutex;std::thread worker;
        std::atomic_bool cancelled{false},obsolete{false},done{false};
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
        Json state;std::string pending;
        ~Job(){if(worker.joinable())worker.join();}
    };
    Json invoke(const std::vector<Json>&);
    Json snapshot(const std::shared_ptr<Job>&);
    std::shared_ptr<Job> find(const Json& id);
    Host& host_;
    std::unordered_map<std::string,std::shared_ptr<Job>> jobs_;
    std::unordered_map<std::string,std::string> latest_;
    std::uint64_t sequence_=1;
};
}
