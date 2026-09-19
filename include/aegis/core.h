#pragma once

#include <QString>
#include <QStringList>

namespace aegis {

struct ChangeSummary {
    QString branch;
    int files = 0;
    int insertions = 0;
    int deletions = 0;
};

QStringList agentCommand(const QString &agent);
ChangeSummary summarizeGit(const QString &status, const QString &numstat);
QString sideBySideDiff(const QString &diff);

}
