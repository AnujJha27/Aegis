#pragma once

#include <QMainWindow>
#include <QString>

namespace aegis::app {

QMainWindow *createWindow(const QString &repo, const QString &selectedAgent, bool paranoia);

}
