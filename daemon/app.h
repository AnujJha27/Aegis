#pragma once

#include "daemon/agents/manager.h"
#include "daemon/api/server.h"
#include "daemon/repository/git.h"

#include <cstdint>
#include <filesystem>

namespace aegis::daemon {

class App final {
public:
    App(std::filesystem::path repository, std::filesystem::path webRoot);
    ~App();

    bool start(std::uint16_t port = 0);
    void stop();
    std::uint16_t port() const;

private:
    std::filesystem::path repository_;
    std::filesystem::path webRoot_;
    Store store_;
    EventHub events_;
    repository::GitRepository git_;
    agents::Manager agents_;
    api::Server server_;
};

}
