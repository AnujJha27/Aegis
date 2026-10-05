#pragma once
#include "daemon/repository/files.h"
#include "daemon/protocol/event_hub.h"
#include <functional>
#include <thread>
namespace aegis::daemon::repository {
class RepositoryWatcher final {
public:
    RepositoryWatcher(GitRepository &git, Files &files, EventHub &events, std::function<bool()> active);
    ~RepositoryWatcher();
private:
    std::jthread thread_;
};
}
