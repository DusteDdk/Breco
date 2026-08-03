#pragma once

#include <QByteArray>
#include <QString>
#include <QHash>

#include "struct/StructureGraph.h"
#include "struct/VisualizedNode.h"

namespace breco {

struct VisualizationSource {
    QByteArray bytes;
    QString filePath;
    quint64 baseOffset = 0;
};

VisualizedNode visualize(const StructureGraph& graph, const QString& entryName,
                         const QByteArray& dataBuffer, size_t dataStartOffset, int entryCount,
                         Endianness defaultEndianness = Endianness::Little);
VisualizedNode visualize(const StructureGraph& graph, const QString& entryName,
                         const VisualizationSource& primarySource,
                         size_t dataStartOffset, int entryCount,
                         const QHash<QString, VisualizationSource>& externalSources,
                         Endianness defaultEndianness = Endianness::Little);

}  // namespace breco
