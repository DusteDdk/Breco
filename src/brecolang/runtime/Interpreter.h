#pragma once

#include <QVector>

#include <memory>

#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/DecodedData.h"

QT_BEGIN_NAMESPACE
class QIODevice;
QT_END_NAMESPACE

namespace breco::lang {

enum class DecodeMode {
    Tree,
    Probe,
    Streaming,
};

enum class DecodeStatus {
    Success,
    NoMatch,
    Error,
};

struct RuntimeDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    QString code;
    QString message;
    SourceSpan schemaSpan;
    ByteSpanValue inputSpan;
    bool hasInputSpan = false;
};

struct DecodeRequest {
    std::shared_ptr<const BrecoProgram> program;
    QString entryName;
    QVector<std::shared_ptr<ByteSource>> inputs;
    DecodeMode mode = DecodeMode::Tree;
    quint64 startOffset = 0;
    QIODevice* output = nullptr;
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Error;
    std::shared_ptr<const DecodedTree> tree;
    QVector<RuntimeDiagnostic> diagnostics;
    DecodedValueId rootValue = kInvalidId;
    InputId entryInput = kInvalidId;
    quint64 startOffset = 0;
    quint64 endOffset = 0;
    quint64 constructedNodes = 0;

    bool success() const { return status == DecodeStatus::Success; }
};

DecodeResult decodeBrecoProgram(const DecodeRequest& request);

}  // namespace breco::lang
