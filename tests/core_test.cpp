#include "aegis/core.h"

#include <QCoreApplication>
#include <QDebug>

using namespace aegis;

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    Q_ASSERT(agentCommand("codex") == QStringList{"codex"});
    Q_ASSERT(agentCommand("shell") == QStringList{"/bin/sh"});
    Q_ASSERT(agentCommand("unknown") == QStringList{"/bin/sh"});

    const auto summary = summarizeGit(
        "## feature/x...origin/feature/x\n M src/a.cpp\n?? notes.txt\n",
        "3\t1\tsrc/a.cpp\n-	-	notes.txt\n");
    Q_ASSERT(summary.branch == "feature/x");
    Q_ASSERT(summary.files == 2);
    Q_ASSERT(summary.insertions == 3);
    Q_ASSERT(summary.deletions == 1);

    const auto rendered = sideBySideDiff(
        "@@ -1,2 +1,2 @@\n-old\n+new\n same\n");
    Q_ASSERT(rendered.contains("old\t|\tnew"));
    Q_ASSERT(rendered.contains("same\t|\tsame"));

    qInfo() << "aegis_core_test: passed";
    return 0;
}
