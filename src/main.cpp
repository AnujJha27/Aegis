#include "aegis/core.h"
#include "aegis/analysis.h"
#include "aegis/session.h"
#include "aegis/terminal.h"
#include "aegis/ui.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QCommandLineParser>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QShortcut>
#include <QStandardPaths>
#include <QTextStream>

#include <functional>
#include <memory>

namespace {

struct CommandResult {
    int exitCode = -1;
    QString output;
};

CommandResult runCommand(const QString &program, const QStringList &arguments, const QString &directory) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.start(program, arguments);
    if (!process.waitForStarted(3000)) return {-1, process.errorString()};
    process.waitForFinished(-1);
    return {process.exitCode(), QString::fromLocal8Bit(process.readAllStandardOutput() + process.readAllStandardError())};
}

QString git(const QString &repo, const QStringList &arguments) {
    return runCommand("git", arguments, repo).output;
}

QString readText(const QString &repo, const QString &file) {
    QFile input(QDir(repo).filePath(file));
    if (!input.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(input.read(4 * 1024 * 1024));
}

QStringList changedFiles(const QString &status) {
    QStringList files;
    for (const auto &line : status.split('\n', Qt::SkipEmptyParts)) {
        if (!line.startsWith("## ") && line.size() > 3) files << line.mid(3);
    }
    return files;
}

QString renderReport(const aegis::analysis::Report &report) {
    QString output = "Changed files\n" + (report.changedFiles.isEmpty() ? "(none)" : report.changedFiles.join('\n'));
    output += "\n\nSemantic changes\n" + (report.semanticChanges.isEmpty() ? "(none)" : report.semanticChanges.join('\n'));
    output += "\n\nFindings\n";
    for (const auto &finding : report.findings)
        output += QString("%1 %2:%3 %4\n").arg(finding.severity, finding.file).arg(finding.line).arg(finding.message);
    output += "\n\nDependencies\n" + (report.dependencies.isEmpty() ? "(none)" : report.dependencies.join('\n'));
    output += "\n\nTests\n" + (report.impactedTests.isEmpty() ? "(none)" : report.impactedTests.join('\n'));
    output += "\n\nBlast radius\n" + (report.blastRadius.isEmpty() ? "(none)" : report.blastRadius.join('\n'));
    output += "\n\nArchitecture\n" + (report.architectureEdges.isEmpty() ? "(none)" : report.architectureEdges.join('\n'));
    output += "\n\nCompiler AST\n" + (report.ast.isEmpty() ? "(compiler unavailable or file did not parse)" : report.ast.join('\n'));
    output += "\n\nCall graph\n" + (report.callGraph.isEmpty() ? "(none)" : report.callGraph.join('\n'));
    output += "\n\nSpecialized lenses\n" + (report.solidity + report.binary + report.systems + report.formal).join('\n');
    return output;
}

void sendLsp(QProcess &process, const QJsonObject &message) {
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    process.write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    process.waitForBytesWritten(3000);
}

QString readLsp(QProcess &process) {
    QByteArray data;
    for (int attempt = 0; attempt < 20; ++attempt) {
        process.waitForReadyRead(250);
        data += process.readAllStandardOutput();
        const auto headerEnd = data.indexOf("\r\n\r\n");
        if (headerEnd < 0) continue;
        const auto header = data.left(headerEnd);
        const auto length = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(header));
        if (!length.hasMatch()) return QString::fromUtf8(data);
        const int bodyStart = headerEnd + 4;
        if (data.size() >= bodyStart + length.captured(1).toInt()) return QString::fromUtf8(data.mid(bodyStart, length.captured(1).toInt()));
    }
    return QString::fromUtf8(data);
}

class Window final : public QMainWindow {
public:
    Window(QString repo, QString selectedAgent, bool paranoia)
        : repo_(QDir(std::move(repo)).absolutePath()) {
        setWindowTitle("AEGIS — " + QFileInfo(repo_).fileName());
        resize(1200, 760);
        setMinimumSize(720, 480);
        setStyleSheet(aegis::ui::styleSheet());

        auto *root = new QWidget(this);
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(10, 8, 10, 8);
        layout->setSpacing(6);

        auto *header = new QHBoxLayout;
        auto *brand = new QLabel(root);
        brand->setObjectName("brand");
        brand->setText(aegis::ui::sessionHeader(QFileInfo(repo_).fileName(), selectedAgent.toUpper()));
        header->addWidget(brand, 1);
        agent_ = new QComboBox(root);
        agent_->addItems({"shell", "codex", "claude", "opencode"});
        agent_->setCurrentText(selectedAgent);
        header->addWidget(agent_);
        agentState_ = new QLabel("● IDLE", root);
        agentState_->setObjectName("state");
        header->addWidget(agentState_);
        auto *review = new QPushButton("REVIEW  Ctrl+R", root);
        header->addWidget(review);
        auto *palette = new QPushButton("Palette", root);
        header->addWidget(palette);
        paranoia_ = new QCheckBox("Paranoia", root);
        paranoia_->setChecked(paranoia);
        header->addWidget(paranoia_);
        layout->addLayout(header);

        auto *split = new QSplitter(Qt::Horizontal, root);
        terminal_ = new QPlainTextEdit(split);
        terminal_->setReadOnly(true);
        terminal_->setPlaceholderText("Managed agent terminal output");

        reviewPanel_ = new QWidget(split);
        auto *rightLayout = new QVBoxLayout(reviewPanel_);
        rightLayout->setContentsMargins(6, 0, 0, 0);
        reviewTitle_ = new QLabel("REVIEW / EVIDENCE", reviewPanel_);
        reviewTitle_->setObjectName("muted");
        rightLayout->addWidget(reviewTitle_);
        reviewSummary_ = new QLabel("No active changes.\nAgent modifications will appear here automatically.", reviewPanel_);
        reviewSummary_->setObjectName("muted");
        reviewSummary_->setWordWrap(true);
        rightLayout->addWidget(reviewSummary_);
        reviewChecks_ = new QLabel("BUILD   —\nTESTS   —\nFINDINGS 0", reviewPanel_);
        reviewChecks_->setObjectName("muted");
        rightLayout->addWidget(reviewChecks_);
        reviewDetails_ = new QPlainTextEdit(reviewPanel_);
        reviewDetails_->setReadOnly(true);
        reviewDetails_->setMaximumHeight(130);
        reviewDetails_->setPlaceholderText("No verification or findings yet.");
        rightLayout->addWidget(reviewDetails_);
        auto *reviewActions = new QHBoxLayout;
        auto *reviewVerify = new QPushButton("VERIFY", reviewPanel_);
        auto *reviewAnalyze = new QPushButton("ANALYZE", reviewPanel_);
        reviewActions->addWidget(reviewVerify);
        reviewActions->addWidget(reviewAnalyze);
        reviewActions->addStretch();
        rightLayout->addLayout(reviewActions);
        files_ = new QListWidget(reviewPanel_);
        files_->setMaximumHeight(120);
        files_->setContextMenuPolicy(Qt::CustomContextMenu);
        rightLayout->addWidget(files_);
        tabs_ = new QTabWidget(reviewPanel_);
        unified_ = new aegis::ui::CodeView(tabs_);
        unified_->setReadOnly(true);
        side_ = new aegis::ui::CodeView(tabs_);
        side_->setReadOnly(true);
        trace_ = new QPlainTextEdit(tabs_);
        trace_->setReadOnly(true);
        evidence_ = new QPlainTextEdit(tabs_);
        evidence_->setReadOnly(true);
        symbols_ = new QPlainTextEdit(tabs_);
        symbols_->setReadOnly(true);
        architecture_ = new QPlainTextEdit(tabs_);
        architecture_->setReadOnly(true);
        risk_ = new QPlainTextEdit(tabs_);
        risk_->setReadOnly(true);
        timeline_ = new QPlainTextEdit(tabs_);
        timeline_->setReadOnly(true);
        board_ = new QPlainTextEdit(tabs_);
        board_->setReadOnly(true);
        lenses_ = new QPlainTextEdit(tabs_);
        lenses_->setReadOnly(true);
        tabs_->addTab(unified_, "Unified diff");
        tabs_->addTab(side_, "Side-by-side");
        tabs_->addTab(evidence_, "Evidence");
        tabs_->addTab(symbols_, "Symbols");
        tabs_->addTab(architecture_, "Architecture");
        tabs_->addTab(risk_, "Risk");
        tabs_->addTab(timeline_, "Timeline");
        tabs_->addTab(board_, "Board");
        tabs_->addTab(lenses_, "Lenses");
        tabs_->addTab(trace_, "Trace");
        tabs_->setDocumentMode(true);
        rightLayout->addWidget(tabs_);
        reviewPanel_->setMinimumWidth(360);
        split->addWidget(terminal_);
        split->addWidget(reviewPanel_);
        split->setStretchFactor(0, 1);
        split->setStretchFactor(1, 0);
        reviewPanel_->setVisible(false);
        layout->addWidget(split, 1);

        verifyCommand_ = new QLineEdit("ctest --test-dir build", root);
        verifyCommand_->setVisible(false);

        auto *promptRow = new QHBoxLayout;
        prompt_ = new QLineEdit(root);
        prompt_->setPlaceholderText("Prompt the agent or request a revision of this changeset");
        promptRow->addWidget(prompt_);
        auto *send = new QPushButton("Send", root);
        promptRow->addWidget(send);
        layout->addLayout(promptRow);
        status_ = new QLabel("Δ 0 FILES    +0 −0    IDLE", root);
        status_->setObjectName("rail");
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(status_);
        setCentralWidget(root);
        trace_->appendPlainText("session start " + QDateTime::currentDateTime().toString(Qt::ISODate) + " repo " + repo_);

        connect(agent_, &QComboBox::currentTextChanged, this, [this] { if (!process_.isRunning()) startAgent(); });
        connect(send, &QPushButton::clicked, this, [this] { sendPrompt(); });
        connect(prompt_, &QLineEdit::returnPressed, this, [this] { sendPrompt(); });
        connect(review, &QPushButton::clicked, this, [this] { toggleReview(); });
        connect(reviewVerify, &QPushButton::clicked, this, [this] { runVerification(); });
        connect(reviewAnalyze, &QPushButton::clicked, this, [this] { analyzeChanges(); });
        connect(files_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
            if (!files_->itemAt(position)) return;
            QMenu menu(this);
            menu.addAction("Open", this, [this] { openEditor(); });
            menu.addAction("Pin", this, [this] { pinSelected(); });
            menu.addAction("Ask agent", this, [this] { sendPrompt(); });
            menu.exec(files_->viewport()->mapToGlobal(position));
        });
        connect(palette, &QPushButton::clicked, this, [this] { showPalette(); });
        process_.onOutput = [this](const QByteArray &data) { appendTerminal(data); };
        process_.onError = [this](const QString &error) { appendTerminal(("\n[aegis] " + error + "\n").toUtf8()); };
        process_.onFinished = [this](int code) {
            trace_->appendPlainText("agent exited with code " + QString::number(code));
            agentState_->setText("● IDLE");
            refreshStatus();
        };
        connect(&timer_, &QTimer::timeout, this, [this] { refreshStatus(); });
        auto *paletteShortcut = new QShortcut(QKeySequence("Ctrl+Shift+P"), this);
        connect(paletteShortcut, &QShortcut::activated, this, [this] { showPalette(); });
        auto *reviewShortcut = new QShortcut(QKeySequence("Ctrl+R"), this);
        connect(reviewShortcut, &QShortcut::activated, this, [this] { toggleReview(); });
        auto *quickOpenShortcut = new QShortcut(QKeySequence("Ctrl+P"), this);
        connect(quickOpenShortcut, &QShortcut::activated, this, [this] { quickOpen(); });
        auto *closeReviewShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(closeReviewShortcut, &QShortcut::activated, this, [this] { if (reviewPanel_->isVisible()) toggleReview(); });
        auto *deepShortcut = new QShortcut(QKeySequence("Ctrl+Shift+O"), this);
        connect(deepShortcut, &QShortcut::activated, this, [this] { analyzeChanges(); tabs_->setCurrentWidget(evidence_); });
        timer_.start(1000);
        QTimer::singleShot(0, this, [this] { refreshStatus(); refreshReview(); });
        aegis::session::appendEvent(repo_, "session", "started", paranoia_->isChecked());
        startAgent();
    }

private:
    void appendTerminal(const QByteArray &data) {
        terminal_->moveCursor(QTextCursor::End);
        terminal_->insertPlainText(QString::fromLocal8Bit(data));
        terminal_->ensureCursorVisible();
    }

    void startAgent() {
        if (process_.isRunning()) return;
        const auto command = aegis::agentCommand(agent_->currentText());
        agentState_->setText("● RUNNING");
        trace_->appendPlainText(aegis::ui::activityHeader(agent_->currentText()));
        trace_->appendPlainText(aegis::ui::activityLine("start", "agent session", true));
        trace_->appendPlainText("start " + command.join(' '));
        if (!process_.start(command.first(), command.mid(1), repo_)) {
            agentState_->setText("● IDLE");
            appendTerminal("\n[aegis] failed to start agent\n");
        }
    }

    void toggleReview() {
        const auto open = !reviewPanel_->isVisible();
        reviewPanel_->setVisible(open);
        if (open) {
            reviewTitle_->setText("REVIEW / EVIDENCE");
            refreshReview();
        }
        trace_->appendPlainText(open ? "review opened" : "review closed");
    }

    void openDeepView(QWidget *view, const QString &mode) {
        if (!reviewPanel_->isVisible()) reviewPanel_->setVisible(true);
        reviewTitle_->setText(aegis::ui::deepViewTitle(mode));
        tabs_->setCurrentWidget(view);
        trace_->appendPlainText(aegis::ui::deepViewTitle(mode));
    }

    void sendPrompt() {
        const auto text = prompt_->text().trimmed();
        if (text.isEmpty()) return;
        lastPrompt_ = text;
        if (!process_.isRunning()) startAgent();
        const auto line = (text.startsWith("revise:", Qt::CaseInsensitive)
                               ? "Please revise the current changeset: " + text.mid(7).trimmed()
                               : text) + "\n";
        process_.write(line.toUtf8());
        trace_->appendPlainText(aegis::ui::activityLine("prompt", line.trimmed(), true));
        aegis::session::appendEvent(repo_, "prompt", line.trimmed(), paranoia_->isChecked());
        prompt_->clear();
    }

    void refreshStatus() {
        if (statusRefreshInFlight_) return;
        statusRefreshInFlight_ = true;
        runGitAsync({"status", "--short", "--branch"}, [this](const QString &status) {
            runGitAsync({"diff", "HEAD", "--numstat"}, [this, status](const QString &numstat) {
                statusRefreshInFlight_ = false;
                if (!lastStatus_.isEmpty() && lastStatus_ != status)
                    aegis::session::appendEvent(repo_, "repository", "working tree changed", paranoia_->isChecked());
                lastStatus_ = status;
                const auto summary = aegis::summarizeGit(status, numstat);
                reviewSummary_->setText(aegis::ui::reviewSummary(summary.files, summary.insertions, summary.deletions,
                                                                 report_.findings.size()));
                status_->setText(aegis::ui::statusRail(summary.files, summary.insertions, summary.deletions,
                                                       agent_->currentText().toUpper() +
                                                           (paranoia_->isChecked() ? " PARANOIA" : " IDLE")) +
                                 "    BUILD —    TESTS —    ⚠ " + QString::number(report_.findings.size()) +
                                 "    REVIEW ^R    " + (summary.branch.isEmpty() ? "(detached)" : summary.branch));
                const auto selected = files_->currentItem() ? files_->currentItem()->text() : QString();
                files_->clear();
                files_->addItems(changedFiles(status));
                for (int i = 0; i < files_->count(); ++i) {
                    if (files_->item(i)->text() == selected) files_->setCurrentRow(i);
                }
            });
        });
    }

    QString diff() const {
        auto result = git(repo_, {"diff", "HEAD", "--no-ext-diff", "--no-color"});
        if (result.trimmed().isEmpty()) result = "(no tracked diff)";
        return result;
    }

    void refreshReview() {
        if (reviewRefreshInFlight_) return;
        reviewRefreshInFlight_ = true;
        runGitAsync({"diff", "HEAD", "--no-ext-diff", "--no-color"}, [this](QString result) {
            reviewRefreshInFlight_ = false;
            if (result.trimmed().isEmpty()) result = "(no tracked diff)";
            unified_->setDiffText(result);
            side_->setDiffText(aegis::sideBySideDiff(result));
        });
    }

    void runGitAsync(const QStringList &arguments, std::function<void(QString)> callback) {
        auto *process = new QProcess(this);
        process->setWorkingDirectory(repo_);
        const auto completed = std::make_shared<bool>(false);
        const auto finish = [process, completed, callback = std::move(callback)](QString output) {
            if (*completed) return;
            *completed = true;
            callback(std::move(output));
            process->deleteLater();
        };
        connect(process, &QProcess::finished, this, [process, finish](int, QProcess::ExitStatus) mutable {
            finish(QString::fromLocal8Bit(process->readAllStandardOutput() + process->readAllStandardError()));
        });
        connect(process, &QProcess::errorOccurred, this, [process, finish](QProcess::ProcessError) mutable {
            if (process->state() == QProcess::NotRunning) finish(process->errorString());
        });
        process->start("git", arguments);
    }

    void runVerification() {
        const auto command = QProcess::splitCommand(verifyCommand_->text());
        if (command.isEmpty()) return;
        trace_->appendPlainText("verify " + command.join(' '));
        const auto result = runCommand(command.first(), command.mid(1), repo_);
        terminal_->appendPlainText(QString("\n[verify exit %1]\n%2").arg(result.exitCode).arg(result.output));
        reviewChecks_->setText(QString("BUILD   %1\nTESTS   %2\nFINDINGS %3")
                                   .arg(result.exitCode == 0 ? "✓" : "✕")
                                   .arg(result.exitCode == 0 ? "✓" : "✕")
                                   .arg(report_.findings.size()));
        trace_->appendPlainText(QString("verify exit %1").arg(result.exitCode));
        aegis::session::appendEvent(repo_, "verify", QString("%1 (exit %2)").arg(command.join(' ')).arg(result.exitCode), paranoia_->isChecked());
        refreshReview();
    }

    void saveSnapshot() {
        if (paranoia_->isChecked()) {
            terminal_->appendPlainText("\n[aegis] snapshot skipped: Paranoia Mode\n");
            return;
        }
        const auto directory = repo_ + "/.aegis/snapshots";
        QDir().mkpath(directory);
        const auto path = directory + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".patch";
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(diff().toUtf8());
            terminal_->appendPlainText("\n[aegis] snapshot: " + path + "\n");
            trace_->appendPlainText("snapshot " + path);
            aegis::session::appendEvent(repo_, "snapshot", path, paranoia_->isChecked());
        }
    }

    void openEditor() {
        if (!files_->currentItem()) return;
        auto path = files_->currentItem()->text();
        if (path.contains(" -> ")) path = path.section(" -> ", -1);
        QDesktopServices::openUrl(QUrl::fromLocalFile(repo_ + "/" + path));
        trace_->appendPlainText("open editor " + path);
    }

    void quickOpen() {
        QStringList choices;
        for (int i = 0; i < files_->count(); ++i) choices << files_->item(i)->text();
        if (choices.isEmpty()) {
            trace_->appendPlainText("quick open: no changed files");
            return;
        }

        QDialog dialog(this);
        dialog.setWindowTitle("Quick open");
        dialog.resize(560, 320);
        auto *layout = new QVBoxLayout(&dialog);
        auto *filter = new QLineEdit(&dialog);
        filter->setPlaceholderText("Search changed files…");
        auto *results = new QListWidget(&dialog);
        results->addItems(choices);
        layout->addWidget(filter);
        layout->addWidget(results, 1);
        connect(filter, &QLineEdit::textChanged, &dialog, [filter, results] {
            const auto query = filter->text().trimmed();
            for (int i = 0; i < results->count(); ++i)
                results->item(i)->setHidden(!query.isEmpty() && !results->item(i)->text().contains(query, Qt::CaseInsensitive));
            for (int i = 0; i < results->count(); ++i) {
                if (!results->item(i)->isHidden()) {
                    results->setCurrentRow(i);
                    break;
                }
            }
        });
        connect(filter, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
        connect(results, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
        filter->setFocus();
        if (dialog.exec() != QDialog::Accepted || !results->currentItem()) return;
        const auto selected = results->currentItem()->text();
        for (int i = 0; i < files_->count(); ++i) {
            if (files_->item(i)->text() == selected) {
                files_->setCurrentRow(i);
                openEditor();
                return;
            }
        }
    }

    QString list(const QStringList &items) const {
        return items.isEmpty() ? "(none)" : items.join('\n');
    }

    void analyzeChanges() {
        report_ = aegis::analysis::analyze(repo_, diff(), agent_->currentText());
        QString evidence = "EVIDENCE\n\nChanged files\n" + list(report_.changedFiles) + "\n\nSemantic changes\n" + list(report_.semanticChanges);
        evidence += "\n\nDependencies\n" + list(report_.dependencies) + "\n\nLikely affected tests\n" + list(report_.impactedTests);
        evidence += "\n\nBlast radius\n" + list(report_.blastRadius) + "\n\nProvenance\n" + list(report_.provenance);
        evidence += "\n\nFindings\n";
        if (report_.findings.isEmpty()) evidence += "(none)";
        for (const auto &finding : report_.findings)
            evidence += QString("%1 %2:%3 %4\n").arg(finding.severity, finding.file).arg(finding.line).arg(finding.message);
        evidence_->setPlainText(evidence);

        QString symbols = "COMPILER AST\n" + list(report_.ast) + "\n\nCALL GRAPH\n" + list(report_.callGraph) + "\n\n";
        for (const auto &symbol : report_.symbols) {
            symbols += QString("%1  %2:%3  %4 references\n").arg(symbol.name, symbol.file).arg(symbol.line).arg(symbol.references);
            symbols += "  callers: " + (symbol.callers.isEmpty() ? "(none)" : symbol.callers.join(", ")) + "\n";
            symbols += "  callees: " + (symbol.callees.isEmpty() ? "(none)" : symbol.callees.join(", ")) + "\n";
        }
        symbols_->setPlainText(symbols.isEmpty() ? "No symbols detected." : symbols);
        architecture_->setPlainText(list(report_.architectureEdges));

        QString risk;
        for (auto it = report_.risk.cbegin(); it != report_.risk.cend(); ++it)
            risk += QString("%1  %2 %3\n").arg(it.key(), QString(qMin(it.value(), 20), QChar('#'))).arg(it.value());
        risk_->setPlainText(risk.isEmpty() ? "No changed files." : risk);

        QString lenses = "SOLIDITY\n" + list(report_.solidity) + "\n\nBINARY\n" + list(report_.binary) +
                         "\n\nSYSTEMS\n" + list(report_.systems) + "\n\nFORMAL\n" + list(report_.formal);
        lenses_->setPlainText(lenses);
        reviewSummary_->setText(aegis::ui::reviewSummary(report_.changedFiles.size(), 0, 0, report_.findings.size()));
        reviewDetails_->setPlainText("FINDINGS\n" + list(report_.findings.isEmpty() ? QStringList{"✓ No new diagnostics"} : QStringList{QString::number(report_.findings.size()) + " finding(s)"}) +
                                     "\n\nDEPENDENCIES\n" + list(report_.dependencies.isEmpty() ? QStringList{"✓ No dependency changes"} : report_.dependencies) +
                                     "\n\nSYMBOLS\n" + QString::number(report_.symbols.size()) + " affected symbols");
        refreshHistory();
        aegis::session::appendEvent(repo_, "analysis", QString::number(report_.findings.size()) + " findings", paranoia_->isChecked());
    }

    void refreshHistory() {
        auto history = aegis::session::events(repo_);
        history << "\nGIT HISTORY\n" + git(repo_, {"log", "--oneline", "--decorate", "-20"});
        timeline_->setPlainText(history.join('\n'));
        board_->setPlainText(list(aegis::session::board(repo_)));
    }

    void pinSelected() {
        const auto item = files_->currentItem() ? files_->currentItem()->text() : report_.changedFiles.value(0);
        if (item.isEmpty()) return;
        aegis::session::pin(repo_, item, paranoia_->isChecked());
        refreshHistory();
        trace_->appendPlainText("pin " + item);
    }

    void createWorktree() {
        const auto path = repo_ + "/.aegis/worktrees/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        const auto branch = "aegis/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QDir().mkpath(QFileInfo(path).path());
        const auto result = runCommand("git", {"worktree", "add", "-b", branch, path, "HEAD"}, repo_);
        terminal_->appendPlainText("\n[worktree]\n" + result.output);
        trace_->appendPlainText("worktree " + path);
        lastWorktree_ = path;
        aegis::session::appendEvent(repo_, "worktree", path, paranoia_->isChecked());
    }

    void worktreeAction() {
        const auto path = QInputDialog::getText(this, "Worktree action", "Worktree path:", QLineEdit::Normal, lastWorktree_);
        if (path.trimmed().isEmpty()) return;
        if (!QFileInfo(path).isDir()) {
            terminal_->appendPlainText("\n[worktree] directory not found: " + path + "\n");
            return;
        }
        bool ok = false;
        const auto action = QInputDialog::getItem(this, "Worktree action", "Action:", {"merge branch", "cherry-pick HEAD", "request revision", "discard"}, 0, false, &ok);
        if (!ok) return;
        if (action == "request revision") {
            if (!process_.isRunning()) startAgent();
            process_.write(("Review the worktree at " + path + " and request a targeted revision.\n").toUtf8());
            return;
        }
        if (action == "discard") {
            const auto result = runCommand("git", {"worktree", "remove", "--force", path}, repo_);
            terminal_->appendPlainText("\n[discard worktree]\n" + result.output);
            return;
        }
        const auto branch = runCommand("git", {"-C", path, "branch", "--show-current"}, repo_).output.trimmed();
        if (action == "merge branch") {
            const auto result = runCommand("git", {"merge", branch}, repo_);
            terminal_->appendPlainText("\n[merge worktree]\n" + result.output);
        } else {
            const auto commit = runCommand("git", {"-C", path, "rev-parse", "HEAD"}, repo_).output.trimmed();
            const auto result = runCommand("git", {"cherry-pick", commit}, repo_);
            terminal_->appendPlainText("\n[cherry-pick worktree]\n" + result.output);
        }
        aegis::session::appendEvent(repo_, "worktree-action", action + " " + path, paranoia_->isChecked());
    }

    void runLspCheck() {
        const auto file = report_.changedFiles.value(0);
        if (file.isEmpty()) return;
        const auto lsp = QStandardPaths::findExecutable(file.endsWith(".rs") ? "rust-analyzer" : "clangd");
        if (lsp.isEmpty()) {
            trace_->appendPlainText("LSP: no language server found");
            return;
        }
        QProcess server;
        server.setWorkingDirectory(repo_);
        server.start(lsp, {});
        if (!server.waitForStarted(3000)) {
            trace_->appendPlainText("LSP: failed to start " + lsp);
            return;
        }
        const auto uri = QUrl::fromLocalFile(QDir(repo_).filePath(file)).toString();
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
                                    {"params", QJsonObject{{"rootUri", QUrl::fromLocalFile(repo_).toString()}, {"capabilities", QJsonObject{}}}}});
        const auto initialized = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", QJsonObject{}}});
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"method", "textDocument/didOpen"},
                                    {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}, {"languageId", file.endsWith(".rs") ? "rust" : "cpp"}, {"version", 1}, {"text", readText(repo_, file)}}}}}});
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "textDocument/documentSymbol"},
                                    {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}}}});
        const auto symbols = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "textDocument/definition"},
                                    {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}, {"position", QJsonObject{{"line", 0}, {"character", 0}}}}}});
        const auto definition = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "textDocument/references"},
                                    {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}, {"position", QJsonObject{{"line", 0}, {"character", 0}}}, {"context", QJsonObject{{"includeDeclaration", true}}}}}});
        const auto references = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 5}, {"method", "textDocument/hover"},
                                    {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}, {"position", QJsonObject{{"line", 0}, {"character", 0}}}}}});
        const auto hover = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 6}, {"method", "workspace/symbol"},
                                    {"params", QJsonObject{{"query", report_.symbols.value(0).name}}}});
        const auto workspace = readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "shutdown"}, {"params", QJsonValue::Null}});
        readLsp(server);
        sendLsp(server, QJsonObject{{"jsonrpc", "2.0"}, {"method", "exit"}, {"params", QJsonValue::Null}});
        server.waitForFinished(1000);
        evidence_->appendPlainText("\nLSP initialize\n" + initialized + "\n\nLSP document symbols\n" + symbols +
                                   "\n\nLSP definition\n" + definition + "\n\nLSP references\n" + references +
                                   "\n\nLSP hover\n" + hover + "\n\nLSP workspace symbols\n" + workspace);
        aegis::session::appendEvent(repo_, "lsp", lsp + " symbols/definitions/references/hover", paranoia_->isChecked());
    }

    void runFormalCheck() {
        QString file;
        for (const auto &candidate : report_.changedFiles) {
            if (candidate.endsWith(".lean") || candidate.endsWith(".v") || candidate.endsWith(".smt2") || candidate.endsWith(".dafny")) {
                file = candidate;
                break;
            }
        }
        if (file.isEmpty()) {
            terminal_->appendPlainText("\n[formal] no Lean/Coq/SMT/Dafny file in the changeset\n");
            return;
        }
        QStringList command;
        if (file.endsWith(".lean")) command = {"lake", "env", "lean", file};
        else if (file.endsWith(".v")) command = {"coqc", file};
        else if (file.endsWith(".smt2")) command = {"z3", file};
        else command = {"dafny", "verify", file};
        if (QStandardPaths::findExecutable(command.first()).isEmpty()) {
            terminal_->appendPlainText("\n[formal] tool unavailable: " + command.first() + "\n");
            return;
        }
        const auto result = runCommand(command.first(), command.mid(1), repo_);
        terminal_->appendPlainText("\n[formal exit " + QString::number(result.exitCode) + "]\n" + result.output);
        aegis::session::appendEvent(repo_, "formal", command.join(' ') + " exit " + QString::number(result.exitCode), paranoia_->isChecked());
    }

    void runSolidityCheck() {
        const auto forge = QStandardPaths::findExecutable("forge");
        if (forge.isEmpty()) {
            terminal_->appendPlainText("\n[solidity] forge unavailable; showing static lens only\n" + lenses_->toPlainText());
            return;
        }
        const auto result = runCommand(forge, {"test"}, repo_);
        terminal_->appendPlainText("\n[forge exit " + QString::number(result.exitCode) + "]\n" + result.output);
        aegis::session::appendEvent(repo_, "solidity", "forge test exit " + QString::number(result.exitCode), paranoia_->isChecked());
    }

    void criticMode() {
        if (!process_.isRunning()) startAgent();
        const auto request = "Critic mode: inspect the current changeset and report correctness, security, API, and test risks. " + lastPrompt_ + "\n";
        process_.write(request.toUtf8());
        trace_->appendPlainText("critic request");
        aegis::session::appendEvent(repo_, "critic", "current changeset", paranoia_->isChecked());
    }

    void compareWithParent() {
        const auto current = git(repo_, {"diff", "HEAD", "--stat"});
        const auto previous = git(repo_, {"diff", "HEAD~1", "HEAD", "--stat"});
        evidence_->setPlainText("CURRENT WORKTREE\n" + current + "\nPARENT COMMIT\n" + previous);
        tabs_->setCurrentWidget(evidence_);
        aegis::session::appendEvent(repo_, "compare", "worktree vs parent", paranoia_->isChecked());
    }

    void timeMachine() {
        const auto commits = git(repo_, {"log", "--format=%h %s", "-20"});
        bool ok = false;
        const auto commit = QInputDialog::getText(this, "Time machine", "Commit (choose from the timeline):", QLineEdit::Normal, commits.section('\n', 0, 0).section(' ', 0, 0), &ok);
        if (!ok || commit.trimmed().isEmpty()) return;
        const auto snapshot = git(repo_, {"show", "--stat", "--oneline", "--decorate", commit});
        const auto change = git(repo_, {"show", "--format=", "--no-ext-diff", "--no-color", commit});
        evidence_->setPlainText("TIME MACHINE\n" + snapshot + "\n" + change);
        tabs_->setCurrentWidget(evidence_);
        aegis::session::appendEvent(repo_, "time-machine", commit, paranoia_->isChecked());
    }

    void handoffAgent() {
        bool ok = false;
        const auto target = QInputDialog::getItem(this, "Session handoff", "Agent:", {"shell", "codex", "claude", "opencode"}, 0, false, &ok);
        if (!ok || target == agent_->currentText()) return;
        if (process_.isRunning()) {
            process_.terminate();
        }
        agent_->setCurrentText(target);
        QTimer::singleShot(100, this, [this] {
            const auto context = "Handoff context. Original task: " + lastPrompt_ + "\nCurrent diff:\n" + diff() + "\nEvidence:\n" + evidence_->toPlainText() + "\n";
            process_.write(context.toUtf8());
            trace_->appendPlainText("handoff context sent");
        });
        aegis::session::appendEvent(repo_, "handoff", target, paranoia_->isChecked());
    }

    void compareAgents() {
        const auto task = QInputDialog::getText(this, "Multi-agent proposals", "Task:");
        if (task.trimmed().isEmpty()) return;
        QString output;
        for (const auto &agent : QStringList{"codex", "claude", "opencode"}) {
            const auto command = aegis::agentCommand(agent);
            if (QStandardPaths::findExecutable(command.first()).isEmpty()) continue;
            const auto path = repo_ + "/.aegis/proposals/" + agent + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
            QDir().mkpath(QFileInfo(path).path());
            const auto worktree = runCommand("git", {"worktree", "add", "--detach", path, "HEAD"}, repo_);
            if (worktree.exitCode != 0) { output += agent + ": worktree failed\n"; continue; }
            QProcess process;
            process.setWorkingDirectory(path);
            process.start(command.first(), command.mid(1));
            if (!process.waitForStarted(3000)) { output += agent + ": failed to start\n"; continue; }
            process.write((task + "\n").toUtf8());
            process.waitForFinished(30000);
            output += "PROPOSAL " + agent + "\n" + QString::fromLocal8Bit(process.readAllStandardOutput() + process.readAllStandardError()) + "\n";
            aegis::session::appendEvent(repo_, "proposal", agent + " " + path, paranoia_->isChecked());
        }
        evidence_->setPlainText(output.isEmpty() ? "No configured agent executable was found." : output);
        tabs_->setCurrentWidget(evidence_);
    }

    void showPalette() {
        const QStringList actions = {"Analyze evidence", "Review diff", "Run verification", "Create snapshot", "Create worktree", "Worktree action", "Run LSP check", "Run formal check", "Run Solidity tests", "Critic mode", "Compare agents", "Compare current/parent", "Time machine", "Handoff to agent", "Pin selected file", "Enable/disable paranoia", "Quick open", "Investigate", "Architecture"};
        bool ok = false;
        const auto action = QInputDialog::getItem(this, "Aegis command palette", "Action:", actions, 0, false, &ok);
        if (!ok) return;
        if (action == actions[0]) analyzeChanges();
        else if (action == actions[1]) refreshReview();
        else if (action == actions[2]) runVerification();
        else if (action == actions[3]) saveSnapshot();
        else if (action == actions[4]) createWorktree();
        else if (action == actions[5]) worktreeAction();
        else if (action == actions[6]) runLspCheck();
        else if (action == actions[7]) runFormalCheck();
        else if (action == actions[8]) runSolidityCheck();
        else if (action == actions[9]) criticMode();
        else if (action == actions[10]) compareAgents();
        else if (action == actions[11]) compareWithParent();
        else if (action == actions[12]) timeMachine();
        else if (action == actions[13]) handoffAgent();
        else if (action == actions[14]) pinSelected();
        else if (action == actions[15]) paranoia_->setChecked(!paranoia_->isChecked());
        else if (action == actions[16]) quickOpen();
        else if (action == actions[17]) openDeepView(evidence_, "investigate");
        else if (action == actions[18]) openDeepView(architecture_, "architecture");
    }

    QString repo_;
    QComboBox *agent_ = nullptr;
    QLabel *agentState_ = nullptr;
    QCheckBox *paranoia_ = nullptr;
    QLabel *status_ = nullptr;
    QWidget *reviewPanel_ = nullptr;
    QLabel *reviewTitle_ = nullptr;
    QLabel *reviewSummary_ = nullptr;
    QLabel *reviewChecks_ = nullptr;
    QPlainTextEdit *reviewDetails_ = nullptr;
    QListWidget *files_ = nullptr;
    QPlainTextEdit *terminal_ = nullptr;
    aegis::ui::CodeView *unified_ = nullptr;
    aegis::ui::CodeView *side_ = nullptr;
    QPlainTextEdit *trace_ = nullptr;
    QPlainTextEdit *evidence_ = nullptr;
    QPlainTextEdit *symbols_ = nullptr;
    QPlainTextEdit *architecture_ = nullptr;
    QPlainTextEdit *risk_ = nullptr;
    QPlainTextEdit *timeline_ = nullptr;
    QPlainTextEdit *board_ = nullptr;
    QPlainTextEdit *lenses_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QLineEdit *verifyCommand_ = nullptr;
    QLineEdit *prompt_ = nullptr;
    aegis::terminal::PtySession process_;
    QTimer timer_;
    bool statusRefreshInFlight_ = false;
    bool reviewRefreshInFlight_ = false;
    QString lastPrompt_;
    QString lastStatus_;
    QString lastWorktree_;
    aegis::analysis::Report report_;
};

int runCli(const QStringList &args) {
    const auto command = args.value(1);
    auto repo = QDir(command == "verify" ? "." : args.value(2, ".")).absolutePath();
    const auto requireRepo = [&repo] {
        if (QFileInfo(repo).isDir()) return true;
        QTextStream(stderr) << "aegis: not a repository directory: " << repo << '\n';
        return false;
    };
    if (command == "status") {
        if (!requireRepo()) return 2;
        const auto status = git(repo, {"status", "--short", "--branch"});
        const auto summary = aegis::summarizeGit(status, git(repo, {"diff", "HEAD", "--numstat"}));
        QTextStream(stdout) << QString("%1 files  +%2 -%3  branch %4\n%5")
                                   .arg(summary.files).arg(summary.insertions).arg(summary.deletions)
                                   .arg(summary.branch).arg(status);
        return 0;
    }
    if (command == "analyze" || command == "lens") {
        if (!requireRepo()) return 2;
        const auto report = aegis::analysis::analyze(repo, git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"}), "CLI");
        QTextStream(stdout) << (command == "lens" ? (report.solidity + report.binary + report.systems + report.formal).join('\n') : renderReport(report)) << '\n';
        return 0;
    }
    if (command == "history") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << aegis::session::events(repo).join('\n') << "\n\n" << git(repo, {"log", "--oneline", "--decorate", "-20"});
        return 0;
    }
    if (command == "board") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << aegis::session::board(repo).join('\n') << '\n';
        return 0;
    }
    if (command == "worktree") {
        if (!requireRepo()) return 2;
        const auto path = repo + "/.aegis/worktrees/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QDir().mkpath(QFileInfo(path).path());
        const auto result = runCommand("git", {"worktree", "add", "--detach", path, "HEAD"}, repo);
        QTextStream(stdout) << result.output << path << '\n';
        return result.exitCode;
    }
    if (command == "review") {
        if (!requireRepo()) return 2;
        QTextStream(stdout) << git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"});
        return 0;
    }
    if (command == "snapshot") {
        if (!requireRepo()) return 2;
        QDir().mkpath(repo + "/.aegis/snapshots");
        const auto path = repo + "/.aegis/snapshots/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".patch";
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
        file.write(git(repo, {"diff", "HEAD", "--no-ext-diff", "--no-color"}).toUtf8());
        QTextStream(stdout) << path << '\n';
        return 0;
    }
    if (command == "verify") {
        auto verify = args.mid(2);
        if (!verify.isEmpty() && verify.first() != "--") {
            repo = QDir(verify.takeFirst()).absolutePath();
        }
        if (!requireRepo()) return 2;
        if (!verify.isEmpty() && verify.first() == "--") verify.removeFirst();
        if (verify.isEmpty()) verify = {"ctest", "--test-dir", "build"};
        const auto result = runCommand(verify.first(), verify.mid(1), repo);
        QTextStream(stdout) << result.output;
        return result.exitCode < 0 ? 1 : result.exitCode;
    }
    return -1;
}

}

int main(int argc, char **argv) {
    const auto cliCommands = QStringList{"status", "review", "verify", "snapshot", "analyze", "lens", "history", "board", "worktree"};
    if (argc > 1 && cliCommands.contains(QString::fromLocal8Bit(argv[1]))) {
        QCoreApplication app(argc, argv);
        return runCli(app.arguments());
    }
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == "--help" || QString::fromLocal8Bit(argv[i]) == "-h") {
            QTextStream(stdout) << "Usage: aegis [path] [--agent shell|codex|claude|opencode] [--paranoia]\n"
                                   << "       aegis status|review|verify|snapshot|analyze|lens|history|board|worktree [path]\n"
                                   << "       aegis verify [path] -- command args...\n";
            return 0;
        }
    }

    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("A local-first control plane for coding agents");
    parser.addHelpOption();
    parser.addOption({{"a", "agent"}, "Agent to run: shell, codex, claude, or opencode", "agent", "shell"});
    parser.addOption({"paranoia", "Disable snapshot persistence and keep the session local"});
    parser.addPositionalArgument("path", "Repository path", ".");
    parser.process(app);
    const auto path = parser.positionalArguments().value(0, ".");
    auto agent = parser.value("agent");
    auto actualPath = path;
    if (agent == "shell" && QStringList{"codex", "claude", "opencode", "shell"}.contains(path.toLower())) {
        agent = path.toLower();
        actualPath = parser.positionalArguments().value(1, ".");
    }
    if (!QFileInfo(actualPath).isDir()) {
        QTextStream(stderr) << "aegis: not a repository directory: " << QDir(actualPath).absolutePath() << '\n';
        return 2;
    }
    Window window(actualPath, agent, parser.isSet("paranoia"));
    window.show();
    return app.exec();
}
