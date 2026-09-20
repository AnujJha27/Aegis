#include "aegis/transcript.h"

#include <QCoreApplication>

#include <cassert>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    aegis::transcript::CodexStream stream;
    const auto first = stream.feed(
        "{\"type\":\"thread.started\",\"thread_id\":\"thread-1\"}\n"
        "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"hello\"}}\n");
    assert(stream.threadId() == "thread-1");
    assert(first.size() == 1);
    assert(first.first().label == "CODEX");
    assert(first.first().body == "hello");

    const auto second = stream.feed("{\"type\":\"item.completed\",\"item\":{\"type\":\"command_execution\",\"command\":\"ls\"}}");
    assert(second.isEmpty());
    const auto finished = stream.finish();
    assert(finished.size() == 1);
    assert(finished.first().label == "TOOL");
    return 0;
}
