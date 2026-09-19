#pragma once

#include <QString>

namespace aegis::ui {

QString styleSheet();
QString statusRail(int files, int additions, int deletions, const QString &agentState);
QString sessionHeader(const QString &project, const QString &agent);

}
