#pragma once

#include <QByteArray>
#include <QString>

#include "struct/StructureGraph.h"
#include "struct/VisualizedNode.h"

namespace breco {

VisualizedNode visualize(const StructureGraph& graph, const QString& entryName,
                         const QByteArray& dataBuffer, size_t dataStartOffset, int entryCount,
                         Endianness defaultEndianness = Endianness::Little);

}  // namespace breco
