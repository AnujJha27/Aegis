#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace aegis::analysis {

struct Finding {
    QString severity;
    QString category;
    QString file;
    int line = 0;
    QString message;
};

struct SymbolInfo {
    QString name;
    QString file;
    int line = 0;
    int references = 0;
    QStringList callers;
    QStringList callees;
};

struct Report {
    QStringList changedFiles;
    QStringList semanticChanges;
    QStringList blastRadius;
    QStringList provenance;
    QStringList dependencies;
    QStringList impactedTests;
    QStringList architectureEdges;
    QStringList ast;
    QStringList callGraph;
    QStringList solidity;
    QStringList binary;
    QStringList systems;
    QStringList formal;
    QList<SymbolInfo> symbols;
    QList<Finding> findings;
    QMap<QString, int> risk;
};

Report analyze(const QString &repo, const QString &diff, const QString &agent);

}
