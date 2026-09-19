#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>

#include <functional>

namespace aegis::terminal {

class PtySession final : public QObject {
public:
    explicit PtySession(QObject *parent = nullptr);
    ~PtySession() override;

    bool start(const QString &program, const QStringList &arguments, const QString &directory);
    void write(const QByteArray &data);
    void terminate();
    bool isRunning() const { return pid_ > 0; }

    std::function<void(const QByteArray &)> onOutput;
    std::function<void(const QString &)> onError;
    std::function<void(int)> onFinished;

private:
    void readAvailable();
    void reap();

    int master_ = -1;
    int pid_ = -1;
    QSocketNotifier *notifier_ = nullptr;
};

}
