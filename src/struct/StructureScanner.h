#pragma once

#include <QByteArray>
#include <QVector>

#include "struct/StructureGraph.h"

namespace breco {

struct StructureScanMatch {
    quint64 offset = 0;
    quint64 byteLength = 0;
};

QVector<StructureScanMatch> scanForStructure(
    const StructureGraph& graph, const QString& entryName, const QByteArray& data,
    quint64 baseOffset = 0,
    Endianness defaultEndianness = Endianness::Little);

}  // namespace breco
