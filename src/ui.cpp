#include "aegis/ui.h"

namespace aegis::ui {

QString styleSheet() {
    return R"(
        QWidget {
            background: #0A0D10;
            color: #D6DEE8;
            font-family: "IBM Plex Sans", "Inter", sans-serif;
            font-size: 13px;
        }
        QMainWindow { background: #0A0D10; }
        QLineEdit, QPlainTextEdit, QListWidget, QComboBox {
            background: #0E1217;
            color: #D6DEE8;
            border: 1px solid #1C242E;
            border-radius: 4px;
            selection-background-color: #294A78;
            selection-color: #FFFFFF;
        }
        QPlainTextEdit {
            font-family: "IBM Plex Mono", "JetBrains Mono", monospace;
            font-size: 12px;
            padding: 8px;
        }
        QPushButton {
            background: transparent;
            color: #8A98A8;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 5px 9px;
        }
        QPushButton:hover, QPushButton:focus {
            color: #D6DEE8;
            border-color: #6EA8FE;
            background: #141A21;
        }
        QPushButton:pressed, QTabBar::tab:selected {
            color: #FFFFFF;
            background: #294A78;
        }
        QTabWidget::pane { border: 1px solid #1C242E; background: #0E1217; }
        QTabBar::tab {
            color: #52606D;
            background: #0A0D10;
            padding: 6px 9px;
            border: 0;
        }
        QTabBar::tab:hover { color: #D6DEE8; }
        QListWidget::item { padding: 4px 6px; }
        QListWidget::item:selected { color: #FFFFFF; background: #294A78; }
        QSplitter::handle { background: #1C242E; }
        QStatusBar { background: #0E1217; color: #8A98A8; }
        QCheckBox { color: #8A98A8; spacing: 6px; }
        QScrollBar:vertical, QScrollBar:horizontal { background: #0A0D10; }
        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: #1C242E;
            border-radius: 3px;
        }
        QLabel#brand { color: #D6DEE8; font-size: 14px; font-weight: 600; letter-spacing: 1px; }
        QLabel#muted { color: #52606D; }
        QLabel#state { color: #5ED6C8; }
        QLabel#rail { color: #8A98A8; background: #0E1217; border-top: 1px solid #1C242E; padding: 7px 10px; }
    )";
}

QString statusRail(int files, int additions, int deletions, const QString &agentState) {
    return QString("Δ %1 FILES    +%2 −%3    %4").arg(files).arg(additions).arg(deletions).arg(agentState);
}

}
