#include "aegis/terminal.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    aegis::terminal::PtySession session;
    QByteArray output;
    bool finished = false;
    session.onOutput = [&](const QByteArray &data) { output += data; };
    session.onFinished = [&](int code) { finished = code == 0; app.quit(); };
    Q_ASSERT(session.start("/bin/sh", {"-c", "printf 'pty-ok\\n'"}, "."));
    QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    app.exec();
    Q_ASSERT(finished);
    Q_ASSERT(output.contains("pty-ok"));
    return 0;
}
