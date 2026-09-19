#include "aegis/session.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace aegis::session {
namespace {

QString stateDir(const QString &repo) {
    return QDir(repo).filePath(".aegis");
}

}

void appendEvent(const QString &repo, const QString &kind, const QString &detail, bool paranoia) {
    if (paranoia) return;
    const auto dir = stateDir(repo);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath("session.jsonl"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    const QJsonObject event{{"timestamp", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"kind", kind}, {"detail", detail}};
    file.write(QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n');
}

QStringList events(const QString &repo) {
    QFile file(QDir(stateDir(repo)).filePath("session.jsonl"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList result;
    for (const auto &line : file.readAll().split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        const auto object = QJsonDocument::fromJson(line).object();
        result << object.value("timestamp").toString() + "  " + object.value("kind").toString() + ": " + object.value("detail").toString();
    }
    return result;
}

void pin(const QString &repo, const QString &item, bool paranoia) {
    if (paranoia || item.trimmed().isEmpty()) return;
    const auto dir = stateDir(repo);
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath("board.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    if (!board(repo).contains(item)) file.write((item.trimmed() + '\n').toUtf8());
}

QStringList board(const QString &repo) {
    QFile file(QDir(stateDir(repo)).filePath("board.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList result;
    for (const auto &line : file.readAll().split('\n')) {
        if (!line.trimmed().isEmpty()) result << QString::fromUtf8(line).trimmed();
    }
    return result;
}

}
