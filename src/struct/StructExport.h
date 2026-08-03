#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include "struct/StructTypes.h"
#include "struct/VisualizedNode.h"

namespace breco {

enum class StructBinaryExportMode {
    SourceEndianness,
    DeclaredEndianness,
};

enum class StructScalarFormat {
    Default,
    Hex,
    Decimal,
    Ascii,
    Utf8,
    Utf16,
};

QByteArray serializeVisualizedNode(const VisualizedNode& node);
QByteArray serializeVisualizedNodes(const QVector<const VisualizedNode*>& nodes);
QByteArray serializeDump(const QJsonObject& metadata, const QString& entryName,
                         const VisualizedNode& visualization);

QByteArray exportVisualizedBytes(const VisualizedNode& node,
                                 StructBinaryExportMode mode,
                                 Endianness sourceEndianness);
QByteArray exportVisualizedBytes(const QVector<const VisualizedNode*>& nodes,
                                 StructBinaryExportMode mode,
                                 Endianness sourceEndianness);

bool isScalarCopyNode(const VisualizedNode& node);
QString formatScalarValue(const VisualizedNode& node, StructScalarFormat format);
QString formatPrefixedScalarValue(const VisualizedNode& node,
                                  StructScalarFormat format);

// Renders {{name}}, {{type}}, {{value}}, {{offset}}, {{length}}, {{bytes}},
// {{path}} and {{#children}}...{{/children}} placeholders.
QString renderStructureTemplate(const QString& templateText,
                                const VisualizedNode& node,
                                QString* errorMessage = nullptr);

}  // namespace breco
