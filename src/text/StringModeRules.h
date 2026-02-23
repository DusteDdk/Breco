#pragma once

#include <QByteArray>
#include <QVector>
#include <optional>

namespace breco {

// StringMode NUL rule:
// - Render 0x00 only as special single "0" box.
// - Render is allowed only when previous byte exists, previous byte is not 0x00,
//   and previous byte is printed (ASCII 0x20..0x7E, '\r', or '\n').
// - All other 0x00 bytes are hidden and treated as semantically skipped.
bool isStringModePrintedPredecessor(unsigned char byte);
bool shouldRenderStringModeNull(std::optional<unsigned char> previousByte);
QVector<bool> buildStringModeVisibilityMask(const QByteArray& bytes,
                                            std::optional<unsigned char> previousByteBeforeBase);

}  // namespace breco
