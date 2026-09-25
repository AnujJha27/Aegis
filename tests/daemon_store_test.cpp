#include "daemon/session/store.h"
#include "daemon/protocol/event_hub.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "aegis-daemon-store-test.sqlite";
    std::filesystem::remove(path);

    {
        aegis::daemon::Store store(path);
        const auto task = store.createTask("inspect the parser", "/tmp/repo");
        assert(!task.id.empty());
        assert(task.prompt == "inspect the parser");

        const auto run = store.startRun(task.id, "codex");
        assert(!run.id.empty());
        assert(run.taskId == task.id);

        store.appendEvent({"event-1", task.id, run.id, "agent.started", "codex", "", 1});
        store.appendEvent({"event-2", task.id, run.id, "agent.message.completed", "codex", "done", 2});

        assert(store.tasks().size() == 1);
        assert(store.events(task.id).size() == 2);
    }

    {
        aegis::daemon::Store store(path);
        assert(store.tasks().size() == 1);
        assert(store.events(store.tasks().front().id).front().content == "");
    }

    std::filesystem::remove(path);

    aegis::daemon::EventHub hub;
    const auto subscription = hub.subscribe();
    hub.publish({"event-3", "task", "run", "agent.finished", "shell", "ok", 3});
    aegis::daemon::AgentEvent event;
    assert(hub.wait(subscription, event, std::chrono::milliseconds(10)));
    assert(event.type == "agent.finished");
    hub.unsubscribe(subscription);
    assert(!hub.wait(subscription, event, std::chrono::milliseconds(1)));
}
