#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include <limits>
#include <memory>
#include <optional>

#include "brecolang/compiler/Source.h"
#include "brecolang/compiler/Syntax.h"

namespace breco::lang {

using SymbolId = quint32;
using TypeId = quint32;
using ExpressionId = quint32;
using StatementId = quint32;
using SourceSpanId = quint32;
using InputId = quint32;
constexpr quint32 kInvalidId = std::numeric_limits<quint32>::max();

struct IdRange {
    quint32 first = 0;
    quint32 count = 0;
};

enum class TypeKind {
    Invalid,
    Void,
    Boolean,
    UnsignedInteger,
    SignedInteger,
    FloatingPoint,
    String,
    Bytes,
    Enum,
    Record,
    Shape,
    Bitfield,
    Sequence,
    Optional,
    Variant,
    Node,
    Span,
    Reference,
};

enum class Endianness {
    None,
    Little,
    Big,
};

struct TypeDesc {
    TypeKind kind = TypeKind::Invalid;
    SymbolId name = kInvalidId;
    quint16 bitWidth = 0;
    Endianness endianness = Endianness::None;
    TypeId elementType = kInvalidId;
    TypeId referenceKeyType = kInvalidId;
    IdRange fields;
    IdRange alternatives;
};

struct FieldDesc {
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    StatementId statement = kInvalidId;
    bool optional = false;
};

enum class ExpressionKind {
    Invalid,
    UnsignedInteger,
    FloatingPoint,
    String,
    Boolean,
    TypeName,
    Slot,
    Constant,
    EnumValue,
    Member,
    MetadataMember,
    Unary,
    Binary,
    Call,
    ByteArray,
    InterpolatedString,
};

struct Expression {
    ExpressionKind kind = ExpressionKind::Invalid;
    SourceSpanId sourceSpan = kInvalidId;
    TypeId type = kInvalidId;
    SymbolId symbol = kInvalidId;
    quint32 slot = kInvalidId;
    quint64 unsignedValue = 0;
    double floatingValue = 0.0;
    bool booleanValue = false;
    SyntaxUnaryOperator unaryOperator = SyntaxUnaryOperator::Negate;
    SyntaxBinaryOperator binaryOperator = SyntaxBinaryOperator::Add;
    IdRange operands;
    IdRange textParts;
};

enum class ParentAdvance {
    None,
    Contiguous,
    ExternalInput,
    MultiInput,
};

struct ExtentSummary {
    quint64 minBytes = 0;
    std::optional<quint64> maxBytes;
    std::optional<quint64> exactBytes;
    quint64 fixedPrefixBytes = 0;
    ParentAdvance parentAdvance = ParentAdvance::None;
    bool mayFail = false;
    bool mayNoMatch = false;
    bool requiresRandomAccess = false;
    bool hasReferenceEffects = false;
};

enum class StatementKind {
    Invalid,
    Field,
    Reference,
    ComputedField,
    BitfieldField,
    Identify,
    Commit,
    Require,
    Check,
    Match,
    Preserve,
    Raw,
    Region,
    Repeat,
    While,
    Many,
    Select,
    OneOf,
    Recover,
    Continue,
    Break,
    Emit,
    Let,
    If,
    For,
};

enum class LoopScanPlan {
    ExecuteItems,
    BatchAdvance,
};

enum class ReferenceEffectScanPlan {
    None,
    ExecuteItems,
};

using ReferenceTemplateId = quint32;

enum class AddressBaseKind {
    Input,
    EntryRoot,
    ContainingEntity,
};

enum class ReferenceStrength {
    Follow,
    Weak,
};

enum class ReferenceCoverage {
    DecodedStorage,
    WholeRegion,
};

struct ReferenceAddressDesc {
    InputId input = kInvalidId;
    AddressBaseKind base = AddressBaseKind::Input;
    ExpressionId displacement = kInvalidId;
    ExpressionId regionLength = kInvalidId;
};

struct ReferenceRewriteDesc {
    IdRange patchPath;
    StatementId patchStatement = kInvalidId;
    ExpressionId exportedValue = kInvalidId;
};

struct ReferenceDesc {
    TypeId targetType = kInvalidId;
    IdRange targetArguments;
    ReferenceAddressDesc address;
    IdRange keyExpressions;
    ReferenceStrength strength = ReferenceStrength::Weak;
    ReferenceCoverage coverage = ReferenceCoverage::DecodedStorage;
    IdRange rewriteRules;
};

struct SelectCase {
    SourceSpanId sourceSpan = kInvalidId;
    bool isDefault = false;
    bool isConditional = false;
    ExpressionId expression = kInvalidId;
    TypeId resultType = kInvalidId;
    IdRange arguments;
    IdRange statements;
};

struct Alternative {
    SourceSpanId sourceSpan = kInvalidId;
    TypeId type = kInvalidId;
    IdRange arguments;
};

struct BitMember {
    SymbolId name = kInvalidId;
    SourceSpanId sourceSpan = kInvalidId;
    quint8 highBit = 0;
    quint8 lowBit = 0;
};

struct Statement {
    StatementKind kind = StatementKind::Invalid;
    SourceSpanId sourceSpan = kInvalidId;
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    ExpressionId expression = kInvalidId;
    ExpressionId condition = kInvalidId;
    ExpressionId secondaryExpression = kInvalidId;
    InputId input = kInvalidId;
    SymbolId message = kInvalidId;
    SymbolId gapsName = kInvalidId;
    quint64 stepBytes = 1;
    quint32 resultSlot = kInvalidId;
    quint32 iterationSlot = kInvalidId;
    quint32 secondarySlot = kInvalidId;
    IdRange arguments;
    IdRange statements;
    IdRange initializers;
    IdRange elseStatements;
    IdRange selectCases;
    IdRange alternatives;
    IdRange bitMembers;
    IdRange bytePatterns;
    quint32 extent = kInvalidId;
    quint32 itemExtent = kInvalidId;
    quint32 staticItemTemplate = kInvalidId;
    ReferenceTemplateId reference = kInvalidId;
    LoopScanPlan loopScanPlan = LoopScanPlan::ExecuteItems;
    ReferenceEffectScanPlan referenceEffectScanPlan =
        ReferenceEffectScanPlan::None;
};

struct InputDesc {
    SymbolId name = kInvalidId;
    SymbolId label = kInvalidId;
    SymbolId description = kInvalidId;
    bool isDefault = false;
};

struct LimitSet {
    quint32 maxParseDepth = 128;
    quint64 maxLoopIterations = 1'000'000;
    quint64 maxNodes = 5'000'000;
    quint64 maxProbeBytes = 1024 * 1024;
    quint64 maxTransformOutput = 256ULL * 1024ULL * 1024ULL;
};

struct ConstantDesc {
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    ExpressionId value = kInvalidId;
};

struct EnumValueDesc {
    SymbolId name = kInvalidId;
    quint64 value = 0;
    SourceSpanId sourceSpan = kInvalidId;
};

struct EnumDesc {
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    TypeId underlyingType = kInvalidId;
    IdRange values;
};

struct ParameterDesc {
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    quint32 slot = kInvalidId;
};

struct RecordDesc {
    SymbolId name = kInvalidId;
    TypeId type = kInvalidId;
    IdRange parameters;
    IdRange statements;
    quint32 slotCount = 0;
    quint32 extent = kInvalidId;
};

struct EntryDesc {
    SymbolId name = kInvalidId;
    TypeId resultType = kInvalidId;
    InputId input = kInvalidId;
    IdRange statements;
    quint32 slotCount = 0;
    quint32 extent = kInvalidId;
};

struct OutformDesc {
    SymbolId name = kInvalidId;
    SymbolId parameterName = kInvalidId;
    TypeId targetType = kInvalidId;
    OutformMode mode = OutformMode::Text;
    IdRange statements;
    quint32 slotCount = 0;
    SourceSpanId sourceSpan = kInvalidId;
    SymbolId sourcePath = kInvalidId;
};

struct BytePattern {
    SourceSpanId sourceSpan = kInvalidId;
    QByteArray bytes;
};

class BrecoProgram {
public:
    QString languageVersion;
    LimitSet limits;
    SymbolId defaultEntry = kInvalidId;

    QVector<QString> symbols;
    QVector<SourceSpan> sourceSpans;
    QVector<InputDesc> inputs;
    QVector<ConstantDesc> constants;
    QVector<EnumDesc> enums;
    QVector<EnumValueDesc> enumValues;
    QVector<RecordDesc> records;
    QVector<EntryDesc> entries;
    QVector<OutformDesc> outforms;
    QVector<ParameterDesc> parameters;
    QVector<TypeDesc> types;
    QVector<FieldDesc> fields;
    QVector<Statement> statements;
    QVector<Expression> expressions;
    QVector<SelectCase> selectCases;
    QVector<Alternative> alternatives;
    QVector<BitMember> bitMembers;
    QVector<BytePattern> bytePatterns;
    QVector<ReferenceDesc> references;
    QVector<ReferenceRewriteDesc> referenceRewrites;
    QVector<ExtentSummary> extents;

    QVector<quint32> statementRefs;
    QVector<quint32> expressionRefs;
    QVector<quint32> typeRefs;
    QVector<quint32> fieldRefs;
    QVector<quint32> bitMemberRefs;
    QVector<quint32> bytePatternRefs;
    QVector<quint32> referenceRewriteRefs;
    QVector<SymbolId> symbolRefs;
    QVector<SymbolId> textPartSymbols;

    const QString& symbol(SymbolId id) const;
};

struct CompileResult {
    std::shared_ptr<const BrecoProgram> program;
    std::shared_ptr<const SyntaxFile> syntax;
    QVector<Diagnostic> diagnostics;

    bool success() const { return program != nullptr; }
};

}  // namespace breco::lang
