#include "aegis/ui.h"

#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTextBlock>

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

QString sessionHeader(const QString &project, const QString &agent) {
    return QString("AEGIS   %1                              %2 ●").arg(project, agent);
}

QString reviewSummary(int files, int additions, int deletions, int findings) {
    return QString("%1 CHANGED FILES\n+%2 −%3\n%4 FINDING%5")
        .arg(files).arg(additions).arg(deletions).arg(findings).arg(findings == 1 ? "" : "S");
}

QString activityHeader(const QString &agent) {
    return "◆ " + agent.toUpper();
}

QString activityLine(const QString &action, const QString &value, bool last) {
    return QString("  %1 %2 %3").arg(last ? "└" : "├", action, value);
}

QString diffLineMarker(const QString &line) {
    return line.isEmpty() ? " " : line.left(1);
}

QString deepViewTitle(const QString &mode) {
    return "DEEP VIEW / " + mode.toUpper();
}

class CodeView::LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(CodeView *editor) : QWidget(editor), editor_(editor) {}

    QSize sizeHint() const override { return {editor_->lineNumberWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent *event) override { editor_->paintLineNumbers(event); }

private:
    CodeView *editor_;
};

CodeView::CodeView(QWidget *parent) : QPlainTextEdit(parent), lineNumbers_(new LineNumberArea(this)) {
    setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int count) { updateLineNumberWidth(count); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int delta) {
        if (delta) lineNumbers_->scroll(0, delta);
        else lineNumbers_->update(0, rect.y(), lineNumbers_->width(), rect.height());
        if (rect.contains(viewport()->rect())) updateLineNumberWidth(0);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, lineNumbers_, qOverload<>(&QWidget::update));
    updateLineNumberWidth(0);
}

int CodeView::lineNumberWidth() const {
    int digits = 1;
    auto value = qMax(1, blockCount());
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return 10 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeView::updateLineNumberWidth(int) {
    setViewportMargins(lineNumberWidth(), 0, 0, 0);
    lineNumbers_->setFixedWidth(lineNumberWidth());
}

void CodeView::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    lineNumbers_->setGeometry(0, 0, lineNumberWidth(), height());
}

void CodeView::paintLineNumbers(QPaintEvent *event) {
    QPainter painter(lineNumbers_);
    painter.fillRect(event->rect(), QColor("#0E1217"));
    auto block = firstVisibleBlock();
    auto top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    const auto bottom = event->rect().bottom();
    while (block.isValid() && top <= bottom) {
        const auto height = static_cast<int>(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(block == textCursor().block() ? QColor("#6EA8FE") : QColor("#52606D"));
            painter.drawText(0, top, lineNumbers_->width() - 6, height, Qt::AlignRight, QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top += height;
    }
}

void CodeView::setDiffText(const QString &text) {
    setPlainText(text);
    QList<QTextEdit::ExtraSelection> selections;
    auto block = document()->firstBlock();
    while (block.isValid()) {
        const auto marker = diffLineMarker(block.text());
        if (marker == "+" || marker == "-") {
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setBackground(QColor(marker == "+" ? "#18352C" : "#3A252B"));
            selections.append(selection);
        }
        block = block.next();
    }
    setExtraSelections(selections);
}

}
