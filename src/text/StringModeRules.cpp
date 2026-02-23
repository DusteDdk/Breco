#include "text/StringModeRules.h"

namespace breco {

bool isStringModePrintedPredecessor(unsigned char byte) {
    return (byte >= 0x20U && byte <= 0x7EU) || byte == '\r' || byte == '\n';
}

bool shouldRenderStringModeNull(std::optional<unsigned char> previousByte) {
    if (!previousByte.has_value()) {
        return false;
    }
    const unsigned char prev = previousByte.value();
    if (prev == 0x00U) {
        return false;
    }
    return isStringModePrintedPredecessor(prev);
}

QVector<bool> buildStringModeVisibilityMask(const QByteArray& bytes,
                                            std::optional<unsigned char> previousByteBeforeBase) {
    QVector<bool> visible;
    visible.reserve(bytes.size());

    for (int i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes.at(i));
        if (byte != 0x00U) {
            visible.push_back(true);
            continue;
        }

        std::optional<unsigned char> prev;
        if (i > 0) {
            prev = static_cast<unsigned char>(bytes.at(i - 1));
        } else {
            prev = previousByteBeforeBase;
        }
        visible.push_back(shouldRenderStringModeNull(prev));
    }

    return visible;
}

}  // namespace breco
