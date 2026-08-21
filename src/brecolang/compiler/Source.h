#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

namespace breco::lang {

struct SourceSpan {
    qsizetype start = 0;
    qsizetype length = 0;

    qsizetype end() const { return start + length; }
    bool isValid() const { return start >= 0 && length >= 0; }
};

enum class DiagnosticSeverity {
    Error,
    Warning,
};

struct RelatedDiagnostic {
    SourceSpan span;
    QString message;
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    QString code;
    QString message;
    SourceSpan span;
    QVector<RelatedDiagnostic> related;
};

}  // namespace breco::lang
