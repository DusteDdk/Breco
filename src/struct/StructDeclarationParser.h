#pragma once

#include <QString>

#include "struct/StructureGraph.h"

namespace breco {

struct ParseResult {
    bool valid = false;
    QString errorMessage;
    TextRange errorRange;
    StructureGraph graph;
};

ParseResult parseStructDeclaration(const QString& text);

}  // namespace breco
