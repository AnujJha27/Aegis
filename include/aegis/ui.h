#pragma once

#include <QPlainTextEdit>
#include <QString>

namespace aegis::ui {

QString styleSheet();
QString statusRail(int files, int additions, int deletions, const QString &agentState);
QString sessionHeader(const QString &project, const QString &agent);
QString reviewSummary(int files, int additions, int deletions, int findings);
QString activityHeader(const QString &agent);
QString activityLine(const QString &action, const QString &value, bool last);
QString diffLineMarker(const QString &line);
QString deepViewTitle(const QString &mode);

class CodeView final : public QPlainTextEdit {
public:
    explicit CodeView(QWidget *parent = nullptr);
    void setDiffText(const QString &text);

private:
    class LineNumberArea;
    int lineNumberWidth() const;
    void updateLineNumberWidth(int newBlockCount);
    void paintLineNumbers(QPaintEvent *event);
    void resizeEvent(QResizeEvent *event) override;

    LineNumberArea *lineNumbers_ = nullptr;
};

}
