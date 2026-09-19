#include "aegis/analysis.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace aegis::analysis;

static void writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    Q_ASSERT(file.open(QIODevice::WriteOnly));
    Q_ASSERT(file.write(data) == data.size());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir repo;
    Q_ASSERT(repo.isValid());
    QDir().mkpath(repo.path() + "/src");
    QDir().mkpath(repo.path() + "/test");
    writeFile(repo.path() + "/src/auth.h", "struct User {};\n");
    writeFile(repo.path() + "/src/auth.cpp", "#include <cstring>\n#include \"auth.h\"\nint helper();\nint authenticateUser(User user) { char buf[16]; const char *input = \"\"; strcpy(buf, input); return helper(); }\nint helper() { return 1; }\n");
    writeFile(repo.path() + "/test/auth_test.cpp", "#include \"../src/auth.cpp\"\nvoid test_authenticateUser() {}\n");
    writeFile(repo.path() + "/package.json", "{\"dependencies\":{\"new-lib\":\"1.0\"}}\n");
    writeFile(repo.path() + "/Vault.sol", "contract Vault { address owner; function withdraw() public { msg.sender.call(\"\"); } }\n");
    writeFile(repo.path() + "/proof.lean", "theorem safe : True := by trivial\n");
    writeFile(repo.path() + "/mystery.bin", QByteArray::fromHex("7f454c46") + "binary-data");

    const auto report = analyze(
        repo.path(),
        "diff --git a/src/auth.cpp b/src/auth.cpp\n+++ b/src/auth.cpp\n@@\n+int authenticateUser(User user) { return 1; }\n+strcpy(buf, input);\n",
        "Codex");

    Q_ASSERT(report.changedFiles.contains("src/auth.cpp"));
    Q_ASSERT(report.semanticChanges.contains("function or method added"));
    Q_ASSERT(!report.symbols.isEmpty());
    const auto auth = std::find_if(report.symbols.cbegin(), report.symbols.cend(), [](const auto &symbol) { return symbol.name == "authenticateUser"; });
    Q_ASSERT(auth != report.symbols.cend());
    Q_ASSERT(auth->references >= 1);
    Q_ASSERT(auth->callers.contains("test/auth_test.cpp"));
    Q_ASSERT(!report.ast.isEmpty());
    Q_ASSERT(!report.callGraph.isEmpty());
    Q_ASSERT(!report.blastRadius.isEmpty());
    Q_ASSERT(report.provenance.contains("Codex -> src/auth.cpp"));
    Q_ASSERT(!report.findings.isEmpty());
    Q_ASSERT(report.dependencies.contains("package.json"));
    Q_ASSERT(report.impactedTests.contains("test/auth_test.cpp"));
    Q_ASSERT(!report.architectureEdges.isEmpty());
    Q_ASSERT(!report.solidity.isEmpty());
    Q_ASSERT(!report.formal.isEmpty());
    Q_ASSERT(!report.binary.isEmpty());
    Q_ASSERT(report.risk.value("src/auth.cpp") > 0);
    return 0;
}
