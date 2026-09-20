#include "aegis/core.h"
#include "aegis/analysis.h"
#include "aegis/commands.h"
#include "aegis/session.h"
#include "aegis/terminal.h"
#include "aegis/transcript.h"
#include "aegis/ui.h"
#include "aegis/window.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QShortcut>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <memory>

namespace {

using aegis::app::CommandResult;
using aegis::app::git;
using aegis::app::runCommand;

QString readText(const QString &repo, const QString &file) {
    QFile input(QDir(repo).filePath(file));
    if (!input.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(input.read(4 * 1024 * 1024));
}

QStringList changedFiles(const QString &status) {
    QStringList files;
    for (const auto &line : status.split('\n', Qt::SkipEmptyParts))
        if (!line.startsWith("## ") && line.size() > 3) files << line.mid(3);
    return files;
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
        const auto length = QRegularExpression("Content-Length: (\\d+)", QRegularExpression::CaseInsensitiveOption)
                                .match(QString::fromLatin1(header));
        if (!length.hasMatch()) return QString::fromUtf8(data);
        const int bodyStart = headerEnd + 4;
        if (data.size() >= bodyStart + length.captured(1).toInt())
            return QString::fromUtf8(data.mid(bodyStart, length.captured(1).toInt()));
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
        brand_ = new QLabel(root);
        brand_->setObjectName("brand");
        brand_->setText(aegis::ui::sessionHeader(QFileInfo(repo_).fileName(), selectedAgent.toUpper()));
        header->addWidget(brand_, 1);
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
        terminal_ = new QTextBrowser(split);
        terminal_->setReadOnly(true);
        terminal_->setPlaceholderText("Managed agent terminal output");
        terminal_->setOpenExternalLinks(false);

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
        activity_ = new QPlainTextEdit(tabs_);
        activity_->setReadOnly(true);
        tabs_->addTab(unified_, "Unified diff");
        tabs_->addTab(evidence_, "Evidence");
        tabs_->addTab(symbols_, "Symbols");
        tabs_->addTab(architecture_, "Architecture");
        tabs_->addTab(risk_, "Risk");
        tabs_->addTab(timeline_, "Timeline");
        tabs_->addTab(board_, "Board");
        tabs_->addTab(lenses_, "Lenses");
        tabs_->addTab(activity_, "Activity");
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

        busyOverlay_ = new QFrame(root);
        busyOverlay_->setObjectName("busyOverlay");
        auto *busyLayout = new QVBoxLayout(busyOverlay_);
        busyLayout->setAlignment(Qt::AlignCenter);
        busyLabel_ = new QLabel("Working…", busyOverlay_);
        busyLabel_->setObjectName("busyLabel");
        busyProgress_ = new QProgressBar(busyOverlay_);
        busyProgress_->setRange(0, 0);
        busyProgress_->setFixedWidth(280);
        busyLayout->addWidget(busyLabel_, 0, Qt::AlignCenter);
        busyLayout->addWidget(busyProgress_, 0, Qt::AlignCenter);
        busyOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
        busyOverlay_->setGeometry(root->rect());
        busyOverlay_->hide();

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

        connect(agent_, &QComboBox::currentTextChanged, this, [this](const QString &agent) { switchAgent(agent); });
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
        process_.onOutput = [this](const QByteArray &data) {
            if (codexSessionActive_) {
                activity_->moveCursor(QTextCursor::End);
                activity_->insertPlainText(aegis::ui::cleanTerminalText(data));
                activity_->ensureCursorVisible();
                for (const auto &event : codex_.feed(data)) appendTranscriptCard(event.label, event.body);
            }
            else appendTerminal(data);
        };
        process_.onError = [this](const QString &error) { appendTerminal(("\n[aegis] " + error + "\n").toUtf8()); };
        process_.onFinished = [this](int code) {
            if (codexSessionActive_) {
                for (const auto &event : codex_.finish()) appendTranscriptCard(event.label, event.body);
                if (code != 0) appendTranscriptCard("CODEX ERROR", "Codex exited with code " + QString::number(code));
            }
            trace_->appendPlainText("agent exited with code " + QString::number(code));
            agentState_->setText(agent_->currentText().compare("codex", Qt::CaseInsensitive) == 0 ? "● READY" : "● IDLE");
            codexSessionActive_ = false;
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

    ~Window() override { process_.terminate(); }

private:
    void closeEvent(QCloseEvent *event) override {
        process_.terminate();
        QMainWindow::closeEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override {
        QMainWindow::resizeEvent(event);
        if (busyOverlay_ && centralWidget()) busyOverlay_->setGeometry(centralWidget()->rect());
    }

    void appendTerminal(const QByteArray &data) {
        const auto text = aegis::ui::cleanTerminalText(data);
        if (text.isEmpty()) return;
        activity_->moveCursor(QTextCursor::End);
        activity_->insertPlainText(text);
        activity_->ensureCursorVisible();
        terminal_->moveCursor(QTextCursor::End);
        terminal_->insertPlainText(text);
        terminal_->ensureCursorVisible();
    }

    void appendTranscriptCard(const QString &label, const QString &body) {
        if (body.trimmed().isEmpty()) return;
        const auto escaped = body.toHtmlEscaped().replace('\n', "<br>");
        terminal_->moveCursor(QTextCursor::End);
        terminal_->insertHtml(QString("<p style='margin: 12px 0 16px 0;'><span style='color:#6EA8FE; font-weight:600;'>%1</span><br><span style='color:#D6DEE8;'>%2</span></p>")
                                  .arg(label.toHtmlEscaped(), escaped));
        terminal_->insertPlainText("\n");
        terminal_->ensureCursorVisible();
    }

    void beginBusy(const QString &message) {
        ++busyDepth_;
        busyLabel_->setText(message);
        busyOverlay_->setGeometry(centralWidget()->rect());
        busyOverlay_->raise();
        busyOverlay_->show();
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    void endBusy() {
        if (busyDepth_ <= 0 || --busyDepth_ > 0) return;
        busyOverlay_->hide();
    }

    CommandResult runBlocking(const QString &program, const QStringList &arguments, const QString &message) {
        beginBusy(message);
        const auto result = runCommand(program, arguments, repo_);
        endBusy();
        return result;
    }

    void startAgent() {
        if (process_.isRunning()) return;
        if (agent_->currentText().compare("codex", Qt::CaseInsensitive) == 0) {
            codex_.reset();
            agentState_->setText("● READY");
            trace_->appendPlainText("CODEX exec mode ready");
            return;
        }
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

    void switchAgent(const QString &agent) {
        beginBusy("Switching to " + agent.toUpper() + "…");
        agent_->setEnabled(false);
        QTimer::singleShot(0, this, [this, agent] {
            if (process_.isRunning()) {
                trace_->appendPlainText("switching agent to " + agent.toUpper());
                appendTerminal(("\n[aegis] switching agent to " + agent + "\n").toUtf8());
                process_.terminate();
            }
            codexSessionActive_ = false;
            codex_.reset();
            brand_->setText(aegis::ui::sessionHeader(QFileInfo(repo_).fileName(), agent.toUpper()));
            startAgent();
            agent_->setEnabled(true);
            endBusy();
        });
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
        if (agent_->currentText().compare("codex", Qt::CaseInsensitive) == 0) {
            sendCodexPrompt(text);
            return;
        }
        if (!process_.isRunning()) startAgent();
        const auto line = (text.startsWith("revise:", Qt::CaseInsensitive)
                               ? "Please revise the current changeset: " + text.mid(7).trimmed()
                               : text) + "\n";
        process_.write(line.toUtf8());
        appendTranscriptCard("YOU", line.trimmed());
        trace_->appendPlainText(aegis::ui::activityLine("prompt", line.trimmed(), true));
        aegis::session::appendEvent(repo_, "prompt", line.trimmed(), paranoia_->isChecked());
        prompt_->clear();
    }

    void sendCodexPrompt(const QString &text) {
        if (process_.isRunning()) {
            appendTranscriptCard("SYSTEM", "Codex is still processing the previous prompt.");
            return;
        }
        const auto command = codex_.threadId().isEmpty()
                                 ? QStringList{"exec", "--json", "--color", "never", text}
                                 : QStringList{"exec", "resume", "--json", codex_.threadId(), text};
        codexSessionActive_ = true;
        agentState_->setText("● RUNNING");
        trace_->appendPlainText(aegis::ui::activityLine("prompt", text, true));
        trace_->appendPlainText("start codex " + command.join(' '));
        appendTranscriptCard("YOU", text);
        if (!process_.start("codex", command, repo_)) {
            agentState_->setText("● READY");
            appendTranscriptCard("SYSTEM", "Failed to start Codex exec.");
            return;
        }
        aegis::session::appendEvent(repo_, "prompt", text, paranoia_->isChecked());
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

    QString diff() {
        auto result = runBlocking("git", {"diff", "HEAD", "--no-ext-diff", "--no-color"}, "Loading diff…").output;
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
        });
    }

    void runGitAsync(const QStringList &arguments, std::function<void(QString)> callback) {
        auto *process = new QProcess(this);
        process->setWorkingDirectory(repo_);
        if (!arguments.contains("-C")) {
            const auto privateGit = QDir(repo_).filePath(".aegis-git");
            if (QFileInfo(privateGit).isDir()) {
                auto environment = QProcessEnvironment::systemEnvironment();
                environment.insert("GIT_DIR", privateGit);
                environment.insert("GIT_WORK_TREE", repo_);
                process->setProcessEnvironment(environment);
            }
        }
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
        const auto result = runBlocking(command.first(), command.mid(1), "Running verification…");
        appendTranscriptCard("VERIFY", QString("exit %1\n%2").arg(result.exitCode).arg(result.output));
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
            appendTranscriptCard("SYSTEM", "Snapshot skipped: Paranoia Mode");
            return;
        }
        const auto directory = repo_ + "/.aegis/snapshots";
        QDir().mkpath(directory);
        const auto path = directory + "/" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".patch";
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            file.write(diff().toUtf8());
            appendTranscriptCard("SNAPSHOT", path);
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
        beginBusy("Analyzing changes…");
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
        endBusy();
    }

    void refreshHistory() {
        auto history = aegis::session::events(repo_);
        history << "\nGIT HISTORY\n" + runBlocking("git", {"log", "--oneline", "--decorate", "-20"}, "Loading history…").output;
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
        const auto result = runBlocking("git", {"worktree", "add", "-b", branch, path, "HEAD"}, "Creating worktree…");
        appendTranscriptCard("WORKTREE", result.output);
        trace_->appendPlainText("worktree " + path);
        lastWorktree_ = path;
        aegis::session::appendEvent(repo_, "worktree", path, paranoia_->isChecked());
    }

    void worktreeAction() {
        const auto path = QInputDialog::getText(this, "Worktree action", "Worktree path:", QLineEdit::Normal, lastWorktree_);
        if (path.trimmed().isEmpty()) return;
        if (!QFileInfo(path).isDir()) {
            appendTranscriptCard("WORKTREE ERROR", "Directory not found: " + path);
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
            const auto result = runBlocking("git", {"worktree", "remove", "--force", path}, "Removing worktree…");
            appendTranscriptCard("WORKTREE", result.output);
            return;
        }
        const auto branch = runBlocking("git", {"-C", path, "branch", "--show-current"}, "Reading worktree…").output.trimmed();
        if (action == "merge branch") {
            const auto result = runBlocking("git", {"merge", branch}, "Merging worktree…");
            appendTranscriptCard("MERGE", result.output);
        } else {
            const auto commit = runBlocking("git", {"-C", path, "rev-parse", "HEAD"}, "Reading worktree commit…").output.trimmed();
            const auto result = runBlocking("git", {"cherry-pick", commit}, "Applying worktree commit…");
            appendTranscriptCard("CHERRY-PICK", result.output);
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
        beginBusy("Querying language server…");
        QProcess server;
        server.setWorkingDirectory(repo_);
        server.start(lsp, {});
        if (!server.waitForStarted(3000)) {
            trace_->appendPlainText("LSP: failed to start " + lsp);
            endBusy();
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
        endBusy();
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
            appendTranscriptCard("FORMAL", "No Lean/Coq/SMT/Dafny file in the changeset.");
            return;
        }
        QStringList command;
        if (file.endsWith(".lean")) command = {"lake", "env", "lean", file};
        else if (file.endsWith(".v")) command = {"coqc", file};
        else if (file.endsWith(".smt2")) command = {"z3", file};
        else command = {"dafny", "verify", file};
        if (QStandardPaths::findExecutable(command.first()).isEmpty()) {
            appendTranscriptCard("FORMAL", "Tool unavailable: " + command.first());
            return;
        }
        const auto result = runBlocking(command.first(), command.mid(1), "Running formal check…");
        appendTranscriptCard("FORMAL", "exit " + QString::number(result.exitCode) + "\n" + result.output);
        aegis::session::appendEvent(repo_, "formal", command.join(' ') + " exit " + QString::number(result.exitCode), paranoia_->isChecked());
    }

    void runSolidityCheck() {
        const auto forge = QStandardPaths::findExecutable("forge");
        if (forge.isEmpty()) {
            appendTranscriptCard("SOLIDITY", "Forge unavailable; showing static lens only\n" + lenses_->toPlainText());
            return;
        }
        const auto result = runBlocking(forge, {"test"}, "Running Solidity tests…");
        appendTranscriptCard("SOLIDITY", "Forge exit " + QString::number(result.exitCode) + "\n" + result.output);
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
        const auto current = runBlocking("git", {"diff", "HEAD", "--stat"}, "Comparing changes…").output;
        const auto previous = runBlocking("git", {"diff", "HEAD~1", "HEAD", "--stat"}, "Loading parent changes…").output;
        evidence_->setPlainText("CURRENT WORKTREE\n" + current + "\nPARENT COMMIT\n" + previous);
        tabs_->setCurrentWidget(evidence_);
        aegis::session::appendEvent(repo_, "compare", "worktree vs parent", paranoia_->isChecked());
    }

    void timeMachine() {
        const auto commits = runBlocking("git", {"log", "--format=%h %s", "-20"}, "Loading commits…").output;
        bool ok = false;
        const auto commit = QInputDialog::getText(this, "Time machine", "Commit (choose from the timeline):", QLineEdit::Normal, commits.section('\n', 0, 0).section(' ', 0, 0), &ok);
        if (!ok || commit.trimmed().isEmpty()) return;
        const auto snapshot = runBlocking("git", {"show", "--stat", "--oneline", "--decorate", commit}, "Loading commit…").output;
        const auto change = runBlocking("git", {"show", "--format=", "--no-ext-diff", "--no-color", commit}, "Loading commit diff…").output;
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
        beginBusy("Comparing agents…");
        QString output;
        for (const auto &agent : QStringList{"codex", "claude", "opencode"}) {
            const auto command = aegis::agentCommand(agent);
            if (QStandardPaths::findExecutable(command.first()).isEmpty()) continue;
            const auto path = repo_ + "/.aegis/proposals/" + agent + "-" + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
            QDir().mkpath(QFileInfo(path).path());
            const auto worktree = runBlocking("git", {"worktree", "add", "--detach", path, "HEAD"}, "Preparing " + agent.toUpper() + "…");
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
        endBusy();
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
    QLabel *brand_ = nullptr;
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
    QTextBrowser *terminal_ = nullptr;
    aegis::ui::CodeView *unified_ = nullptr;
    QFrame *busyOverlay_ = nullptr;
    QLabel *busyLabel_ = nullptr;
    QProgressBar *busyProgress_ = nullptr;
    QPlainTextEdit *trace_ = nullptr;
    QPlainTextEdit *evidence_ = nullptr;
    QPlainTextEdit *symbols_ = nullptr;
    QPlainTextEdit *architecture_ = nullptr;
    QPlainTextEdit *risk_ = nullptr;
    QPlainTextEdit *timeline_ = nullptr;
    QPlainTextEdit *board_ = nullptr;
    QPlainTextEdit *lenses_ = nullptr;
    QPlainTextEdit *activity_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QLineEdit *verifyCommand_ = nullptr;
    QLineEdit *prompt_ = nullptr;
    aegis::terminal::PtySession process_;
    QTimer timer_;
    bool statusRefreshInFlight_ = false;
    bool reviewRefreshInFlight_ = false;
    int busyDepth_ = 0;
    QString lastPrompt_;
    QString lastStatus_;
    QString lastWorktree_;
    aegis::transcript::CodexStream codex_;
    aegis::analysis::Report report_;
    bool codexSessionActive_ = false;
};

}

QMainWindow *aegis::app::createWindow(const QString &repo, const QString &selectedAgent, bool paranoia) {
    return new Window(repo, selectedAgent, paranoia);
}
