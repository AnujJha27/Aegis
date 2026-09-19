#include "aegis/session.h"

#include <QCoreApplication>
#include <QTemporaryDir>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir repo;
    Q_ASSERT(repo.isValid());

    aegis::session::appendEvent(repo.path(), "task", "started", false);
    aegis::session::appendEvent(repo.path(), "verify", "passed", false);
    aegis::session::pin(repo.path(), "src/auth.cpp", false);
    Q_ASSERT(aegis::session::events(repo.path()).size() == 2);
    Q_ASSERT(aegis::session::events(repo.path()).first().contains("task: started"));
    Q_ASSERT(aegis::session::board(repo.path()) == QStringList{"src/auth.cpp"});

    aegis::session::appendEvent(repo.path(), "secret", "not persisted", true);
    aegis::session::pin(repo.path(), "secret.txt", true);
    Q_ASSERT(aegis::session::events(repo.path()).size() == 2);
    Q_ASSERT(!aegis::session::board(repo.path()).contains("secret.txt"));
    return 0;
}
