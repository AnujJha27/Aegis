#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace aegis::transcript {

struct Event {
    QString label;
    QString body;
};

class CodexStream final {
public:
    QList<Event> feed(const QByteArray &data);
    QList<Event> finish();
    const QString &threadId() const { return threadId_; }
    void reset();

private:
    QByteArray buffer_;
    QString threadId_;
};

}
