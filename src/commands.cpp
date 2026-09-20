#include "aegis/commands.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

namespace aegis::app {

CommandResult runCommand(const QString &program, const QStringList &arguments, const QString &directory) {
    QProcess process;
    process.setWorkingDirectory(directory);
    if (program == "git" && !arguments.contains("-C")) {
        const auto privateGit = QDir(directory).filePath(".aegis-git");
        if (QFileInfo(privateGit).isDir()) {
            auto environment = QProcessEnvironment::systemEnvironment();
            environment.insert("GIT_DIR", privateGit);
            environment.insert("GIT_WORK_TREE", directory);
            process.setProcessEnvironment(environment);
        }
    }
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) return {-1, process.errorString()};
    process.waitForFinished(-1);
    return {process.exitCode(), QString::fromLocal8Bit(process.readAllStandardOutput() + process.readAllStandardError())};
}
QString git(const QString &repo, const QStringList &arguments) {
    return runCommand("git", arguments, repo).output;
}

}
