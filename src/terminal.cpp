#include "aegis/terminal.h"

#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

namespace aegis::terminal {

PtySession::PtySession(QObject *parent) : QObject(parent) {}

PtySession::~PtySession() {
    terminate();
}

bool PtySession::start(const QString &program, const QStringList &arguments, const QString &directory) {
    terminate();
    QByteArray executable = program.toLocal8Bit();
    std::vector<QByteArray> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable);
    for (const auto &argument : arguments) storage.push_back(argument.toLocal8Bit());
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (auto &value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    master_ = -1;
    pid_ = forkpty(&master_, nullptr, nullptr, nullptr);
    if (pid_ < 0) {
        if (onError) onError(QString::fromLocal8Bit(strerror(errno)));
        master_ = -1;
        return false;
    }
    if (pid_ == 0) {
        const auto cwd = directory.toLocal8Bit();
        if (chdir(cwd.constData()) != 0) _exit(126);
        setenv("TERM", "xterm-256color", 1);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
    notifier_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { readAvailable(); });
    return true;
}

void PtySession::write(const QByteArray &data) {
    if (master_ < 0) return;
    const char *cursor = data.constData();
    auto remaining = data.size();
    while (remaining > 0) {
        const auto count = ::write(master_, cursor, size_t(remaining));
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        cursor += count;
        remaining -= count;
    }
}

void PtySession::readAvailable() {
    char buffer[8192];
    for (;;) {
        const auto count = ::read(master_, buffer, sizeof(buffer));
        if (count > 0) {
            if (onOutput) onOutput(QByteArray(buffer, int(count)));
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        reap();
        return;
    }
}

void PtySession::reap() {
    if (pid_ <= 0) return;
    int status = 0;
    const auto result = waitpid(pid_, &status, WNOHANG);
    if (result == 0) return;
    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (master_ >= 0) close(master_);
    master_ = -1;
    pid_ = -1;
    if (onFinished) onFinished(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

void PtySession::terminate() {
    if (pid_ <= 0) return;
    if (kill(-pid_, SIGTERM) < 0 && errno != ESRCH) kill(pid_, SIGTERM);
    int status = 0;
    waitpid(pid_, &status, 0);
    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (master_ >= 0) close(master_);
    master_ = -1;
    pid_ = -1;
}

}
