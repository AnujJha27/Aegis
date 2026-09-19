#include "aegis/ui.h"

#include <QCoreApplication>

#include <cassert>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto css = aegis::ui::styleSheet();
    assert(css.contains("#0A0D10"));
    assert(css.contains("#D6DEE8"));
    assert(css.contains("#6EA8FE"));
    assert(css.contains("#5ED6C8"));
    assert(aegis::ui::statusRail(3, 9, 2, "CODEX IDLE") == "Δ 3 FILES    +9 −2    CODEX IDLE");
    assert(aegis::ui::sessionHeader("vista", "CODEX") == "AEGIS   vista                              CODEX ●");
    assert(aegis::ui::reviewSummary(3, 9, 2, 1) == "3 CHANGED FILES\n+9 −2\n1 FINDING");
    assert(aegis::ui::activityHeader("codex") == "◆ CODEX");
    return 0;
}
