#include "aegis/core.h"

namespace aegis {

QStringList agentCommand(const QString &agent) {
    const auto name = agent.trimmed().toLower();
    if (name == "codex") return {"codex"};
    if (name == "claude") return {"claude"};
    if (name == "opencode") return {"opencode"};
    return {"/bin/sh"};
}

ChangeSummary summarizeGit(const QString &status, const QString &numstat) {
    ChangeSummary result;
    for (const auto &line : status.split('\n', Qt::SkipEmptyParts)) {
        if (line.startsWith("## ")) {
            result.branch = line.mid(3).section("...", 0, 0);
        } else if (line.size() >= 2) {
            ++result.files;
        }
    }

    for (const auto &line : numstat.split('\n', Qt::SkipEmptyParts)) {
        const auto columns = line.split('\t');
        if (columns.size() < 2) continue;
        bool ok = false;
        const auto added = columns[0].toInt(&ok);
        if (ok) result.insertions += added;
        const auto removed = columns[1].toInt(&ok);
        if (ok) result.deletions += removed;
    }
    return result;
}

}
