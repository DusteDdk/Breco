#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QVector>
#include <QWidget>

#include "visualize/VisualizeData.h"

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QThread;
class QWheelEvent;

namespace breco {

class VisualizeBitmapCanvas final : public QWidget {
    Q_OBJECT

public:
    explicit VisualizeBitmapCanvas(QWidget* parent = nullptr);
    ~VisualizeBitmapCanvas() override;

    void setVisualization(QVector<VisualizeBitmapPixel> pixels,
                          QByteArray packedBits, int bitsPerPixel,
                          bool hasPlot);
    void clear();

    double zoom() const { return m_zoom; }
    QSize logicalImageSize() const;
    QRectF imageRect() const;
    qsizetype sequentialColumns() const { return m_columns; }
    void setSequentialColumns(qsizetype columns);
    bool hasPlot() const { return m_hasPlot; }
    int bitsPerPixel() const { return m_bitsPerPixel; }
    bool isRendering() const { return m_renderThread != nullptr; }

signals:
    void zoomChanged(double zoom);
    void packingChanged(qsizetype columns, qsizetype rows);
    void inputExtentResized(double areaScale, bool keepEnd);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    enum class DragHandle {
        None,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    void updateLogicalSize(bool recenter);
    void applySequentialAspect(double aspect);
    void centerImage();
    void invalidateRenderCache();
    void ensureRendered(bool allowImmediateScale,
                        bool refineScaledPreview = false);
    void startBackgroundRender(double zoom);
    QSize renderSize(double zoom) const;
    int cacheKey(double zoom) const;
    DragHandle hitHandle(const QPointF& position) const;
    void updateCursorForPosition(const QPointF& position);
    void resizeShapeFromDrag(const QPointF& position);
    void resizeExtentFromDrag(const QPointF& position);
    bool isCornerHandle(DragHandle handle) const;
    qsizetype sourcePixelCount() const;
    void insertZoomOutCache(int key, const QImage& image);

    QVector<VisualizeBitmapPixel> m_pixels;
    QByteArray m_packedBits;
    int m_bitsPerPixel = 1;
    bool m_hasPlot = false;
    qsizetype m_columns = 1;
    qsizetype m_rows = 1;
    qint64 m_logicalWidth = 0;
    qint64 m_logicalHeight = 0;
    qint64 m_plotMinimumX = 0;
    qint64 m_plotMinimumY = 0;

    double m_zoom = 1.0;
    QPointF m_imageOrigin;
    bool m_originValid = false;
    bool m_keepColumns = false;
    bool m_hasContent = false;

    bool m_panning = false;
    DragHandle m_dragHandle = DragHandle::None;
    QPointF m_dragStart;
    QPointF m_dragOrigin;
    qsizetype m_dragColumns = 1;
    qsizetype m_dragRows = 1;
    qreal m_dragWidth = 1.0;
    qreal m_dragHeight = 1.0;

    QImage m_displayImage;
    double m_displayImageZoom = 1.0;
    QHash<int, QImage> m_zoomOutCache;
    QList<int> m_zoomOutLru;
    quint64 m_zoomOutCacheBytes = 0;
    QThread* m_renderThread = nullptr;
    quint64 m_renderGeneration = 0;
    bool m_refinePending = false;
};

}  // namespace breco
