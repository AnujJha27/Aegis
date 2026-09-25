#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/manager.h"
#include "daemon/process/process.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/session/store.h"
#include "daemon/verification/runner.h"

#include <cassert>
#include <filesystem>

int main() {
    const auto repository = std::filesystem::current_path();
    const auto command = aegis::daemon::process::run({"/usr/bin/printf", "ok"}, repository);
    assert(command.exitCode == 0);
    assert(command.output == "ok");

    aegis::daemon::repository::GitRepository git(repository);
    assert(!git.state().path.empty());
    assert(!git.state().branch.empty());

    const auto verification = aegis::daemon::verification::run({"/usr/bin/true"}, repository);
    assert(verification.exitCode == 0);

    const auto event = aegis::daemon::agents::parseCodexJsonLine(
        R"({"type":"item.completed","item":{"type":"agent_message","text":"implemented"}})", "task", "run", "codex");
    assert(event.has_value());
    assert(event->type == "agent.message.completed");
    assert(event->content == "implemented");

    aegis::daemon::Store store(std::filesystem::temp_directory_path() / "aegis-daemon-services-test.sqlite");
    aegis::daemon::EventHub events;
    aegis::daemon::agents::Manager manager(repository, store, events);
    assert(!manager.available().empty());
}
