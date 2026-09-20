#pragma once

#include <QString>
#include <QStringList>

namespace aegis::app {

struct CommandResult {
    int exitCode = -1;
    QString output;
};

CommandResult runCommand(const QString &program, const QStringList &arguments, const QString &directory);
QString git(const QString &repo, const QStringList &arguments);

}
