#pragma once

#include <QString>

namespace aegis::ui {

QString styleSheet();
QString statusRail(int files, int additions, int deletions, const QString &agentState);
QString sessionHeader(const QString &project, const QString &agent);
QString reviewSummary(int files, int additions, int deletions, int findings);
QString activityHeader(const QString &agent);

}
