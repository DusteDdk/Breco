#pragma once

#include <QByteArray>

#include "model/ResultTypes.h"

namespace breco {

class MatchUtils {
public:
    static int indexOf(const QByteArray& haystack, const QByteArray& needle, int from,
                       TextInterpretationMode mode, bool ignoreCase);
};

}  // namespace breco
