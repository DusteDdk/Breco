#include "view/BitmapViewWidget.h"

#include <algorithm>

#include <QColor>
#include <QPainter>
#include <QToolTip>
#include <QtMath>

#include "debug/SelectionTrace.h"

namespace breco {

constexpr int kTooltipMaxBytes = 128;

BitmapViewWidget::BitmapViewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(220);
    setMouseTracking(true);
    m_rgbi256Palette = buildRgbi256Palette();
    m_hoverTooltipTimer.setSingleShot(true);
    connect(&m_hoverTooltipTimer, &QTimer::timeout, this, [this]() {
        if (m_pendingTooltipSequenceIndex < 0 || m_pendingTooltipByteIndex < 0) {
            return;
        }
        QToolTip::showText(m_pendingTooltipGlobalPos,
                           tooltipForSequence(m_pendingTooltipSequenceIndex,
                                              m_pendingTooltipByteIndex),
                           this);
    });
}

void BitmapViewWidget::setData(const QByteArray& bytes) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::setData: bytes=%1").arg(bytes.size()));
    }
    m_bytes = bytes;
    m_hoveredSequenceIndex = -1;
    m_lastHoverByteIndex = -1;
    m_hoverTooltipTimer.stop();
    m_pendingTooltipSequenceIndex = -1;
    m_pendingTooltipByteIndex = -1;
    QToolTip::hideText();
    markTextAnalysisDirty();
    resetPanOffset();
    markDirty();
    update();
    BRECO_SELTRACE("BitmapViewWidget::setData: done");
}

void BitmapViewWidget::setMode(BitmapMode mode) {
    m_mode = mode;
    m_hoverTooltipTimer.stop();
    m_pendingTooltipSequenceIndex = -1;
    m_pendingTooltipByteIndex = -1;
    QToolTip::hideText();
    resetPanOffset();
    markDirty();
    update();
}

void BitmapViewWidget::setTextMode(TextInterpretationMode mode) {
    if (m_textMode == mode) {
        return;
    }
    m_textMode = mode;
    markTextAnalysisDirty();
    if (m_mode == BitmapMode::Text) {
        markDirty();
        update();
    }
}

void BitmapViewWidget::setExternalHoverOffset(std::optional<quint64> absoluteOffset) {
    if (m_externalHoverOffset == absoluteOffset) {
        return;
    }
    m_externalHoverOffset = absoluteOffset;
    markDirty();
    update();
}

void BitmapViewWidget::setExternalSelectionRange(
    std::optional<QPair<quint64, quint64>> absoluteRange) {
    if (absoluteRange.has_value()) {
        const QPair<quint64, quint64> range = absoluteRange.value();
        const quint64 start = qMin(range.first, range.second);
        const quint64 end = qMax(range.first, range.second);
        absoluteRange = qMakePair(start, end);
    }
    if (m_externalSelectionRange == absoluteRange) {
        return;
    }
    m_externalSelectionRange = absoluteRange;
    markDirty();
    update();
}

void BitmapViewWidget::setResultOverlayEnabled(bool enabled) {
    m_resultOverlayEnabled = enabled;
    markDirty();
    update();
}

void BitmapViewWidget::setResultHighlight(quint64 absoluteOffset, quint32 validBefore,
                                          quint32 termLength, quint32 validAfter,
                                          quint64 previewBaseOffset) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "BitmapViewWidget::setResultHighlight: offset=%1 termLength=%2 previewBase=%3")
                           .arg(absoluteOffset)
                           .arg(termLength)
                           .arg(previewBaseOffset));
    }
    m_resultOffset = absoluteOffset;
    m_validBefore = validBefore;
    m_termLength = termLength;
    m_validAfter = validAfter;
    m_previewBaseOffset = previewBaseOffset;
    m_beforeStart = (m_resultOffset > m_validBefore) ? (m_resultOffset - m_validBefore) : 0;
    m_termStart = m_resultOffset;
    m_termEnd = m_termStart + m_termLength;
    m_afterEnd = m_termEnd + m_validAfter;
    markDirty();
    update();
    BRECO_SELTRACE("BitmapViewWidget::setResultHighlight: done");
}

void BitmapViewWidget::setZoom(int zoom) {
    const int oldZoom = m_zoom;
    m_zoom = qBound(1, zoom, 32);
    if (m_zoom == 1) {
        resetPanOffset();
    }
    markDirty();
    update();
    if (m_zoom != oldZoom) {
        emit zoomChanged(m_zoom);
    }
}

int BitmapViewWidget::zoom() const { return m_zoom; }

quint64 BitmapViewWidget::viewportByteCapacity() const {
    const int w = qMax(1, width());
    const int h = qMax(1, height());
    const int sourceWidth = qMax(1, w / qMax(1, m_zoom));
    const int sourceHeight = qMax(1, h / qMax(1, m_zoom));
    const quint64 sourcePixels =
        static_cast<quint64>(sourceWidth) * static_cast<quint64>(sourceHeight);

    if (m_mode == BitmapMode::Binary) {
        return qMax<quint64>(1, (sourcePixels + 7ULL) / 8ULL);
    }

    int bytesPerPixel = 3;
    if (m_mode == BitmapMode::Grey8 || m_mode == BitmapMode::Text ||
        m_mode == BitmapMode::Rgbi256) {
        bytesPerPixel = 1;
    }
    return qMax<quint64>(1, sourcePixels * static_cast<quint64>(bytesPerPixel));
}

void BitmapViewWidget::setCenterAnchorOffset(quint64 absoluteOffset) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::setCenterAnchorOffset: offset=%1")
                           .arg(absoluteOffset));
    }
    resetPanOffset();
    m_centerAnchorOffset = absoluteOffset;
    markDirty();
    update();
}

void BitmapViewWidget::setOverlapIntervals(const QVector<QPair<quint64, quint64>>& intervals) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::setOverlapIntervals: intervals=%1")
                           .arg(intervals.size()));
    }
    m_overlapIntervals = intervals;
    const quint64 mergeStartUs = debug::selectionTraceElapsedUs();
    rebuildMergedIntervals();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "BitmapViewWidget::setOverlapIntervals: mergedIntervals=%1 mergeElapsed=%2us")
                           .arg(m_mergedOverlapIntervals.size())
                           .arg(debug::selectionTraceElapsedUs() - mergeStartUs));
    }
    markDirty();
    update();
    BRECO_SELTRACE("BitmapViewWidget::setOverlapIntervals: done");
}

bool BitmapViewWidget::isHighlightedOffset(quint64 absoluteByteOffset) const {
    if (absoluteByteOffset >= m_termStart && absoluteByteOffset < m_termEnd) {
        return true;
    }
    if (absoluteByteOffset >= m_beforeStart && absoluteByteOffset < m_termStart) {
        return true;
    }
    if (absoluteByteOffset >= m_termEnd && absoluteByteOffset < m_afterEnd) {
        return true;
    }
    return false;
}

void BitmapViewWidget::markDirty() { m_dirty = true; }

void BitmapViewWidget::markTextAnalysisDirty() { m_textAnalysisDirty = true; }

void BitmapViewWidget::rebuildTextAnalysisIfNeeded() {
    if (!m_textAnalysisDirty) {
        return;
    }
    const quint64 startUs = debug::selectionTraceElapsedUs();
    m_textAnalysis = TextSequenceAnalyzer::analyze(m_bytes, m_textMode);
    m_textAnalysisDirty = false;
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::rebuildTextAnalysisIfNeeded: elapsed=%1us")
                           .arg(debug::selectionTraceElapsedUs() - startUs));
    }
}

void BitmapViewWidget::resetPanOffset() {
    m_panDxPixels = 0;
    m_panDyPixels = 0;
    if (m_dragPanning) {
        unsetCursor();
    }
    m_dragPanning = false;
}

void BitmapViewWidget::applyOverlayColor(int baseLuma, int overlayR, int overlayG, int overlayB,
                                         int& outR, int& outG, int& outB) const {
    if (m_mode == BitmapMode::Rgb24) {
        outR = overlayR;
        outG = overlayG;
        outB = overlayB;
        return;
    }

    auto scaleChannel = [baseLuma](int channelValue) -> int {
        if (channelValue <= 0) {
            return 0;
        }
        int scaled = (channelValue * baseLuma) / 255;
        if (baseLuma > 0) {
            scaled = qMax(24, scaled);
        }
        return qBound(0, scaled, 255);
    };

    outR = scaleChannel(overlayR);
    outG = scaleChannel(overlayG);
    outB = scaleChannel(overlayB);
}

QColor BitmapViewWidget::colorForTextClass(TextByteClass cls) {
    switch (cls) {
        case TextByteClass::Printable:
            return QColor(0x00, 0x8B, 0x8B);  // DarkCyan
        case TextByteClass::Newline:
            return QColor(0xF5, 0xF5, 0xDC);  // Beige
        case TextByteClass::CarriageReturn:
            return QColor(0xFA, 0xEB, 0xD7);  // AntiqueWhite
        case TextByteClass::NonBreakingSpace:
            return QColor(0x00, 0xFF, 0xFF);  // Aqua
        case TextByteClass::Space:
            return QColor(0x7F, 0xFF, 0xD4);  // Mild Aquamarine
        case TextByteClass::Tab:
            return QColor(0x5F, 0x9E, 0xA0);  // CadetBlue
        case TextByteClass::SoftHyphen:
            return QColor(0xFF, 0x8C, 0x00);  // DarkOrange
        case TextByteClass::OtherWhitespace:
            return QColor(0x00, 0xBF, 0xFF);  // DeepSkyBlue
        case TextByteClass::Invalid:
        default:
            return QColor(0x00, 0x00, 0x00);
    }
}

std::array<QColor, 256> BitmapViewWidget::buildRgbi256Palette() {
    std::array<QColor, 256> palette{};
    int r = 0;
    int g = 0;
    int b = 0;
    palette[0] = QColor(r, g, b);
    for (int i = 1; i < 256; ++i) {
        const int step = (i - 1) % 3;
        if (step == 0) {
            r = qMin(255, r + 3);
        } else if (step == 1) {
            g = qMin(255, g + 3);
        } else {
            b = qMin(255, b + 3);
        }
        palette[i] = QColor(r, g, b);
    }
    return palette;
}

void BitmapViewWidget::rebuildMergedIntervals() {
    const quint64 startUs = debug::selectionTraceElapsedUs();
    m_mergedOverlapIntervals.clear();
    if (m_overlapIntervals.isEmpty()) {
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::rebuildMergedIntervals: input=0 output=0 elapsed=%1us")
                               .arg(debug::selectionTraceElapsedUs() - startUs));
        }
        return;
    }
    QVector<QPair<quint64, quint64>> sorted = m_overlapIntervals;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return a.second < b.second;
    });

    QPair<quint64, quint64> current = sorted.first();
    for (int i = 1; i < sorted.size(); ++i) {
        const auto& next = sorted.at(i);
        if (next.first <= current.second) {
            current.second = qMax(current.second, next.second);
        } else {
            m_mergedOverlapIntervals.push_back(current);
            current = next;
        }
    }
    m_mergedOverlapIntervals.push_back(current);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::rebuildMergedIntervals: input=%1 output=%2 elapsed=%3us")
                           .arg(m_overlapIntervals.size())
                           .arg(m_mergedOverlapIntervals.size())
                           .arg(debug::selectionTraceElapsedUs() - startUs));
    }
}

bool BitmapViewWidget::overlapsAnyOtherMatch(quint64 absoluteByteOffset, int* intervalIdx) const {
    if (m_mergedOverlapIntervals.isEmpty()) {
        return false;
    }
    int idx = intervalIdx != nullptr ? *intervalIdx : 0;
    idx = qBound(0, idx, m_mergedOverlapIntervals.size() - 1);
    while (idx < m_mergedOverlapIntervals.size() &&
           m_mergedOverlapIntervals.at(idx).second <= absoluteByteOffset) {
        ++idx;
    }
    if (intervalIdx != nullptr) {
        *intervalIdx = idx;
    }
    if (idx >= m_mergedOverlapIntervals.size()) {
        return false;
    }
    const auto& interval = m_mergedOverlapIntervals.at(idx);
    return absoluteByteOffset >= interval.first && absoluteByteOffset < interval.second;
}

std::optional<int> BitmapViewWidget::byteIndexAtPoint(const QPoint& point) const {
    if (m_bytes.isEmpty()) {
        return std::nullopt;
    }

    const int w = qMax(1, width());
    const int h = qMax(1, height());
    const int sourceWidth = qMax(1, w / m_zoom);
    const int sourceHeight = qMax(1, h / m_zoom);
    const qint64 centerSourceIndex =
        static_cast<qint64>(sourceHeight / 2) * static_cast<qint64>(sourceWidth) + (sourceWidth / 2);
    const bool binaryMode = m_mode == BitmapMode::Binary;

    int bytesPerPixel = 3;
    if (m_mode == BitmapMode::Grey8 || m_mode == BitmapMode::Text ||
        m_mode == BitmapMode::Rgbi256) {
        bytesPerPixel = 1;
    }

    const qint64 anchorRelative =
        binaryMode
            ? (static_cast<qint64>(m_centerAnchorOffset) - static_cast<qint64>(m_previewBaseOffset)) * 8
            : static_cast<qint64>(m_centerAnchorOffset) - static_cast<qint64>(m_previewBaseOffset);

    const int sx = static_cast<int>(qFloor((static_cast<double>(point.x() - m_panDxPixels)) /
                                           static_cast<double>(m_zoom)));
    const int sy = static_cast<int>(qFloor((static_cast<double>(point.y() - m_panDyPixels)) /
                                           static_cast<double>(m_zoom)));
    const qint64 sourcePixelIndex =
        static_cast<qint64>(sy) * static_cast<qint64>(sourceWidth) + static_cast<qint64>(sx);
    const qint64 deltaPixels = sourcePixelIndex - centerSourceIndex;

    if (binaryMode) {
        const qint64 bitIndexSigned = anchorRelative + deltaPixels;
        if (bitIndexSigned < 0 || bitIndexSigned >= static_cast<qint64>(m_bytes.size()) * 8) {
            return std::nullopt;
        }
        const quint64 byteIndex = static_cast<quint64>(bitIndexSigned) / 8ULL;
        if (byteIndex >= static_cast<quint64>(m_bytes.size())) {
            return std::nullopt;
        }
        return static_cast<int>(byteIndex);
    }

    const qint64 sourceIndex = anchorRelative + (deltaPixels * static_cast<qint64>(bytesPerPixel));
    if (sourceIndex < 0 || sourceIndex >= static_cast<qint64>(m_bytes.size())) {
        return std::nullopt;
    }
    return static_cast<int>(sourceIndex);
}

QString BitmapViewWidget::tooltipForSequence(int sequenceIndex, int hoveredByteIndex) const {
    if (sequenceIndex < 0 || sequenceIndex >= m_textAnalysis.sequences.size()) {
        return {};
    }
    const ValidTextSequence sequence = m_textAnalysis.sequences.at(sequenceIndex);
    const int sequenceStart = qMax(0, sequence.startIndex);
    const int sequenceEnd = qMax(sequenceStart, sequence.endIndex);
    const int sequenceLength = qMax(0, sequenceEnd - sequenceStart);
    if (sequenceLength <= 0) {
        return {};
    }

    const int safeHover = qBound(sequenceStart, hoveredByteIndex, sequenceEnd - 1);
    const int windowLength = qMin(kTooltipMaxBytes, sequenceLength);
    int windowStart = safeHover - (windowLength / 2);
    const int maxWindowStart = sequenceEnd - windowLength;
    windowStart = qBound(sequenceStart, windowStart, maxWindowStart);

    const quint64 absoluteSequenceStart = m_previewBaseOffset + static_cast<quint64>(sequenceStart);
    const QString text = TextSequenceAnalyzer::decodeRange(
        m_bytes, windowStart, windowLength, m_textMode, m_textAnalysis.utf16LittleEndian);
    return QStringLiteral("%1 bytes at offset: %2\n---\n%3")
        .arg(sequenceLength)
        .arg(absoluteSequenceStart)
        .arg(text);
}

void BitmapViewWidget::rebuildImageIfNeeded() {
    const quint64 rebuildStartUs = debug::selectionTraceElapsedUs();
    const int w = qMax(1, width());
    const int h = qMax(1, height());
    if (!m_dirty && m_cachedWidth == w && m_cachedHeight == h && !m_cachedImage.isNull()) {
        return;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "BitmapViewWidget::rebuildImageIfNeeded: rebuilding w=%1 h=%2 bytes=%3 mode=%4 zoom=%5")
                           .arg(w)
                           .arg(h)
                           .arg(m_bytes.size())
                           .arg(static_cast<int>(m_mode))
                           .arg(m_zoom));
    }

    m_cachedWidth = w;
    m_cachedHeight = h;
    m_cachedImage = QImage(w, h, QImage::Format_RGB32);
    m_cachedImage.fill(Qt::black);

    if (m_bytes.isEmpty()) {
        m_dirty = false;
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral(
                               "BitmapViewWidget::rebuildImageIfNeeded: empty bytes elapsed=%1us")
                               .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
        }
        return;
    }

    if (m_mode == BitmapMode::Text) {
        rebuildTextAnalysisIfNeeded();
    }

    int bytesPerPixel = 3;
    if (m_mode == BitmapMode::Grey8 || m_mode == BitmapMode::Text ||
        m_mode == BitmapMode::Rgbi256) {
        bytesPerPixel = 1;
    }
    const bool binaryMode = m_mode == BitmapMode::Binary;
    const bool textMode = m_mode == BitmapMode::Text;

    const int sourceWidth = qMax(1, w / m_zoom);
    const int sourceHeight = qMax(1, h / m_zoom);
    const qint64 centerSourceIndex =
        static_cast<qint64>(sourceHeight / 2) * static_cast<qint64>(sourceWidth) + (sourceWidth / 2);
    const qint64 anchorRelative =
        binaryMode
            ? (static_cast<qint64>(m_centerAnchorOffset) - static_cast<qint64>(m_previewBaseOffset)) * 8
            : static_cast<qint64>(m_centerAnchorOffset) - static_cast<qint64>(m_previewBaseOffset);

    int overlapIdx = 0;

    for (int y = 0; y < h; ++y) {
        QRgb* scan = reinterpret_cast<QRgb*>(m_cachedImage.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;

            const int sx = static_cast<int>(qFloor((static_cast<double>(x - m_panDxPixels)) /
                                                   static_cast<double>(m_zoom)));
            const int sy = static_cast<int>(qFloor((static_cast<double>(y - m_panDyPixels)) /
                                                   static_cast<double>(m_zoom)));
            const qint64 sourcePixelIndex =
                static_cast<qint64>(sy) * static_cast<qint64>(sourceWidth) + static_cast<qint64>(sx);
            const qint64 deltaPixels = sourcePixelIndex - centerSourceIndex;
            const qint64 sourceIndex = anchorRelative + (deltaPixels * static_cast<qint64>(bytesPerPixel));

            quint64 absoluteByte0 = 0;
            quint64 absoluteByte1 = 0;
            quint64 absoluteByte2 = 0;
            bool validSample = false;
            int byteIndexForTextMode = -1;

            if (binaryMode) {
                const qint64 bitIndexSigned = anchorRelative + deltaPixels;
                if (bitIndexSigned >= 0 && bitIndexSigned < static_cast<qint64>(m_bytes.size()) * 8) {
                    validSample = true;
                    const quint64 bitIndex = static_cast<quint64>(bitIndexSigned);
                    const quint64 byteIndex = bitIndex / 8;
                    const int bitInByte = 7 - static_cast<int>(bitIndex % 8);
                    const unsigned char value =
                        static_cast<unsigned char>(m_bytes.at(static_cast<int>(byteIndex)));
                    const bool bitSet = ((value >> bitInByte) & 0x1U) != 0;
                    r = g = b = bitSet ? 255 : 0;
                    absoluteByte0 = m_previewBaseOffset + byteIndex;
                    absoluteByte1 = absoluteByte0;
                    absoluteByte2 = absoluteByte0;
                }
            } else if (sourceIndex >= 0 && sourceIndex < static_cast<qint64>(m_bytes.size())) {
                validSample = true;
                const quint64 byteIndex = static_cast<quint64>(sourceIndex);
                const int b0 = static_cast<unsigned char>(m_bytes.at(static_cast<int>(byteIndex)));
                const int b1 =
                    (byteIndex + 1 < static_cast<quint64>(m_bytes.size()))
                        ? static_cast<unsigned char>(m_bytes.at(static_cast<int>(byteIndex + 1)))
                        : 0;
                const int b2 =
                    (byteIndex + 2 < static_cast<quint64>(m_bytes.size()))
                        ? static_cast<unsigned char>(m_bytes.at(static_cast<int>(byteIndex + 2)))
                        : 0;

                switch (m_mode) {
                    case BitmapMode::Rgb24:
                        r = b0;
                        g = b1;
                        b = b2;
                        break;
                    case BitmapMode::Grey8:
                        r = g = b = b0;
                        break;
                    case BitmapMode::Grey24:
                        r = g = b = (b0 + b1 + b2) / 3;
                        break;
                    case BitmapMode::Rgbi256:
                        r = m_rgbi256Palette[static_cast<unsigned char>(b0)].red();
                        g = m_rgbi256Palette[static_cast<unsigned char>(b0)].green();
                        b = m_rgbi256Palette[static_cast<unsigned char>(b0)].blue();
                        break;
                    case BitmapMode::Text:
                        r = g = b = b0;
                        byteIndexForTextMode = static_cast<int>(byteIndex);
                        break;
                    case BitmapMode::Binary:
                        break;
                }
                absoluteByte0 = m_previewBaseOffset + byteIndex;
                absoluteByte1 = absoluteByte0 + 1;
                absoluteByte2 = absoluteByte0 + 2;
            }

            const int baseLuma = (r + g + b) / 3;

            if (validSample && textMode && byteIndexForTextMode >= 0 &&
                byteIndexForTextMode < m_textAnalysis.classes.size()) {
                const TextByteClass cls = m_textAnalysis.classes.at(byteIndexForTextMode);
                const int seqIdx =
                    (byteIndexForTextMode < m_textAnalysis.sequenceIndexByByte.size())
                        ? m_textAnalysis.sequenceIndexByByte.at(byteIndexForTextMode)
                        : -1;

                const bool inTerm = absoluteByte0 >= m_termStart && absoluteByte0 < m_termEnd;
                const bool inWindow = isHighlightedOffset(absoluteByte0);

                if (m_hoveredSequenceIndex >= 0 && seqIdx == m_hoveredSequenceIndex) {
                    r = 0xFF;
                    g = 0x69;
                    b = 0xB4;  // HotPink
                } else if (m_resultOverlayEnabled && inTerm) {
                    r = 0x1E;
                    g = 0x90;
                    b = 0xFF;  // DodgerBlue
                } else if (m_resultOverlayEnabled && inWindow) {
                    r = 0x22;
                    g = 0x8B;
                    b = 0x22;  // ForestGreen
                } else if (seqIdx >= 0) {
                    const QColor seqColor = colorForTextClass(cls);
                    r = seqColor.red();
                    g = seqColor.green();
                    b = seqColor.blue();
                } else {
                    r = g = b = static_cast<unsigned char>(m_bytes.at(byteIndexForTextMode));
                }
            } else if (validSample && m_resultOverlayEnabled) {
                const bool inTerm =
                    (absoluteByte0 >= m_termStart && absoluteByte0 < m_termEnd) ||
                    (absoluteByte1 >= m_termStart && absoluteByte1 < m_termEnd) ||
                    (absoluteByte2 >= m_termStart && absoluteByte2 < m_termEnd);
                const bool inWindow = isHighlightedOffset(absoluteByte0) ||
                                      isHighlightedOffset(absoluteByte1) ||
                                      isHighlightedOffset(absoluteByte2);
                const bool inOther = overlapsAnyOtherMatch(absoluteByte0, &overlapIdx) ||
                                     overlapsAnyOtherMatch(absoluteByte1, &overlapIdx) ||
                                     overlapsAnyOtherMatch(absoluteByte2, &overlapIdx);
                if (inTerm) {
                    applyOverlayColor(baseLuma, 0, 64, 255, r, g, b);
                } else if (inWindow && inOther) {
                    applyOverlayColor(baseLuma, 144, 255, 144, r, g, b);
                } else if (inWindow) {
                    applyOverlayColor(baseLuma, 0, 255, 0, r, g, b);
                } else if (inOther) {
                    applyOverlayColor(baseLuma, 144, 255, 144, r, g, b);
                }
            }

            if (validSample) {
                bool inExternalHover = false;
                if (m_externalHoverOffset.has_value()) {
                    const quint64 hoverOffset = m_externalHoverOffset.value();
                    inExternalHover = (absoluteByte0 == hoverOffset) || (absoluteByte1 == hoverOffset) ||
                                      (absoluteByte2 == hoverOffset);
                }

                bool inExternalSelection = false;
                if (m_externalSelectionRange.has_value()) {
                    const quint64 selStart = m_externalSelectionRange->first;
                    const quint64 selEnd = m_externalSelectionRange->second;
                    inExternalSelection =
                        ((absoluteByte0 >= selStart && absoluteByte0 < selEnd) ||
                         (absoluteByte1 >= selStart && absoluteByte1 < selEnd) ||
                         (absoluteByte2 >= selStart && absoluteByte2 < selEnd));
                }

                if (inExternalHover) {
                    r = 0xFF;
                    g = 0x14;
                    b = 0x93;  // Deep pink
                } else if (inExternalSelection) {
                    r = 0x00;
                    g = 0xFF;
                    b = 0xFF;  // Cyan
                }
            }

            scan[x] = qRgb(r, g, b);
        }
    }

    m_dirty = false;
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("BitmapViewWidget::rebuildImageIfNeeded: done elapsed=%1us")
                           .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
    }
}

void BitmapViewWidget::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    rebuildImageIfNeeded();
    if (m_cachedImage.isNull() || m_bytes.isEmpty()) {
        painter.setPen(palette().text().color());
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Bitmap view"));
        return;
    }
    painter.drawImage(rect(), m_cachedImage);
}

void BitmapViewWidget::wheelEvent(QWheelEvent* event) {
    const QPoint delta = event->angleDelta();
    if (delta.y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const int steps = delta.y() / 120;
    if (steps != 0) {
        setZoom(m_zoom + steps);
        event->accept();
        return;
    }
    QWidget::wheelEvent(event);
}

void BitmapViewWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragPanning = true;
        m_lastDragPos = event->pos();
        m_dragMoved = false;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void BitmapViewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragPanning && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->pos() - m_lastDragPos;
        if (!delta.isNull()) {
            m_panDxPixels += delta.x();
            m_panDyPixels += delta.y();
            m_lastDragPos = event->pos();
            m_dragMoved = true;
            markDirty();
            update();
        }
        event->accept();
        return;
    }

    const std::optional<int> byteIndex = byteIndexAtPoint(event->pos());
    if (byteIndex.has_value()) {
        const qint64 idx = static_cast<qint64>(byteIndex.value());
        const quint64 absoluteOffset =
            m_previewBaseOffset + static_cast<quint64>(byteIndex.value());
        if (m_lastHoverByteIndex != idx) {
            m_lastHoverByteIndex = idx;
            emit hoverAbsoluteOffsetChanged(absoluteOffset);
        }

        rebuildTextAnalysisIfNeeded();
        const int seqIdx =
            (byteIndex.value() >= 0 && byteIndex.value() < m_textAnalysis.sequenceIndexByByte.size())
                ? m_textAnalysis.sequenceIndexByByte.at(byteIndex.value())
                : -1;
        const bool hoveredSequenceChanged = (seqIdx != m_hoveredSequenceIndex);
        if (hoveredSequenceChanged) {
            m_hoveredSequenceIndex = seqIdx;
            if (m_mode == BitmapMode::Text) {
                markDirty();
                update();
            }
        }
        if (seqIdx >= 0) {
            m_pendingTooltipSequenceIndex = seqIdx;
            m_pendingTooltipByteIndex = byteIndex.value();
            m_pendingTooltipGlobalPos = event->globalPosition().toPoint();
            if (hoveredSequenceChanged || !QToolTip::isVisible()) {
                m_hoverTooltipTimer.stop();
                QToolTip::showText(m_pendingTooltipGlobalPos,
                                   tooltipForSequence(seqIdx, byteIndex.value()), this);
            } else {
                m_hoverTooltipTimer.start(40);
            }
        } else {
            m_hoverTooltipTimer.stop();
            m_pendingTooltipSequenceIndex = -1;
            m_pendingTooltipByteIndex = -1;
            QToolTip::hideText();
        }
    } else {
        if (m_lastHoverByteIndex >= 0) {
            m_lastHoverByteIndex = -1;
            emit hoverLeft();
        }
        m_hoverTooltipTimer.stop();
        m_pendingTooltipSequenceIndex = -1;
        m_pendingTooltipByteIndex = -1;
        if (m_hoveredSequenceIndex >= 0) {
            m_hoveredSequenceIndex = -1;
            QToolTip::hideText();
            markDirty();
            update();
        }
    }

    QWidget::mouseMoveEvent(event);
}

void BitmapViewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const bool dragged = m_dragMoved;
        if (m_dragPanning) {
            m_dragPanning = false;
            unsetCursor();
        }
        m_dragMoved = false;

        if (dragged) {
            const QPoint centerPoint(width() / 2, height() / 2);
            const std::optional<int> centeredByte = byteIndexAtPoint(centerPoint);
            if (centeredByte.has_value()) {
                const quint64 absoluteOffset =
                    m_previewBaseOffset + static_cast<quint64>(centeredByte.value());
                emit byteClicked(absoluteOffset);
            } else {
                resetPanOffset();
                markDirty();
                update();
            }
        } else {
            const std::optional<int> byteIndex = byteIndexAtPoint(event->pos());
            if (byteIndex.has_value()) {
                const quint64 absoluteOffset =
                    m_previewBaseOffset + static_cast<quint64>(byteIndex.value());
                emit byteClicked(absoluteOffset);
            }
        }

        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void BitmapViewWidget::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    m_hoverTooltipTimer.stop();
    m_pendingTooltipSequenceIndex = -1;
    m_pendingTooltipByteIndex = -1;
    if (m_lastHoverByteIndex >= 0) {
        m_lastHoverByteIndex = -1;
        emit hoverLeft();
    }
    if (m_hoveredSequenceIndex >= 0) {
        m_hoveredSequenceIndex = -1;
        QToolTip::hideText();
        markDirty();
        update();
    }
}

}  // namespace breco
