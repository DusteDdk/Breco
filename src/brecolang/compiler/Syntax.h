#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <limits>

#include "brecolang/compiler/Source.h"

namespace breco::lang {

using SyntaxExpressionId = quint32;
using SyntaxStatementId = quint32;
constexpr SyntaxExpressionId kInvalidSyntaxExpression =
    std::numeric_limits<SyntaxExpressionId>::max();

enum class SyntaxExpressionKind {
    Invalid,
    UnsignedInteger,
    FloatingPoint,
    String,
    Boolean,
    Identifier,
    Member,
    MetadataMember,
    Unary,
    Binary,
    Call,
    ByteArray,
    InterpolatedString,
};

enum class SyntaxUnaryOperator {
    Negate,
    LogicalNot,
};

enum class SyntaxBinaryOperator {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
    Range,
};

struct SyntaxExpression {
    SyntaxExpressionKind kind = SyntaxExpressionKind::Invalid;
    SourceSpan span;
    QString text;
    quint64 unsignedValue = 0;
    double floatingValue = 0.0;
    bool booleanValue = false;
    SyntaxUnaryOperator unaryOperator = SyntaxUnaryOperator::Negate;
    SyntaxBinaryOperator binaryOperator = SyntaxBinaryOperator::Add;
    QVector<SyntaxExpressionId> operands;
    QVector<QString> textParts;
};

struct SyntaxType {
    QString name;
    SourceSpan span;
    QVector<SyntaxExpressionId> arguments;
};

struct SyntaxBitMember {
    QString name;
    SourceSpan span;
    SyntaxExpressionId highBit = kInvalidSyntaxExpression;
    SyntaxExpressionId lowBit = kInvalidSyntaxExpression;
};

struct SyntaxSelectCase {
    SourceSpan span;
    bool isDefault = false;
    bool isConditional = false;
    SyntaxExpressionId expression = kInvalidSyntaxExpression;
    bool isType = false;
    SyntaxType type;
    QVector<SyntaxStatementId> statements;
};

struct SyntaxAlternative {
    SourceSpan span;
    SyntaxType type;
};

struct SyntaxBytePattern {
    SourceSpan span;
    QVector<SyntaxExpressionId> bytes;
};

enum class SyntaxStatementKind {
    Invalid,
    Field,
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

struct SyntaxStatement {
    SyntaxStatementKind kind = SyntaxStatementKind::Invalid;
    SourceSpan span;
    QString name;
    SourceSpan nameSpan;
    QString secondaryName;
    SourceSpan secondaryNameSpan;
    SyntaxType type;
    SyntaxExpressionId expression = kInvalidSyntaxExpression;
    SyntaxExpressionId condition = kInvalidSyntaxExpression;
    SyntaxExpressionId secondaryExpression = kInvalidSyntaxExpression;
    QString message;
    QString sourceInput;
    SourceSpan sourceInputSpan;
    QVector<SyntaxStatementId> statements;
    QVector<SyntaxStatementId> initializers;
    QVector<SyntaxStatementId> elseStatements;
    QVector<SyntaxSelectCase> selectCases;
    QVector<SyntaxAlternative> alternatives;
    QVector<SyntaxBitMember> bitMembers;
    QVector<SyntaxBytePattern> syncPatterns;
    QString gapsName;
    quint64 stepBytes = 1;
};

struct SyntaxParameter {
    QString name;
    SourceSpan nameSpan;
    SyntaxType type;
};

struct SyntaxInput {
    QString name;
    SourceSpan nameSpan;
    QString label;
    QString description;
    bool isDefault = false;
};

struct SyntaxLimit {
    QString name;
    SourceSpan nameSpan;
    SyntaxExpressionId value = kInvalidSyntaxExpression;
};

struct SyntaxConstant {
    QString name;
    SourceSpan nameSpan;
    SyntaxType type;
    bool hasExplicitType = false;
    SyntaxExpressionId value = kInvalidSyntaxExpression;
};

struct SyntaxEnumMember {
    QString name;
    SourceSpan nameSpan;
    SyntaxExpressionId value = kInvalidSyntaxExpression;
};

struct SyntaxEnum {
    QString name;
    SourceSpan nameSpan;
    SyntaxType underlyingType;
    QVector<SyntaxEnumMember> members;
};

struct SyntaxRecord {
    QString name;
    SourceSpan nameSpan;
    QVector<SyntaxParameter> parameters;
    QVector<SyntaxStatementId> statements;
};

struct SyntaxEntry {
    QString name;
    SourceSpan nameSpan;
    QString inputName;
    SourceSpan inputSpan;
    QVector<SyntaxStatementId> statements;
};

enum class OutformMode {
    Text,
    Binary,
};

struct SyntaxOutform {
    QString name;
    SourceSpan nameSpan;
    QString parameterName;
    SourceSpan parameterSpan;
    SyntaxType targetType;
    OutformMode mode = OutformMode::Text;
    QVector<SyntaxStatementId> statements;
};

struct SyntaxFile {
    QString sourcePath;
    QString languageName;
    QString languageVersion;
    QVector<SyntaxInput> inputs;
    QVector<SyntaxLimit> limits;
    QVector<SyntaxConstant> constants;
    QVector<SyntaxEnum> enums;
    QVector<SyntaxRecord> records;
    QVector<SyntaxEntry> entries;
    QVector<SyntaxOutform> outforms;
    QString defaultEntry;
    SourceSpan defaultEntrySpan;
    QVector<SyntaxExpression> expressions;
    QVector<SyntaxStatement> statements;
};

struct ParseSyntaxResult {
    SyntaxFile syntax;
    QVector<Diagnostic> diagnostics;
};

}  // namespace breco::lang
