#pragma once

#include <QByteArray>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <QString>

#include "struct/StructTypes.h"

namespace breco {

enum class VisualizedValueKind {
    Scalar,
    Object,
    Array,
};

enum class VisualizedScalarKind {
    None,
    SignedInteger,
    UnsignedInteger,
    String,
    Bytes,
};

struct VisualizedNode {
    QString name;
    QString typeName;
    QString decoration;
    QString endianness;
    QString valueText;
    TextRange declarationRange;
    QByteArray rawBytes;
    int bytesMissing = 0;
    bool valid = true;
    bool hasCondition = false;
    quint64 sourceOffset = 0;
    quint64 sourceLength = 0;
    bool hasSourceOffset = false;
    QString sourceFilePath;
    VisualizedScalarKind scalarKind = VisualizedScalarKind::None;
    qint64 signedValue = 0;
    quint64 unsignedValue = 0;
    QString stringValue;
    StringEncoding stringEncoding = StringEncoding::Ascii;
    Endianness declaredEndianness = Endianness::Native;
    Endianness effectiveEndianness = Endianness::Native;
    int byteOrderUnitWidth = 0;
    QString errorMessage;
    QVector<VisualizedNode> children;
    VisualizedValueKind valueKind = VisualizedValueKind::Scalar;

    QVariantMap toVariantMap() const {
        QVariantMap map;
        map.insert(QStringLiteral("name"), name);
        map.insert(QStringLiteral("typeName"), typeName);
        map.insert(QStringLiteral("decoration"), decoration);
        map.insert(QStringLiteral("endianness"), endianness);
        map.insert(QStringLiteral("valueText"), valueText);
        map.insert(QStringLiteral("rawBytes"), rawBytes);
        map.insert(QStringLiteral("bytesMissing"), bytesMissing);
        map.insert(QStringLiteral("valid"), valid);
        map.insert(QStringLiteral("errorMessage"), errorMessage);
        QVariantList childList;
        for (const VisualizedNode& child : children) {
            childList.push_back(child.toVariantMap());
        }
        map.insert(QStringLiteral("children"), childList);
        return map;
    }
};

}  // namespace breco
