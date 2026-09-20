#include "aegis/transcript.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace aegis::transcript {

QList<Event> CodexStream::feed(const QByteArray &data) {
    QList<Event> events;
    buffer_ += data;
    while (true) {
        const auto newline = buffer_.indexOf('\n');
        if (newline < 0) break;
        const auto line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        if (line.isEmpty()) continue;

        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            events.append({"CODEX OUTPUT", QString::fromUtf8(line)});
            continue;
        }
        const auto event = document.object();
        const auto type = event.value("type").toString();
        if (type == "thread.started") {
            threadId_ = event.value("thread_id").toString();
        } else if (type == "error") {
            events.append({"CODEX ERROR", event.value("message").toString()});
        } else if (type == "item.completed") {
            const auto item = event.value("item").toObject();
            const auto itemType = item.value("type").toString();
            if (itemType == "agent_message") events.append({"CODEX", item.value("text").toString()});
            else if (itemType == "command_execution") events.append({"TOOL", item.value("command").toString()});
        }
    }
    return events;
}

QList<Event> CodexStream::finish() {
    if (buffer_.trimmed().isEmpty()) return {};
    const auto data = buffer_ + '\n';
    buffer_.clear();
    return feed(data);
}

void CodexStream::reset() {
    buffer_.clear();
    threadId_.clear();
}

}
