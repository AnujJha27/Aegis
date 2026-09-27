#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/manager.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/process/process.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/session/store.h"
#include "daemon/verification/runner.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>

int main() {
    const auto repository = std::filesystem::current_path();
    const auto command = aegis::daemon::process::run({"/usr/bin/printf", "ok"}, repository);
    assert(command.exitCode == 0);
    assert(command.output == "ok");

    aegis::daemon::repository::GitRepository git(repository);
    assert(!git.state().path.empty());
    assert(!git.state().branch.empty());

    const auto verification = aegis::daemon::verification::run({"/usr/bin/true"}, repository, "test-task");
    assert(verification.exitCode == 0);
    assert(verification.taskId == "test-task");

    const auto event = aegis::daemon::agents::parseCodexJsonLine(
        R"({"type":"item.completed","item":{"type":"agent_message","text":"implemented"}})", "task", "run", "codex");
    assert(event.has_value());
    assert(event->type == "agent.message.completed");
    assert(event->content == "implemented");

    const auto codexEvents = aegis::daemon::agents::parseCodexJsonOutput(
        R"({"type":"item.completed","item":{"type":"reasoning","text":"private instructions"}})"
        "\n"
        R"({"type":"item.completed","item":{"type":"agent_message","text":"visible answer"}})"
        "\n"
        R"({"type":"turn.completed","usage":{"input_tokens":10}})", "task", "run", "codex");
    assert(codexEvents.size() == 1);
    assert(codexEvents.front().type == "agent.message.completed");
    assert(codexEvents.front().content == "visible answer");

    std::mutex ptyMutex;
    std::condition_variable ptyOutputChanged;
    std::string ptyOutput;
    aegis::daemon::agents::PtyAdapter pty("shell", {"/bin/sh", "-c", "stty size; read line; stty size"},
        [&](aegis::daemon::AgentEvent event) {
            if (event.type != "agent.message.delta") return;
            std::lock_guard lock(ptyMutex);
            ptyOutput += event.content;
            ptyOutputChanged.notify_all();
        });
    assert(pty.start({"task", "pty-run", "shell", repository}));
    {
        std::unique_lock lock(ptyMutex);
        assert(ptyOutputChanged.wait_for(lock, std::chrono::seconds(2), [&] { return ptyOutput.find("30 100") != std::string::npos; }));
    }
    assert(pty.resizePty(120, 40));
    assert(pty.sendPty("go\n"));
    {
        std::unique_lock lock(ptyMutex);
        assert(ptyOutputChanged.wait_for(lock, std::chrono::seconds(2), [&] { return ptyOutput.find("40 120") != std::string::npos; }));
    }
    pty.terminate();

    aegis::daemon::Store store(std::filesystem::temp_directory_path() / "aegis-daemon-services-test.sqlite");
    aegis::daemon::EventHub events;
    aegis::daemon::agents::Manager manager(repository, store, events);
    assert(!manager.available().empty());
}
