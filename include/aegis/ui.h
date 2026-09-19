#pragma once

#include <QString>

namespace aegis::ui {

QString styleSheet();
QString statusRail(int files, int additions, int deletions, const QString &agentState);

}
