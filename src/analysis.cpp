#include "aegis/analysis.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QProcess>
#include <QStandardPaths>
#include <QSet>
#include <QTemporaryDir>

namespace aegis::analysis {
namespace {

QString relativePath(const QString &repo, const QString &path) {
    return QDir(repo).relativeFilePath(path).replace('\\', '/');
}

QStringList filesUnder(const QString &repo) {
    QStringList files;
    QDirIterator it(repo, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const auto path = it.next();
        const auto rel = relativePath(repo, path);
        if (rel.startsWith(".git/") || rel.startsWith(".aegis-git/") || rel.startsWith("build") || rel.startsWith(".aegis/") ||
            rel.contains("/node_modules/")) continue;
        files << rel;
    }
    return files;
}

QString readFile(const QString &repo, const QString &rel) {
    QFile file(QDir(repo).filePath(rel));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.read(4 * 1024 * 1024));
}

QStringList diffFiles(const QString &diff) {
    QStringList files;
    for (const auto &line : diff.split('\n')) {
        if (line.startsWith("+++ b/")) files << line.mid(6);
        else if (line.startsWith("diff --git a/")) {
            const auto parts = line.split(' ');
            if (parts.size() >= 4 && parts[3].startsWith("b/")) files << parts[3].mid(2);
        }
    }
    files.removeDuplicates();
    return files;
}

bool isTestFile(const QString &path) {
    const auto lower = path.toLower();
    return lower.contains("test") || lower.contains("spec") || lower.endsWith("_tests.cpp");
}

void addSemantic(Report &report, const QString &value) {
    if (!report.semanticChanges.contains(value)) report.semanticChanges << value;
}

void inspectSemanticDiff(Report &report, const QString &diff) {
    for (const auto &line : diff.split('\n')) {
        if (line.startsWith("+") && !line.startsWith("+++")) {
            if (line.contains(QRegularExpression("\\b(class|struct|enum|function|def)\\b")) || line.contains('('))
                addSemantic(report, "function or method added");
            if (line.contains(QRegularExpression("\\b(if|switch|case)\\b"))) addSemantic(report, "condition introduced");
            if (line.contains(QRegularExpression("\\b(for|while|foreach)\\b"))) addSemantic(report, "loop introduced");
            if (line.contains(QRegularExpression("\\b(throw|catch|except)\\b"))) addSemantic(report, "exception path changed");
            if (line.contains(QRegularExpression("\\b(push_back|append|insert|emplace|store|write)\\b"))) addSemantic(report, "state mutation added");
            if (line.contains(QRegularExpression("https?://|\\b(curl|socket|requests|fetch)\\b"))) addSemantic(report, "external API call added");
        } else if (line.startsWith("-") && !line.startsWith("---")) {
            if (line.contains('(')) addSemantic(report, "function or method removed");
        }
    }
}

void inspectSecurity(Report &report, const QString &file, const QString &text) {
    const QStringList patterns = {
        "hardcoded secret", "password|api[_-]?key|private[_-]?key\\s*[:=]",
        "dangerous C/C++ call", "\\b(strcpy|strcat|sprintf|gets)\\s*\\(",
        "unsafe command execution", "\\b(system|popen|eval|exec)\\s*\\(",
        "path traversal", "\\.\\./", "insecure temporary file", "\\b(mktemp|tmpnam)\\s*\\(",
        "unsafe deserialization", "\\b(deserialize|pickle\\.loads|yaml\\.load)\\s*\\("
    };
    for (int i = 0; i + 1 < patterns.size(); i += 2) {
        const auto match = QRegularExpression(patterns[i + 1], QRegularExpression::CaseInsensitiveOption).match(text);
        if (match.hasMatch()) report.findings << Finding{"high", "security", file, int(text.left(match.capturedStart()).count('\n') + 1), patterns[i]};
    }
}

void inspectSymbols(Report &report, const QString &repo, const QStringList &files) {
    const auto allFiles = filesUnder(repo);
    QMap<QString, QString> contents;
    for (const auto &file : allFiles) contents[file] = readFile(repo, file);
    const QRegularExpression declaration(
        "^\\s*(?:class|struct|enum|namespace|def|function)\\s+([A-Za-z_]\\w*)|^\\s*(?:[A-Za-z_][\\w:<>&*~]*\\s+)+([A-Za-z_]\\w*)\\s*\\(",
        QRegularExpression::MultilineOption);
    for (const auto &file : files) {
        const auto text = contents.value(file);
        auto it = declaration.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            const auto name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
            if (name.isEmpty() || name == "if" || name == "for" || name == "while") continue;
            int references = 0;
            QStringList callers;
            for (auto all = contents.cbegin(); all != contents.cend(); ++all)
                if (all.value().contains(QRegularExpression("\\b" + QRegularExpression::escape(name) + "\\b"))) {
                    references += all.value().count(QRegularExpression("\\b" + QRegularExpression::escape(name) + "\\b"));
                    if (all.key() != file) callers << all.key();
                }
            const auto line = text.left(match.capturedStart()).count('\n') + 1;
            QStringList callees;
            const auto end = text.indexOf('}', match.capturedStart());
            const auto body = text.mid(match.capturedStart(), (end < 0 ? text.size() : end) - match.capturedStart());
            auto calls = QRegularExpression("\\b([A-Za-z_]\\w*)\\s*\\(").globalMatch(body);
            while (calls.hasNext()) {
                const auto called = calls.next().captured(1);
                if (called != name && !QStringList{"if", "for", "while", "switch", "catch"}.contains(called) && !callees.contains(called)) callees << called;
            }
            report.symbols << SymbolInfo{name, file, int(line), qMax(0, references - 1), callers, callees};
            for (const auto &callee : callees) report.callGraph << name + " -> " + callee;
            report.blastRadius << QString("%1: %2 references, %3 modules, %4 tests")
                                      .arg(name).arg(qMax(0, references - 1)).arg(contents.size()).arg(
                                          std::count_if(allFiles.cbegin(), allFiles.cend(), [](const auto &path) { return isTestFile(path); }));
        }
    }
}

void inspectCompilerAst(Report &report, const QString &repo, const QStringList &files) {
    const auto compiler = QStandardPaths::findExecutable("g++");
    if (compiler.isEmpty()) return;
    QTemporaryDir scratch;
    if (!scratch.isValid()) return;
    QStringList projectFlags;
    const auto pkgConfig = QStandardPaths::findExecutable("pkg-config");
    if (!pkgConfig.isEmpty()) {
        QProcess flags;
        flags.start(pkgConfig, {"--cflags", "Qt6Core", "Qt6Widgets"});
        if (flags.waitForFinished(3000) && flags.exitCode() == 0)
            projectFlags = QProcess::splitCommand(QString::fromLocal8Bit(flags.readAllStandardOutput()).trimmed());
    }
    int index = 0;
    for (const auto &file : files) {
        if (!QRegularExpression("\\.(c|cc|cpp|cxx)$", QRegularExpression::CaseInsensitiveOption).match(file).hasMatch()) continue;
        const auto dump = scratch.path() + "/" + QString::number(index++) + ".tree";
        QProcess process;
        process.setWorkingDirectory(repo);
        QStringList arguments = {"-std=c++23", "-fsyntax-only", "-fno-diagnostics-color", "-I" + repo,
                                 "-I" + QDir(repo).filePath("include")};
        arguments += projectFlags;
        arguments += {"-fdump-tree-original=" + dump, QDir(repo).filePath(file)};
        process.start(compiler, arguments);
        if (!process.waitForFinished(30000) || process.exitCode() != 0) {
            process.kill();
            continue;
        }
        QFile tree(dump);
        if (!tree.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const auto text = QString::fromUtf8(tree.read(16 * 1024 * 1024));
        auto sections = QRegularExpression("^;; Function ([^\\n]+)\\n(.*?)(?=^;; Function |\\z)",
                                            QRegularExpression::DotMatchesEverythingOption | QRegularExpression::MultilineOption).globalMatch(text);
        while (sections.hasNext()) {
            const auto match = sections.next();
            const auto function = match.captured(1).trimmed();
            if (function.isEmpty() || function.contains("std::") || function.contains("QtPrivate::")) continue;
            report.ast << file + ": " + function;
            const auto beforeArguments = function.section('(', 0, 0).trimmed();
            const auto functionName = beforeArguments.section(' ', -1).remove(QRegularExpression("[*&]"));
            auto calls = QRegularExpression("\\b([A-Za-z_]\\w*(?:::[A-Za-z_]\\w*)?)\\s*\\(").globalMatch(match.captured(2));
            while (calls.hasNext()) {
                const auto callee = calls.next().captured(1);
                if (!QStringList{"if", "for", "while", "switch", "return", "sizeof"}.contains(callee) &&
                    !callee.startsWith("std::") && !callee.startsWith("__"))
                    report.callGraph << functionName + " -> " + callee;
            }
        }
    }
    report.ast.removeDuplicates();
    report.callGraph.removeDuplicates();
}

void inspectDependencies(Report &report, const QStringList &files) {
    const QSet<QString> manifests = {"package.json", "package-lock.json", "Cargo.toml", "Cargo.lock", "go.mod", "go.sum",
                                     "requirements.txt", "pyproject.toml", "poetry.lock", "CMakeLists.txt", "conanfile", "vcpkg.json",
                                     "foundry.toml", "pom.xml", "build.gradle"};
    for (const auto &file : files) {
        const auto name = QFileInfo(file).fileName();
        if (manifests.contains(name)) report.dependencies << file;
    }
}

void inspectArchitecture(Report &report, const QString &repo, const QStringList &files) {
    for (const auto &file : files) {
        const auto text = readFile(repo, file);
        for (const auto &line : text.split('\n')) {
            const auto include = QRegularExpression("#include\\s*[<\"]([^>\"]+)").match(line);
            const auto import = QRegularExpression("(?:import|from)\\s+([A-Za-z0-9_./-]+)").match(line);
            if (include.hasMatch()) report.architectureEdges << file + " -> " + include.captured(1);
            else if (import.hasMatch()) report.architectureEdges << file + " -> " + import.captured(1);
        }
    }
    report.architectureEdges.removeDuplicates();
}

void inspectLenses(Report &report, const QString &repo, const QStringList &allFiles) {
    for (const auto &file : allFiles) {
        const auto text = readFile(repo, file);
        if (file.endsWith(".sol", Qt::CaseInsensitive)) {
            if (text.contains(QRegularExpression("\\bcontract\\s+\\w+"))) report.solidity << file + ": contract map";
            if (text.contains("delegatecall")) report.solidity << file + ": delegatecall";
            if (text.contains("tx.origin")) report.solidity << file + ": tx.origin";
            if (text.contains(QRegularExpression("\\bfunction\\s+\\w+"))) report.solidity << file + ": functions";
            if (text.contains(QRegularExpression("\\b(address|uint|bytes)\\s+\\w+\\s*;"))) report.solidity << file + ": storage candidates";
            if (text.contains("call(")) report.solidity << file + ": low-level external call";
        }
        QFile binary(QDir(repo).filePath(file));
        const auto extensionBinary = QRegularExpression("\\.(so|dll|dylib|exe|bin)$", QRegularExpression::CaseInsensitiveOption).match(file).hasMatch();
        QByteArray header;
        if (binary.open(QIODevice::ReadOnly)) header = binary.read(4);
        const auto magicBinary = header.startsWith(QByteArray::fromHex("7f454c46")) || header.startsWith("MZ") ||
                                 header.startsWith(QByteArray::fromHex("cffaedfe")) || header.startsWith(QByteArray::fromHex("feedfacf"));
        if (extensionBinary || magicBinary) {
            if (binary.isOpen()) {
                binary.seek(0);
                const auto data = binary.read(4 * 1024 * 1024);
                QString kind = "binary";
                if (data.startsWith(QByteArray::fromHex("7f454c46"))) kind = "ELF";
                else if (data.startsWith("MZ")) kind = "PE";
                else if (data.startsWith(QByteArray::fromHex("cffaedfe")) || data.startsWith(QByteArray::fromHex("feedfacf"))) kind = "Mach-O";
                report.binary << file + ": " + kind + " sha256=" + QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
                for (const auto &tool : QStringList{"file", "readelf", "nm", "strings"}) {
                    if (QStandardPaths::findExecutable(tool).isEmpty()) continue;
                    QStringList arguments;
                    if (tool == "readelf") arguments = {"-h", QDir(repo).filePath(file)};
                    else if (tool == "nm") arguments = {"-D", "--defined-only", QDir(repo).filePath(file)};
                    else if (tool == "strings") arguments = {"-n", "4", QDir(repo).filePath(file)};
                    else arguments = {QDir(repo).filePath(file)};
                    QProcess process;
                    process.start(tool, arguments);
                    if (!process.waitForFinished(1500)) continue;
                    const auto output = QString::fromLocal8Bit(process.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts).mid(0, 5);
                    if (!output.isEmpty()) report.binary << file + ": " + tool + " " + output.join(" | ");
                }
            }
        }
        if (QRegularExpression("\\.(c|cc|cpp|h|hpp|rs|go)$", QRegularExpression::CaseInsensitiveOption).match(file).hasMatch()) {
            const QStringList patterns = {"fork", "exec", "pthread", "SIG[A-Z]+", "malloc", "free", "strace", "gdb"};
            for (const auto &pattern : patterns)
                if (text.contains(QRegularExpression("\\b" + pattern + "\\b"))) report.systems << file + ": " + pattern;
        }
        if (QRegularExpression("\\.(lean|v|smt2|smt|why|dafny)$", QRegularExpression::CaseInsensitiveOption).match(file).hasMatch()) {
            const auto count = text.count(QRegularExpression("\\b(theorem|lemma|invariant|assert|check)\\b"));
            report.formal << file + ": " + QString::number(count) + " proof/check declarations";
        }
    }
}

}

Report analyze(const QString &repo, const QString &diff, const QString &agent) {
    Report report;
    report.changedFiles = diffFiles(diff);
    inspectSemanticDiff(report, diff);
    for (const auto &file : report.changedFiles) {
        report.provenance << agent + " -> " + file;
        report.risk[file] = 1;
        inspectSecurity(report, file, readFile(repo, file));
    }
    inspectSymbols(report, repo, report.changedFiles);
    inspectCompilerAst(report, repo, report.changedFiles);
    inspectDependencies(report, report.changedFiles);
    inspectArchitecture(report, repo, filesUnder(repo));
    const auto allFiles = filesUnder(repo);
    inspectLenses(report, repo, allFiles);
    for (const auto &file : allFiles) {
        if (isTestFile(file)) {
            const auto text = readFile(repo, file);
            for (const auto &symbol : report.symbols)
                if (text.contains(symbol.name) && !report.impactedTests.contains(file)) report.impactedTests << file;
        }
    }
    for (const auto &finding : report.findings) report.risk[finding.file] += 3;
    for (const auto &symbol : report.symbols) report.risk[symbol.file] += symbol.references;
    for (auto it = report.risk.begin(); it != report.risk.end(); ++it) it.value() = qMax(1, it.value());
    return report;
}

}
