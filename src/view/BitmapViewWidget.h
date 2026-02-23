#pragma once

#include <QByteArray>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPoint>
#include <QPair>
#include <QTimer>
#include <QWheelEvent>
#include <QVector>
#include <QWidget>
#include <array>
#include <optional>

#include "model/ResultTypes.h"
#include "text/TextSequenceAnalyzer.h"

namespace breco {

class BitmapViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit BitmapViewWidget(QWidget* parent = nullptr);

    void setData(const QByteArray& bytes);
    void setMode(BitmapMode mode);
    void setResultOverlayEnabled(bool enabled);
    void setResultHighlight(quint64 absoluteOffset, quint32 validBefore, quint32 termLength,
                            quint32 validAfter, quint64 previewBaseOffset);
    void setZoom(int zoom);
    int zoom() const;
    quint64 viewportByteCapacity() const;
    void setCenterAnchorOffset(quint64 absoluteOffset);
    void setOverlapIntervals(const QVector<QPair<quint64, quint64>>& intervals);
    void setTextMode(TextInterpretationMode mode);
    void setExternalHoverOffset(std::optional<quint64> absoluteOffset);
    void setExternalSelectionRange(std::optional<QPair<quint64, quint64>> absoluteRange);

signals:
    void zoomChanged(int zoom);
    void hoverAbsoluteOffsetChanged(quint64 offset);
    void hoverLeft();
    void byteClicked(quint64 offset);

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool isHighlightedOffset(quint64 absoluteByteOffset) const;
    void markDirty();
    void markTextAnalysisDirty();
    void rebuildImageIfNeeded();
    void rebuildTextAnalysisIfNeeded();
    void rebuildMergedIntervals();
    bool overlapsAnyOtherMatch(quint64 absoluteByteOffset, int* intervalIdx) const;
    void resetPanOffset();
    void applyOverlayColor(int baseLuma, int overlayR, int overlayG, int overlayB, int& outR, int& outG,
                           int& outB) const;
    std::optional<int> byteIndexAtPoint(const QPoint& point) const;
    QString tooltipForSequence(int sequenceIndex, int hoveredByteIndex) const;
    static QColor colorForTextClass(TextByteClass cls);
    static std::array<QColor, 256> buildRgbi256Palette();

    QByteArray m_bytes;
    BitmapMode m_mode = BitmapMode::Rgb24;
    TextInterpretationMode m_textMode = TextInterpretationMode::Ascii;
    bool m_resultOverlayEnabled = true;
    quint64 m_resultOffset = 0;
    quint32 m_validBefore = 0;
    quint32 m_termLength = 0;
    quint32 m_validAfter = 0;
    quint64 m_previewBaseOffset = 0;
    int m_zoom = 1;
    quint64 m_centerAnchorOffset = 0;
    quint64 m_beforeStart = 0;
    quint64 m_termStart = 0;
    quint64 m_termEnd = 0;
    quint64 m_afterEnd = 0;
    QVector<QPair<quint64, quint64>> m_overlapIntervals;
    QVector<QPair<quint64, quint64>> m_mergedOverlapIntervals;
    int m_panDxPixels = 0;
    int m_panDyPixels = 0;
    bool m_dragPanning = false;
    QPoint m_lastDragPos;
    bool m_dragMoved = false;
    QTimer m_hoverTooltipTimer;
    int m_pendingTooltipSequenceIndex = -1;
    int m_pendingTooltipByteIndex = -1;
    QPoint m_pendingTooltipGlobalPos;
    int m_hoveredSequenceIndex = -1;
    qint64 m_lastHoverByteIndex = -1;
    bool m_dirty = true;
    bool m_textAnalysisDirty = true;
    std::optional<quint64> m_externalHoverOffset;
    std::optional<QPair<quint64, quint64>> m_externalSelectionRange;
    int m_cachedWidth = 0;
    int m_cachedHeight = 0;
    QImage m_cachedImage;
    TextAnalysisResult m_textAnalysis;
    std::array<QColor, 256> m_rgbi256Palette{};
};

}  // namespace breco
