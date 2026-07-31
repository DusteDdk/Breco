#include "struct/StructExport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <algorithm>

namespace breco {

namespace {

QString childKey(const VisualizedNode& node) {
    if (!node.name.isEmpty()) {
        return node.name;
    }
    if (!node.typeName.isEmpty()) {
        return node.typeName;
    }
    return QStringLiteral("node");
}

void appendIndent(QByteArray* output, int depth) {
    if (output != nullptr && depth > 0) {
        output->append(QByteArray(depth * 4, ' '));
    }
}

QByteArray serializedScalar(const QJsonValue& value) {
    QJsonArray wrapper;
    wrapper.push_back(value);
    const QByteArray serialized =
        QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return serialized.mid(1, serialized.size() - 2);
}

void appendMemberPrefix(QByteArray* output, const QString& name, int depth,
                        bool* first) {
    if (!*first) {
        output->append(",\n");
    }
    *first = false;
    appendIndent(output, depth);
    output->append(serializedScalar(QJsonValue(name)));
    output->append(": ");
}

void appendJsonValue(QByteArray* output, const QJsonValue& value, int depth);

void appendJsonObject(QByteArray* output, const QJsonObject& object, int depth) {
    output->append('{');
    bool first = true;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (first) {
            output->append('\n');
        }
        appendMemberPrefix(output, it.key(), depth + 1, &first);
        appendJsonValue(output, it.value(), depth + 1);
    }
    if (!first) {
        output->append('\n');
        appendIndent(output, depth);
    }
    output->append('}');
}

void appendJsonArray(QByteArray* output, const QJsonArray& array, int depth) {
    output->append('[');
    for (qsizetype i = 0; i < array.size(); ++i) {
        output->append(i == 0 ? "\n" : ",\n");
        appendIndent(output, depth + 1);
        appendJsonValue(output, array.at(i), depth + 1);
    }
    if (!array.isEmpty()) {
        output->append('\n');
        appendIndent(output, depth);
    }
    output->append(']');
}

void appendJsonValue(QByteArray* output, const QJsonValue& value, int depth) {
    if (value.isObject()) {
        appendJsonObject(output, value.toObject(), depth);
    } else if (value.isArray()) {
        appendJsonArray(output, value.toArray(), depth);
    } else {
        output->append(serializedScalar(value));
    }
}

void appendNode(QByteArray* output, const VisualizedNode& node, int depth) {
    output->append('{');
    bool first = true;
    const auto appendStringMember = [&](const QString& name,
                                        const QString& value) {
        appendMemberPrefix(output, name, depth + 1, &first);
        output->append(serializedScalar(QJsonValue(value)));
    };
    const auto appendIntegerMember = [&](const QString& name, int value) {
        appendMemberPrefix(output, name, depth + 1, &first);
        output->append(QByteArray::number(value));
    };
    const auto appendBooleanMember = [&](const QString& name, bool value) {
        appendMemberPrefix(output, name, depth + 1, &first);
        output->append(value ? "true" : "false");
    };

    if (node.valueKind == VisualizedValueKind::Object) {
        output->append('\n');
        appendMemberPrefix(output, QStringLiteral("value"), depth + 1, &first);
        output->append('{');
        bool firstChild = true;
        for (const VisualizedNode& child : node.children) {
            if (firstChild) {
                output->append('\n');
            }
            appendMemberPrefix(output, childKey(child), depth + 2,
                               &firstChild);
            appendNode(output, child, depth + 2);
        }
        if (!firstChild) {
            output->append('\n');
            appendIndent(output, depth + 1);
        }
        output->append('}');
    } else if (node.valueKind == VisualizedValueKind::Array) {
        output->append('\n');
        appendMemberPrefix(output, QStringLiteral("value"), depth + 1, &first);
        output->append('[');
        for (qsizetype i = 0; i < node.children.size(); ++i) {
            output->append(i == 0 ? "\n" : ",\n");
            appendIndent(output, depth + 2);
            appendNode(output, node.children.at(i), depth + 2);
        }
        if (!node.children.isEmpty()) {
            output->append('\n');
            appendIndent(output, depth + 1);
        }
        output->append(']');
    } else if (!node.valueText.isEmpty()) {
        output->append('\n');
        appendStringMember(QStringLiteral("value"), node.valueText);
    }
    if (node.hasCondition) {
        if (first) {
            output->append('\n');
        }
        appendBooleanMember(QStringLiteral("valid"), node.valid);
    }
    if (!node.rawBytes.isEmpty()) {
        if (first) {
            output->append('\n');
        }
        appendStringMember(
            QStringLiteral("rawBytesHex"),
            QString::fromLatin1(node.rawBytes.toHex(' ').toUpper()));
    }
    if (!node.typeName.isEmpty()) {
        if (first) {
            output->append('\n');
        }
        appendStringMember(QStringLiteral("type"), node.typeName);
    }
    if (!node.decoration.isEmpty()) {
        if (first) {
            output->append('\n');
        }
        appendStringMember(QStringLiteral("decoration"), node.decoration);
    }
    if (node.endianness == QStringLiteral("little") ||
        node.endianness == QStringLiteral("big")) {
        if (first) {
            output->append('\n');
        }
        appendStringMember(QStringLiteral("endianness"), node.endianness);
    }
    if (node.bytesMissing != 0) {
        if (first) {
            output->append('\n');
        }
        appendIntegerMember(QStringLiteral("bytesMissing"), node.bytesMissing);
    }
    if (!node.errorMessage.isEmpty()) {
        if (first) {
            output->append('\n');
        }
        appendStringMember(QStringLiteral("error"), node.errorMessage);
    }
    if (!first) {
        output->append('\n');
        appendIndent(output, depth);
    }
    output->append('}');
}

QByteArray transformedLeafBytes(const VisualizedNode& node,
                                StructBinaryExportMode mode,
                                Endianness sourceEndianness) {
    QByteArray bytes = node.rawBytes;
    if (mode != StructBinaryExportMode::DeclaredEndianness ||
        node.byteOrderUnitWidth <= 1 ||
        node.declaredEndianness == Endianness::Native ||
        node.declaredEndianness == sourceEndianness) {
        return bytes;
    }

    const int unit = node.byteOrderUnitWidth;
    for (int start = 0; start + unit <= bytes.size(); start += unit) {
        std::reverse(bytes.begin() + start, bytes.begin() + start + unit);
    }
    return bytes;
}

QString hexBytes(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

QString decimalBytes(const QByteArray& bytes) {
    QStringList parts;
    parts.reserve(bytes.size());
    for (unsigned char byte : bytes) {
        parts.push_back(QString::number(byte));
    }
    return parts.join(QLatin1Char(' '));
}

QString decodeUtf16Bytes(const QByteArray& bytes, Endianness endianness) {
    QString out;
    for (int i = 0; i + 1 < bytes.size(); i += 2) {
        const quint16 first = static_cast<unsigned char>(bytes.at(i));
        const quint16 second = static_cast<unsigned char>(bytes.at(i + 1));
        const quint16 codeUnit =
            endianness == Endianness::Big
                ? static_cast<quint16>((first << 8U) | second)
                : static_cast<quint16>(first | (second << 8U));
        out += QChar(codeUnit);
    }
    return out;
}

}  // namespace

QByteArray serializeVisualizedNode(const VisualizedNode& node) {
    QByteArray output;
    appendNode(&output, node, 0);
    return output;
}

QByteArray serializeVisualizedNodes(const QVector<const VisualizedNode*>& nodes) {
    QByteArray output;
    output.append('[');
    for (qsizetype i = 0; i < nodes.size(); ++i) {
        output.append(i == 0 ? "\n" : ",\n");
        appendIndent(&output, 1);
        if (nodes.at(i) != nullptr) {
            appendNode(&output, *nodes.at(i), 1);
        } else {
            output.append("null");
        }
    }
    if (!nodes.isEmpty()) {
        output.append('\n');
    }
    output.append(']');
    return output;
}

QByteArray serializeDump(const QJsonObject& metadata, const QString& entryName,
                         const VisualizedNode& visualization) {
    QByteArray output;
    output.append("{\n");
    appendIndent(&output, 1);
    output.append(serializedScalar(QJsonValue(QStringLiteral("metadata"))));
    output.append(": ");
    appendJsonObject(&output, metadata, 1);
    output.append(",\n");
    appendIndent(&output, 1);
    output.append(serializedScalar(QJsonValue(entryName)));
    output.append(": ");
    appendNode(&output, visualization, 1);
    output.append("\n}");
    return output;
}

QByteArray exportVisualizedBytes(const VisualizedNode& node,
                                 StructBinaryExportMode mode,
                                 Endianness sourceEndianness) {
    if (node.children.isEmpty() || node.valueKind == VisualizedValueKind::Scalar) {
        return transformedLeafBytes(node, mode, sourceEndianness);
    }

    QByteArray bytes;
    for (const VisualizedNode& child : node.children) {
        bytes += exportVisualizedBytes(child, mode, sourceEndianness);
    }
    return bytes;
}

QByteArray exportVisualizedBytes(const QVector<const VisualizedNode*>& nodes,
                                 StructBinaryExportMode mode,
                                 Endianness sourceEndianness) {
    QByteArray bytes;
    for (const VisualizedNode* node : nodes) {
        if (node != nullptr) {
            bytes += exportVisualizedBytes(*node, mode, sourceEndianness);
        }
    }
    return bytes;
}

bool isScalarCopyNode(const VisualizedNode& node) {
    return node.valueKind == VisualizedValueKind::Scalar &&
           node.scalarKind != VisualizedScalarKind::None;
}

QString formatScalarValue(const VisualizedNode& node, StructScalarFormat format) {
    switch (format) {
        case StructScalarFormat::Default:
            switch (node.scalarKind) {
                case VisualizedScalarKind::SignedInteger:
                    return QString::number(node.signedValue);
                case VisualizedScalarKind::UnsignedInteger:
                    return QString::number(node.unsignedValue);
                case VisualizedScalarKind::String:
                    return node.stringValue;
                case VisualizedScalarKind::Bytes:
                    return hexBytes(node.rawBytes);
                case VisualizedScalarKind::None:
                    return QString();
            }
            break;
        case StructScalarFormat::Hex:
            return hexBytes(node.rawBytes);
        case StructScalarFormat::Decimal:
            return decimalBytes(node.rawBytes);
        case StructScalarFormat::Ascii:
            if (node.scalarKind != VisualizedScalarKind::String) {
                return node.valueText;
            }
            return QString::fromLatin1(node.rawBytes);
        case StructScalarFormat::Utf8:
            if (node.scalarKind != VisualizedScalarKind::String) {
                return node.valueText;
            }
            return QString::fromUtf8(node.rawBytes);
        case StructScalarFormat::Utf16:
            if (node.scalarKind != VisualizedScalarKind::String) {
                return node.valueText;
            }
            return decodeUtf16Bytes(node.rawBytes, node.effectiveEndianness);
    }
    return QString();
}

QString formatPrefixedScalarValue(const VisualizedNode& node,
                                  StructScalarFormat format) {
    return QStringLiteral("%1:%2:%3:%4 > %5")
        .arg(node.sourceFilePath,
             QString::number(node.sourceOffset),
             node.name,
             node.typeName,
             formatScalarValue(node, format));
}

}  // namespace breco
