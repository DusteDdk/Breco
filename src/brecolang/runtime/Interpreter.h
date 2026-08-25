#pragma once

#include <QVector>

#include <memory>
#include <limits>

#include "brecolang/runtime/ByteSource.h"
#include "brecolang/runtime/DecodedData.h"
#include "brecolang/runtime/DecodeTypes.h"

QT_BEGIN_NAMESPACE
class QIODevice;
QT_END_NAMESPACE

namespace breco::lang {

enum class DecodeMode {
    Tree,
    Probe,
    Streaming,
    ResolveShape,
    MaterializePage,
    ReferenceScan,
};

enum class DecodeStatus {
    Success,
    Paused,
    NoMatch,
    Error,
    Invalidated,
    Cancelled,
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
    std::optional<quint64> entryRootOffset;
    MaterializationLocator root;
    QIODevice* output = nullptr;
    quint64 documentGeneration = 1;
    QVector<SequenceWindow> sequenceWindows;
    std::shared_ptr<const ResolvedShapeSnapshot> resolvedShape;
    quint64 maxMaterializedNodes = std::numeric_limits<quint64>::max();
    WorkBudget workBudget;
    ShapeScanOptions shapeOptions;
    CancellationToken cancellation;
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
    quint64 logicalNodes = 0;
    DecodeDocumentHandle document;
    std::shared_ptr<const ResolvedShapeSnapshot> shape;
    QVector<SequenceWindow> appliedSequenceWindows;
    QVector<ReferenceEvent> referenceEvents;
    DecodeMetrics metrics;

    bool success() const { return status == DecodeStatus::Success; }
};

DecodeResult decodeBrecoProgram(const DecodeRequest& request);

}  // namespace breco::lang
