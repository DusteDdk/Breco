#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <limits>
#include <memory>
#include <optional>

#include "brecolang/ir/BrecoProgram.h"

namespace breco::lang {

class DecodedTree;

struct RequestControl {
    explicit RequestControl(quint64 requestGeneration = 0)
        : generation(requestGeneration) {}

    void cancel() noexcept { cancelled.store(true, std::memory_order_release); }
    bool isCancelled() const noexcept {
        return cancelled.load(std::memory_order_acquire);
    }

    std::atomic_bool cancelled{false};
    quint64 generation = 0;
};

using CancellationToken = std::shared_ptr<RequestControl>;

struct WorkBudget {
    quint64 maxWorkUnits = std::numeric_limits<quint64>::max();
};

struct DecodeDocumentHandle {
    quint64 value = 0;

    bool isValid() const { return value != 0; }
    friend bool operator==(const DecodeDocumentHandle&,
                           const DecodeDocumentHandle&) = default;
};

inline size_t qHash(const DecodeDocumentHandle& handle, size_t seed = 0) {
    return ::qHash(handle.value, seed);
}

struct InstanceLocator {
    quint64 documentGeneration = 0;
    QVector<StatementId> templatePath;
    QVector<quint64> sequenceIndexes;

    bool isValid() const { return documentGeneration != 0; }
    friend bool operator==(const InstanceLocator&,
                           const InstanceLocator&) = default;
};

inline size_t qHash(const InstanceLocator& locator, size_t seed = 0) {
    seed = ::qHash(locator.documentGeneration, seed);
    for (StatementId statement : locator.templatePath) {
        seed = ::qHash(statement, seed);
    }
    for (quint64 index : locator.sequenceIndexes) {
        seed = ::qHash(index, seed);
    }
    return seed;
}

enum class StableReferenceValueKind {
    Null,
    Boolean,
    UnsignedInteger,
    SignedInteger,
    String,
};

struct StableReferenceValue {
    StableReferenceValueKind kind = StableReferenceValueKind::Null;
    quint64 unsignedValue = 0;
    qint64 signedValue = 0;
    bool booleanValue = false;
    QString stringValue;

    friend bool operator==(const StableReferenceValue&,
                           const StableReferenceValue&) = default;
};

inline size_t qHash(const StableReferenceValue& value, size_t seed = 0) {
    seed = ::qHash(static_cast<quint8>(value.kind), seed);
    seed = ::qHash(value.unsignedValue, seed);
    seed = ::qHash(value.signedValue, seed);
    seed = ::qHash(value.booleanValue, seed);
    return ::qHash(value.stringValue, seed);
}

struct StableReferenceKey {
    QVector<StableReferenceValue> values;

    bool isEmpty() const { return values.isEmpty(); }
    friend bool operator==(const StableReferenceKey&,
                           const StableReferenceKey&) = default;
};

inline size_t qHash(const StableReferenceKey& key, size_t seed = 0) {
    for (const StableReferenceValue& value : key.values) {
        seed = qHash(value, seed);
    }
    return seed;
}

enum class ReferenceIdentityKind {
    Physical,
    Explicit,
};

// Equality is deliberately logical rather than observational.  Explicit
// identity is (generation, target type, stable target arguments, explicit
// key); the physical region fields do not participate.  Physical identity
// replaces the explicit key with the full
// normalized (input, offset, length) tuple.
struct ReferenceTargetIdentity {
    quint64 documentGeneration = 0;
    ReferenceIdentityKind kind = ReferenceIdentityKind::Physical;
    TypeId targetType = kInvalidId;
    StableReferenceKey targetArguments;
    StableReferenceKey explicitKey;
    InputId physicalInput = kInvalidId;
    quint64 physicalOffset = 0;
    quint64 physicalLength = 0;

    bool isValid() const { return documentGeneration != 0; }
    friend bool operator==(const ReferenceTargetIdentity& left,
                           const ReferenceTargetIdentity& right) {
        if (left.documentGeneration != right.documentGeneration ||
            left.kind != right.kind || left.targetType != right.targetType ||
            left.targetArguments != right.targetArguments) {
            return false;
        }
        if (left.kind == ReferenceIdentityKind::Explicit) {
            return left.explicitKey == right.explicitKey;
        }
        return left.physicalInput == right.physicalInput &&
               left.physicalOffset == right.physicalOffset &&
               left.physicalLength == right.physicalLength;
    }
};

inline size_t qHash(const ReferenceTargetIdentity& identity, size_t seed = 0) {
    seed = ::qHash(identity.documentGeneration, seed);
    seed = ::qHash(static_cast<quint8>(identity.kind), seed);
    seed = ::qHash(identity.targetType, seed);
    seed = qHash(identity.targetArguments, seed);
    if (identity.kind == ReferenceIdentityKind::Explicit) {
        return qHash(identity.explicitKey, seed);
    }
    seed = ::qHash(identity.physicalInput, seed);
    seed = ::qHash(identity.physicalOffset, seed);
    return ::qHash(identity.physicalLength, seed);
}

struct ReferenceTargetKey {
    // The concrete resolution is kept beside, but outside, canonical
    // identity.  Thus two representations may share an explicit identity
    // without making a page cache reuse bytes from the wrong region.
    ReferenceTargetIdentity identity;
    InputId input = kInvalidId;
    quint64 logicalOffset = 0;
    quint64 regionLength = 0;

    bool isValid() const {
        return identity.isValid() && input != kInvalidId;
    }
    friend bool operator==(const ReferenceTargetKey&,
                           const ReferenceTargetKey&) = default;
};

inline size_t qHash(const ReferenceTargetKey& key, size_t seed = 0) {
    seed = qHash(key.identity, seed);
    seed = ::qHash(key.input, seed);
    seed = ::qHash(key.logicalOffset, seed);
    return ::qHash(key.regionLength, seed);
}

enum class MaterializationRootKind {
    Structural,
    ReferenceTarget,
};

// A tagged locator keeps the shipped structural path shape source-compatible
// while allowing the same relative path to be rooted at a canonical target.
struct MaterializationLocator {
    quint64 documentGeneration = 0;
    QVector<StatementId> templatePath;
    QVector<quint64> sequenceIndexes;
    MaterializationRootKind rootKind = MaterializationRootKind::Structural;
    std::optional<ReferenceTargetKey> referenceTarget;

    MaterializationLocator() = default;
    MaterializationLocator(const InstanceLocator& structural)
        : documentGeneration(structural.documentGeneration),
          templatePath(structural.templatePath),
          sequenceIndexes(structural.sequenceIndexes) {}

    static MaterializationLocator target(ReferenceTargetKey key) {
        MaterializationLocator locator;
        locator.documentGeneration = key.identity.documentGeneration;
        locator.rootKind = MaterializationRootKind::ReferenceTarget;
        locator.referenceTarget = std::move(key);
        return locator;
    }

    bool isValid() const {
        return documentGeneration != 0 &&
               (rootKind == MaterializationRootKind::Structural ||
                (referenceTarget.has_value() && referenceTarget->isValid()));
    }
    bool isReferenceTarget() const {
        return rootKind == MaterializationRootKind::ReferenceTarget;
    }
    operator InstanceLocator() const {
        return isReferenceTarget()
                   ? InstanceLocator{}
                   : InstanceLocator{documentGeneration, templatePath,
                                     sequenceIndexes};
    }
    friend bool operator==(const MaterializationLocator&,
                           const MaterializationLocator&) = default;
};

inline size_t qHash(const MaterializationLocator& locator, size_t seed = 0) {
    seed = ::qHash(locator.documentGeneration, seed);
    seed = ::qHash(static_cast<quint8>(locator.rootKind), seed);
    for (StatementId statement : locator.templatePath) {
        seed = ::qHash(statement, seed);
    }
    for (quint64 index : locator.sequenceIndexes) {
        seed = ::qHash(index, seed);
    }
    return locator.referenceTarget.has_value()
               ? qHash(*locator.referenceTarget, seed)
               : seed;
}

struct ReferenceHandle {
    ReferenceTemplateId referenceTemplate = kInvalidId;
    MaterializationLocator owner;
    ReferenceTargetKey target;
    ReferenceStrength strength = ReferenceStrength::Weak;
    bool isNull = false;

    MaterializationLocator targetLocator() const {
        return MaterializationLocator::target(target);
    }
    friend bool operator==(const ReferenceHandle&,
                           const ReferenceHandle&) = default;
};

struct ReferenceEvent {
    ReferenceHandle handle;
    SourceSpan schemaSpan;
};

enum class ShapeCompleteness {
    Partial,
    Complete,
    Failed,
};

enum class SequenceIndexKind {
    Arithmetic,
    ForwardReplay,
    LegacyEager,
};

class OpaqueContinuationState {
public:
    virtual ~OpaqueContinuationState() = default;

protected:
    OpaqueContinuationState() = default;
};

struct SequenceContinuation {
    quint64 documentGeneration = 0;
    quint64 shapeVersion = 0;
    MaterializationLocator sequence;
    quint64 nextItem = 0;
    std::shared_ptr<const OpaqueContinuationState> state;

    bool isValid() const {
        return documentGeneration != 0 && shapeVersion != 0 &&
               sequence.isValid() && state != nullptr;
    }
    friend bool operator==(const SequenceContinuation&,
                           const SequenceContinuation&) = default;
};

struct ArithmeticSequenceIndex {
    InputId input = kInvalidId;
    quint64 start = 0;
    quint64 count = 0;
    quint64 stride = 0;
};

struct ResolvedSequenceShape {
    MaterializationLocator locator;
    StatementId statement = kInvalidId;
    TypeId itemType = kInvalidId;
    quint32 staticItemTemplate = kInvalidId;
    SequenceIndexKind indexKind = SequenceIndexKind::LegacyEager;
    ArithmeticSequenceIndex arithmetic;
    quint64 itemCount = 0;
    quint64 displayCount = 0;
    quint64 startOffset = 0;
    quint64 endOffset = 0;
    std::optional<SequenceContinuation> startContinuation;
    quint64 totalPrimaryBytes = 0;
    quint64 itemExtentMin = 0;
    quint64 itemExtentMax = 0;
    ShapeCompleteness completeness = ShapeCompleteness::Complete;
};

struct ResolvedShapeSnapshot {
    quint64 documentGeneration = 0;
    quint64 version = 1;
    ShapeCompleteness completeness = ShapeCompleteness::Failed;
    MaterializationLocator root;
    InputId entryInput = kInvalidId;
    quint64 startOffset = 0;
    quint64 endOffset = 0;
    quint64 logicalNodeCount = 0;
    quint64 shapeNodeCount = 0;
    bool usesLegacyEagerTree = false;
    QVector<ResolvedSequenceShape> sequences;

    // Phase 1 reuses the decoded records for the bounded structural outline.
    // Fixed-stride sequence instances are absent from this tree.
    std::shared_ptr<const DecodedTree> outline;
};

struct SequenceWindow {
    MaterializationLocator sequence;
    quint64 firstItem = 0;
    quint64 itemCount = 64;
    std::optional<SequenceContinuation> successor;
    QVector<MaterializationLocator> expansionPath;

    friend bool operator==(const SequenceWindow&,
                           const SequenceWindow&) = default;
};

struct ShapeScanOptions {
    quint64 maxShapeNodes = 100000;
    WorkBudget budget;
};

struct DecodeMetrics {
    quint64 scannedItems = 0;
    quint64 scannedBytes = 0;
    quint64 arithmeticSkippedItems = 0;
    quint64 arithmeticSkippedBytes = 0;
    quint64 resumedItems = 0;
    quint64 coldReplayedItems = 0;
    quint64 coldCursorOpens = 0;
    quint64 shapeNodeCount = 0;
    quint64 materializedNodes = 0;
    quint64 materializedValues = 0;
    quint64 materializedLayouts = 0;
    qint64 elapsedNanoseconds = 0;
};

struct MaterializationMetrics {
    quint64 materializedNodes = 0;
    quint64 materializedValues = 0;
    quint64 materializedLayouts = 0;
    quint64 replayedItems = 0;
    quint64 resumedItems = 0;
    quint64 coldReplayedItems = 0;
    quint64 coldCursorOpens = 0;
    quint64 cacheHits = 0;
    qint64 elapsedNanoseconds = 0;
};

}  // namespace breco::lang

Q_DECLARE_METATYPE(breco::lang::DecodeDocumentHandle)
Q_DECLARE_METATYPE(breco::lang::InstanceLocator)
Q_DECLARE_METATYPE(breco::lang::MaterializationLocator)
Q_DECLARE_METATYPE(breco::lang::ReferenceHandle)
Q_DECLARE_METATYPE(breco::lang::SequenceContinuation)
Q_DECLARE_METATYPE(breco::lang::SequenceWindow)
