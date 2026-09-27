#include "aegis/core.h"
#include "aegis/analysis.h"
#include "aegis/commands.h"
#include "aegis/session.h"
#include "aegis/terminal.h"
#include "aegis/transcript.h"
#include "aegis/ui.h"
#include "aegis/window.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QShortcut>
#include <QStandardPaths>
#include <QTextStream>

#include <functional>
#include <csignal>
#include <memory>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) {
    stopRequested = 1;
}

using aegis::app::git;
using aegis::app::runCommand;

QString renderReport(const aegis::analysis::Report &report) {
    QString output = "Changed files\n" + (report.changedFiles.isEmpty() ? "(none)" : report.changedFiles.join('\n'));
    output += "\n\nSemantic changes\n" + (report.semanticChanges.isEmpty() ? "(none)" : report.semanticChanges.join('\n'));
    output += "\n\nFindings\n";
    for (const auto &finding : report.findings)
        output += QString("%1 %2:%3 %4\n").arg(finding.severity, finding.file).arg(finding.line).arg(finding.message);
    output += "\n\nDependencies\n" + (report.dependencies.isEmpty() ? "(none)" : report.dependencies.join('\n'));
    output += "\n\nTests\n" + (report.impactedTests.isEmpty() ? "(none)" : report.impactedTests.join('\n'));
    output += "\n\nBlast radius\n" + (report.blastRadius.isEmpty() ? "(none)" : report.blastRadius.join('\n'));
    output += "\n\nArchitecture\n" + (report.architectureEdges.isEmpty() ? "(none)" : report.architectureEdges.join('\n'));
    output += "\n\nCompiler AST\n" + (report.ast.isEmpty() ? "(compiler unavailable or file did not parse)" : report.ast.join('\n'));
    output += "\n\nCall graph\n" + (report.callGraph.isEmpty() ? "(none)" : report.callGraph.join('\n'));
    output += "\n\nSpecialized lenses\n" + (report.solidity + report.binary + report.systems + report.formal).join('\n');
    return output;
}

QString daemonExecutable() {
    const auto sibling = QDir(QCoreApplication::applicationDirPath()).filePath("aegis_daemon");
    if (QFileInfo(sibling).isExecutable()) return sibling;
    return QStandardPaths::findExecutable("aegis_daemon");
}

QString frontendRoot(const QString &repository) {
    QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath("web/dist"),
        QDir::cleanPath(QDir::current().filePath("web/dist")),
        QDir(repository).filePath("web/dist")};
#ifdef AEGIS_SOURCE_WEB_ROOT
    candidates << QStringLiteral(AEGIS_SOURCE_WEB_ROOT);
#endif
    for (const auto &candidate : candidates)
        if (QFileInfo(candidate).isDir()) return candidate;
    return {};
}

bool openBrowser(const QString &url) {
    if (qEnvironmentVariableIsSet("WSL_DISTRO_NAME")) {
        const auto wslview = QStandardPaths::findExecutable("wslview");
        if (!wslview.isEmpty() && QProcess::startDetached(wslview, {url})) return true;
        const auto windowsCommand = QStandardPaths::findExecutable("cmd.exe");
        if (!windowsCommand.isEmpty())
            return QProcess::startDetached(windowsCommand, {"/c", "start", "", url});
        return false;
    }
    return QDesktopServices::openUrl(QUrl(url));
}

int runDaemon(const QStringList &arguments) {
    const auto executable = daemonExecutable();
    if (executable.isEmpty()) {
        QTextStream(stderr) << "aegis: aegis_daemon was not found beside the executable or on PATH\n";
        return 1;
    }
    auto forwarded = arguments.mid(1);
    forwarded.removeAll("--daemon");
    return QProcess::execute(executable, forwarded);
}

int runWebApp(QApplication &app, const QString &repository) {
    const auto executable = daemonExecutable();
    const auto webRoot = frontendRoot(repository);
    if (executable.isEmpty() || webRoot.isEmpty()) {
        QTextStream(stderr) << "aegis: daemon or frontend bundle not found; build web first\n";
        return 1;
    }

    QProcess daemon;
    daemon.setProcessChannelMode(QProcess::SeparateChannels);
    const QStringList daemonArguments{"--repo", repository, "--port", "0", "--web-root", webRoot};
    QString url;
    QEventLoop startup;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&daemon, &QProcess::readyReadStandardOutput, &startup, [&] {
        const auto output = QString::fromLocal8Bit(daemon.readAllStandardOutput()).trimmed();
        if (output.startsWith("http://127.0.0.1:")) {
            url = output.split('\n').first().trimmed();
            startup.quit();
        }
    });
    QObject::connect(&daemon, &QProcess::errorOccurred, &startup, [&] { startup.quit(); });
    QObject::connect(&daemon, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &startup, [&] { startup.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &startup, &QEventLoop::quit);
    daemon.start(executable, daemonArguments);
    if (!daemon.waitForStarted(1500)) {
        QTextStream(stderr) << "aegis: could not start local daemon\n";
        return 1;
    }
    timeout.start(5000);
    startup.exec();
    if (url.isEmpty()) {
        QTextStream(stderr) << "aegis: local daemon did not become ready\n" << daemon.readAllStandardError();
        daemon.terminate();
        daemon.waitForFinished(1000);
        return 1;
    }
    if (!openBrowser(url))
        QTextStream(stderr) << "aegis: browser could not be opened; use " << url << '\n';
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &daemon, [&] {
        if (daemon.state() == QProcess::NotRunning) return;
        daemon.terminate();
        if (!daemon.waitForFinished(1500)) daemon.kill();
    });
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer signalPoll;
    QObject::connect(&signalPoll, &QTimer::timeout, &app, [&] {
        if (stopRequested) app.quit();
    });
    signalPoll.start(100);
    return app.exec();
}

int runCli(const QStringList &args) {
    const auto command = args.value(1);
    auto repo = QDir(command == "verify" ? "." : args.value(2, ".")).absolutePath();
    const auto requireRepo = [&repo] {
        if (QFileInfo(repo).isDir()) return true;
        QTextStream(stderr) << "aegis: not a repository directory: " << repo << '\n';
        return false;
    };
    if (command == "status") {
        if (!requireRepo()) return 2;
        const auto status = git(repo, {"status", "--short", "--branch"});
        const auto summary = aegis::summarizeGit(status, git(repo, {"diff", "HEAD", "--numstat"}));
        QTextStream(stdout) << QString("%1 files  +%2 -%3  branch %4\n%5")
                                   .arg(summary.files).arg(summary.insertions).arg(summary.deletions)
                                   .arg(summary.branch).arg(status);
        return 0;
    }
    if (command == "analyze" || command == "lens") {
        if (!requireRepo()) return 2;
        const auto report = aegis::analysis::analyze(repo, git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"}), "CLI");
        QTextStream(stdout) << (command == "lens" ? (report.solidity + report.binary + report.systems + report.formal).join('\n') : renderReport(report)) << '\n';
        return 0;
    }
    if (command == "history") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << aegis::session::events(repo).join('\n') << "\n\n" << git(repo, {"log", "--oneline", "--decorate", "-20"});
        return 0;
    }
    if (command == "board") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << aegis::session::board(repo).join('\n') << '\n';
        return 0;
    }
    if (command == "worktree") {
        if (!requireRepo()) return 2;
        const auto path = repo + "/.aegis/worktrees/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QDir().mkpath(QFileInfo(path).path());
        const auto result = runCommand("git", {"worktree", "add", "--detach", path, "HEAD"}, repo);
        QTextStream(stdout) << result.output << path << '\n';
        return result.exitCode;
    }
    if (command == "review") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"});
        return 0;
    }
    if (command == "snapshot") {
        if (!requireRepo()) return 2;
        QDir().mkpath(repo + "/.aegis/snapshots");
        const auto path = repo + "/.aegis/snapshots/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".patch";
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
        file.write(git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"}).toUtf8());
        QTextStream(stdout) << path << '\n';
        return 0;
    }
    if (command == "verify") {
        auto verify = args.mid(2);
        if (!verify.isEmpty() && verify.first() != "--") {
            repo = QDir(verify.takeFirst()).absolutePath();
        }
        if (!requireRepo()) return 2;
        if (!verify.isEmpty() && verify.first() == "--") verify.removeFirst();
        if (verify.isEmpty()) verify = {"ctest", "--test-dir", "build"};
        const auto result = runCommand(verify.first(), verify.mid(1), repo);
        QTextStream(stdout) << result.output;
        return result.exitCode < 0 ? 1 : result.exitCode;
    }
    return -1;
}

}

int main(int argc, char **argv) {
    const auto cliCommands = QStringList{"status", "review", "verify", "snapshot", "analyze", "lens", "history", "board", "worktree"};
    if (argc > 1 && cliCommands.contains(QString::fromLocal8Bit(argv[1]))) {
        QCoreApplication app(argc, argv);
        return runCli(app.arguments());
    }
    const auto rawArguments = [&] {
        QStringList result;
        for (int index = 0; index < argc; ++index) result << QString::fromLocal8Bit(argv[index]);
        return result;
    }();
    if (rawArguments.contains("--daemon")) {
        QCoreApplication app(argc, argv);
        return runDaemon(rawArguments);
    }
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--help" || QString::fromLocal8Bit(argv[i]) == "-h") {
            QTextStream(stdout) << "Usage: aegis [path] [--agent shell|codex|claude|opencode] [--paranoia]\n"
                                   << "       aegis status|review|verify|snapshot|analyze|lens|history|board|worktree [path]\n"
                                   << "       aegis verify [path] -- command args...\n";
            return 0;
        }
    }

    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("A local-first control plane for coding agents");
    parser.addHelpOption();
    parser.addOption({{"a", "agent"}, "Agent to run: shell, codex, claude, or opencode", "agent", "shell"});
    parser.addOption({"paranoia", "Disable snapshot persistence and keep the session local"});
    parser.addOption({"legacy-ui", "Open the Qt prototype instead of the browser control plane"});
    parser.addPositionalArgument("path", "Repository path", ".");
    parser.process(app);
    const auto path = parser.positionalArguments().value(0, ".");
    auto agent = parser.value("agent");
    auto actualPath = path;
    if (agent == "shell" && QStringList{"codex", "claude", "opencode", "shell"}.contains(path.toLower())) {
        agent = path.toLower();
        actualPath = parser.positionalArguments().value(1, ".");
    }
    if (!QFileInfo(actualPath).isDir()) {
        QTextStream(stderr) << "aegis: not a repository directory: " << QDir(actualPath).absolutePath() << '\n';
        return 2;
    }
    if (!parser.isSet("legacy-ui")) return runWebApp(app, actualPath);
    std::unique_ptr<QMainWindow> window(aegis::app::createWindow(actualPath, agent, parser.isSet("paranoia")));
    window->show();
    return app.exec();
}
