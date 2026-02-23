#include "scan/MatchUtils.h"

namespace breco {

namespace {
unsigned char asciiLower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned char>(c + ('a' - 'A'));
    }
    return c;
}
}  // namespace

int MatchUtils::indexOf(const QByteArray& haystack, const QByteArray& needle, int from,
                        TextInterpretationMode mode, bool ignoreCase) {
    if (!ignoreCase || mode == TextInterpretationMode::Utf16) {
        return haystack.indexOf(needle, from);
    }
    if (needle.isEmpty()) {
        return -1;
    }
    const int start = qMax(0, from);
    const int last = haystack.size() - needle.size();
    for (int i = start; i <= last; ++i) {
        bool matched = true;
        for (int j = 0; j < needle.size(); ++j) {
            const unsigned char h = asciiLower(static_cast<unsigned char>(haystack.at(i + j)));
            const unsigned char n = asciiLower(static_cast<unsigned char>(needle.at(j)));
            if (h != n) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return i;
        }
    }
    return -1;
}

}  // namespace breco
