#include "struct/StructureScanner.h"

#include "struct/StructVisualizer.h"

namespace breco {

QVector<StructureScanMatch> scanForStructure(const StructureGraph& graph,
                                             const QString& entryName,
                                             const QByteArray& data,
                                             quint64 baseOffset,
                                             Endianness defaultEndianness) {
    QVector<StructureScanMatch> matches;
    for (quint64 offset = 0; offset < static_cast<quint64>(data.size()); ++offset) {
        const VisualizedNode root = visualize(graph, entryName, data,
                                              static_cast<size_t>(offset), 1,
                                              defaultEndianness);
        if (root.children.isEmpty()) {
            continue;
        }
        const VisualizedNode& candidate = root.children.first();
        if (candidate.valid && candidate.bytesMissing == 0 &&
            candidate.errorMessage.isEmpty() && candidate.sourceLength > 0) {
            matches.push_back({baseOffset + offset, candidate.sourceLength});
        }
    }
    return matches;
}

}  // namespace breco
