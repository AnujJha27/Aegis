#include "daemon/agents/codex_adapter.h"
#include "daemon/agents/manager.h"
#include "daemon/agents/pty_adapter.h"
#include "daemon/process/process.h"
#include "daemon/protocol/event_hub.h"
#include "daemon/repository/git.h"
#include "daemon/session/store.h"
#include "daemon/verification/runner.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

int main() {
    const auto repository = std::filesystem::current_path();
    const auto command = aegis::daemon::process::run({"/usr/bin/printf", "ok"}, repository);
    assert(command.exitCode == 0);
    assert(command.output == "ok");

    aegis::daemon::process::ChildProcess child;
    assert(child.start({"/bin/sh", "-c", "sleep 30"}, repository));
    aegis::daemon::process::Result interrupted;
    std::thread waiter([&] { interrupted = child.wait(std::chrono::seconds(10)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    child.interrupt();
    waiter.join();
    assert(interrupted.exitCode >= 128);
    aegis::daemon::repository::GitRepository git(repository);
    assert(!git.state().path.empty());
    assert(!git.state().branch.empty());
    auto ownBranch = aegis::daemon::process::run(
        {"git", "--git-dir=" + (repository / ".aegis-git").string(), "--work-tree=" + repository.string(), "branch", "--show-current"}, repository).output;
    if (!ownBranch.empty() && ownBranch.back() == '\n') ownBranch.pop_back();
    assert(git.currentBranch() == ownBranch);

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

    const auto fakeBin = std::filesystem::temp_directory_path() / ("aegis-fake-codex-" + std::to_string(getpid()));
    std::filesystem::create_directories(fakeBin);
    const auto codexLog = fakeBin / "invocations.log";
    const auto codexScript = fakeBin / "codex";
    std::ofstream(codexScript) << "#!/bin/sh\n"
        << "printf '%s\\n' \"$*\" >> \"$CODEX_TEST_LOG\"\n"
        << "if [ \"$2\" != resume ]; then echo '{\"type\":\"thread.started\",\"thread_id\":\"thread-123\"}'; fi\n"
        << "sleep 0.2\n"
        << "case \"$*\" in *slow*) sleep 30;; esac\n"
        << "echo '{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"reply\"}}'\n";
    assert(chmod(codexScript.c_str(), 0755) == 0);
    const auto previousPath = std::getenv("PATH") ? std::string(std::getenv("PATH")) : std::string{};
    setenv("PATH", (fakeBin.string() + ":" + previousPath).c_str(), 1);
    setenv("CODEX_TEST_LOG", codexLog.c_str(), 1);
    std::mutex codexMutex;
    std::condition_variable codexChanged;
    std::vector<aegis::daemon::AgentEvent> codexOutput;
    std::string providerSession;
    aegis::daemon::agents::CodexAdapter codex(
        [&](aegis::daemon::AgentEvent event) {
            std::lock_guard lock(codexMutex);
            codexOutput.push_back(std::move(event));
            codexChanged.notify_all();
        },
        [&](const std::string &, std::string session) { providerSession = std::move(session); });
    assert(codex.start({"task", "codex-run", "codex", repository, std::nullopt}));
    assert(codex.send("first") == aegis::daemon::agents::SendResult::accepted);
    assert(codex.send("overlap") == aegis::daemon::agents::SendResult::busy);
    auto waitForEvents = [&](const std::string &type, std::size_t count) {
        std::unique_lock lock(codexMutex);
        return codexChanged.wait_for(lock, std::chrono::seconds(3), [&] {
            return static_cast<std::size_t>(std::count_if(codexOutput.begin(), codexOutput.end(), [&](const auto &event) { return event.type == type; })) >= count;
        });
    };
    assert(waitForEvents("turn.completed", 1));
    assert(providerSession == "thread-123");
    assert(codex.send("second") == aegis::daemon::agents::SendResult::accepted);
    assert(waitForEvents("turn.completed", 2));
    assert(codex.send("slow") == aegis::daemon::agents::SendResult::accepted);
    assert(waitForEvents("turn.started", 3));
    codex.interrupt();
    assert(waitForEvents("turn.interrupted", 1));
    assert(codex.send("after interrupt") == aegis::daemon::agents::SendResult::accepted);
    assert(waitForEvents("turn.completed", 3));
    codex.terminate();
    const auto invocationText = [&] {
        std::ifstream input(codexLog);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    assert(invocationText.find("exec resume --json thread-123 second") != std::string::npos);
    assert(invocationText.find("exec resume --json thread-123 after interrupt") != std::string::npos);

    const auto activeManagerDb = std::filesystem::temp_directory_path() / "aegis-manager-shutdown-test.sqlite";
    std::filesystem::remove(activeManagerDb);
    {
        aegis::daemon::Store managerStore(activeManagerDb);
        aegis::daemon::EventHub managerEvents;
        const auto task = managerStore.createTask("shutdown active run", repository.string());
        std::string activeRunId;
        {
            aegis::daemon::agents::Manager manager(repository, managerStore, managerEvents);
            const auto run = manager.launch(task.id, "codex");
            assert(run);
            activeRunId = run->id;
            assert(manager.send(activeRunId, "slow") == aegis::daemon::agents::SendResult::accepted);
        }
        const auto stopped = managerStore.run(activeRunId);
        assert(stopped && stopped->status == "terminated" && stopped->finishedAt > 0);
    }
    std::filesystem::remove(activeManagerDb);
    unsetenv("CODEX_TEST_LOG");
    setenv("PATH", previousPath.c_str(), 1);
    std::filesystem::remove_all(fakeBin);

    const auto managerDatabase = std::filesystem::temp_directory_path() / "aegis-manager-lifecycle-test.sqlite";
    std::filesystem::remove(managerDatabase);
    {
        aegis::daemon::Store managerStore(managerDatabase);
        aegis::daemon::EventHub managerEvents;
        aegis::daemon::agents::Manager manager(repository, managerStore, managerEvents);
        const auto task = managerStore.createTask("test run state", repository.string());
        const auto run = manager.launch(task.id, "shell");
        assert(run && run->status == "running");
        assert(manager.send(run->id, "exit") == aegis::daemon::agents::SendResult::accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto saved = managerStore.run(run->id);
            if (saved && saved->status == "completed") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto finished = managerStore.run(run->id);
        assert(finished && finished->status == "completed" && finished->finishedAt > 0);
        const auto history = managerStore.events(task.id);
        assert(std::any_of(history.begin(), history.end(), [](const auto &event) { return event.type == "user.message" && event.content == "exit"; }));
        assert(manager.terminate(run->id));
        assert(managerStore.run(run->id)->status == "completed");
    }
    std::filesystem::remove(managerDatabase);

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
    assert(pty.start({"task", "pty-run", "shell", repository, std::nullopt}));
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

    std::mutex ptyFailureMutex;
    std::condition_variable ptyFailureChanged;
    std::string ptyFailure;
    aegis::daemon::agents::PtyAdapter failingPty("shell", {"/bin/sh", "-c", "exit 7"},
        [&](aegis::daemon::AgentEvent event) {
            if (event.type != "run.failed" && event.type != "run.completed") return;
            std::lock_guard lock(ptyFailureMutex);
            ptyFailure = event.type;
            ptyFailureChanged.notify_all();
        });
    assert(failingPty.start({"task", "pty-failure", "shell", repository, std::nullopt}));
    {
        std::unique_lock lock(ptyFailureMutex);
        assert(ptyFailureChanged.wait_for(lock, std::chrono::seconds(2), [&] { return !ptyFailure.empty(); }));
    }
    assert(ptyFailure == "run.failed");
    failingPty.terminate();

    aegis::daemon::Store store(std::filesystem::temp_directory_path() / "aegis-daemon-services-test.sqlite");
    aegis::daemon::EventHub events;
    aegis::daemon::agents::Manager manager(repository, store, events);
    assert(!manager.available().empty());
}
