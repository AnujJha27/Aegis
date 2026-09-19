#pragma once

#include <QStringList>

namespace aegis::session {

void appendEvent(const QString &repo, const QString &kind, const QString &detail, bool paranoia);
QStringList events(const QString &repo);
void pin(const QString &repo, const QString &item, bool paranoia);
QStringList board(const QString &repo);

}
