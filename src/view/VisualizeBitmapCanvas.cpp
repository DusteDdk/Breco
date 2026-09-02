#include "view/VisualizeBitmapCanvas.h"

#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QThread>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace breco {

namespace {

constexpr int kCheckerSize = 8;
constexpr qreal kHandleWidth = 3.0;
constexpr qreal kEdgeHitSlop = 6.0;
constexpr qreal kCornerSize = 4.0;
constexpr quint64 kMaximumRenderBytes = 512ULL * 1024ULL * 1024ULL;
constexpr quint64 kSynchronousRenderBytes = 16ULL * 1024ULL * 1024ULL;
constexpr double kMinimumZoom = 1.0 / 64.0;
constexpr double kMaximumZoom = 64.0;

void blendPixel(QImage* image, QVector<float>* weights, int x, int y,
                const QColor& color, double weight) {
    if (weight <= 0.0 || x < 0 || y < 0 || x >= image->width() ||
        y >= image->height()) {
        return;
    }
    const qsizetype index =
        static_cast<qsizetype>(y) * image->width() + x;
    const double previousWeight = weights->at(index);
    const double addedWeight = qMax(0.0, weight);
    const double totalWeight = previousWeight + addedWeight;
    if (totalWeight <= 0.0) {
        return;
    }
    const QColor previous = image->pixelColor(x, y);
    const auto channel = [&](int source, int destination) {
        return qBound(
            0,
            static_cast<int>(std::lround(
                (source * addedWeight + destination * previousWeight) /
                totalWeight)),
            255);
    };
    image->setPixelColor(
        x, y,
        QColor(channel(color.red(), previous.red()),
               channel(color.green(), previous.green()),
               channel(color.blue(), previous.blue()),
               channel(color.alpha(), previous.alpha())));
    (*weights)[index] = static_cast<float>(totalWeight);
}

void splat(QImage* image, QVector<float>* weights, double x, double y,
           const QColor& color) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    blendPixel(image, weights, x0, y0, color,
               (1.0 - fx) * (1.0 - fy));
    blendPixel(image, weights, x0 + 1, y0, color, fx * (1.0 - fy));
    blendPixel(image, weights, x0, y0 + 1, color, (1.0 - fx) * fy);
    blendPixel(image, weights, x0 + 1, y0 + 1, color, fx * fy);
}

QImage renderPixels(const QVector<VisualizeBitmapPixel>& pixels,
                    const QByteArray& packedBits, bool hasPlot,
                    qsizetype columns, qint64 logicalWidth,
                    qint64 logicalHeight, qint64 plotMinimumX,
                    qint64 plotMinimumY, const QSize& destinationSize) {
    if (logicalWidth <= 0 || logicalHeight <= 0 ||
        destinationSize.isEmpty()) {
        return {};
    }

    QImage image(destinationSize, QImage::Format_ARGB32);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);

    const bool enlarging =
        image.width() >= logicalWidth && image.height() >= logicalHeight;
    QVector<float> weights;
    if (!enlarging) {
        weights.resize(static_cast<qsizetype>(image.width()) *
                       image.height());
        std::fill(weights.begin(), weights.end(), 0.0F);
    }
    const qsizetype count =
        packedBits.isEmpty() ? pixels.size() : packedBits.size() * 8;
    for (qsizetype index = 0; index < count; ++index) {
        if ((index & 0xfff) == 0 &&
            QThread::currentThread()->isInterruptionRequested()) {
            return {};
        }
        VisualizeBitmapPixel packedPixel;
        const VisualizeBitmapPixel* pixel = nullptr;
        if (packedBits.isEmpty()) {
            pixel = &pixels.at(index);
        } else {
            const unsigned char byte =
                static_cast<unsigned char>(packedBits.at(index / 8));
            const bool set = (byte & (1U << (7 - index % 8))) != 0U;
            packedPixel.color = set ? QColor(Qt::white) : QColor(Qt::black);
            pixel = &packedPixel;
        }
        const long double translatedX =
            hasPlot
                ? static_cast<long double>(pixel->x) - plotMinimumX
                : static_cast<long double>(index % columns);
        const long double translatedY =
            hasPlot
                ? static_cast<long double>(pixel->y) - plotMinimumY
                : static_cast<long double>(index / columns);
        if (translatedX < 0.0L || translatedY < 0.0L ||
            translatedX >= logicalWidth || translatedY >= logicalHeight) {
            continue;
        }
        const qint64 sourceX = static_cast<qint64>(translatedX);
        const qint64 sourceY = static_cast<qint64>(translatedY);
        if (enlarging) {
            const int left = static_cast<int>(
                std::floor(sourceX * image.width() /
                           static_cast<double>(logicalWidth)));
            const int right = static_cast<int>(
                std::ceil((sourceX + 1) * image.width() /
                          static_cast<double>(logicalWidth)));
            const int top = static_cast<int>(
                std::floor(sourceY * image.height() /
                           static_cast<double>(logicalHeight)));
            const int bottom = static_cast<int>(
                std::ceil((sourceY + 1) * image.height() /
                          static_cast<double>(logicalHeight)));
            for (int y = top; y < bottom; ++y) {
                for (int x = left; x < right; ++x) {
                    image.setPixelColor(x, y, pixel->color);
                }
            }
            continue;
        }
        const double targetX =
            (static_cast<double>(sourceX) + 0.5) * image.width() /
                static_cast<double>(logicalWidth) -
            0.5;
        const double targetY =
            (static_cast<double>(sourceY) + 0.5) * image.height() /
                static_cast<double>(logicalHeight) -
            0.5;
        splat(&image, &weights, targetX, targetY, pixel->color);
    }
    return image;
}

}  // namespace

VisualizeBitmapCanvas::VisualizeBitmapCanvas(QWidget* parent)
    : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
}

VisualizeBitmapCanvas::~VisualizeBitmapCanvas() {
    ++m_renderGeneration;
    if (m_renderThread != nullptr) {
        m_renderThread->requestInterruption();
        m_renderThread->wait();
        disconnect(m_renderThread, nullptr, this, nullptr);
        delete m_renderThread;
        m_renderThread = nullptr;
    }
}

void VisualizeBitmapCanvas::setVisualization(
    QVector<VisualizeBitmapPixel> pixels, QByteArray packedBits,
    int bitsPerPixel, bool hasPlot) {
    const bool hadContent = m_hasContent;
    m_pixels = std::move(pixels);
    m_packedBits = std::move(packedBits);
    m_bitsPerPixel = bitsPerPixel;
    m_hasPlot = hasPlot;
    m_hasContent = sourcePixelCount() != 0;
    if (!hadContent) {
        m_originValid = false;
    }
    updateLogicalSize(!hadContent || !m_originValid);
    invalidateRenderCache();
    ensureRendered(false);
    update();
}

void VisualizeBitmapCanvas::clear() {
    m_pixels.clear();
    m_packedBits.clear();
    m_logicalWidth = 0;
    m_logicalHeight = 0;
    m_columns = 1;
    m_rows = 1;
    m_originValid = false;
    m_keepColumns = false;
    m_hasContent = false;
    invalidateRenderCache();
    update();
}

QSize VisualizeBitmapCanvas::logicalImageSize() const {
    return QSize(static_cast<int>(qMin<qint64>(
                     m_logicalWidth, std::numeric_limits<int>::max())),
                 static_cast<int>(qMin<qint64>(
                     m_logicalHeight, std::numeric_limits<int>::max())));
}

QRectF VisualizeBitmapCanvas::imageRect() const {
    return QRectF(m_imageOrigin,
                  QSizeF(static_cast<qreal>(m_logicalWidth) * m_zoom,
                         static_cast<qreal>(m_logicalHeight) * m_zoom));
}

void VisualizeBitmapCanvas::setSequentialColumns(qsizetype columns) {
    if (m_hasPlot || sourcePixelCount() == 0) {
        return;
    }
    const qsizetype bounded =
        qBound<qsizetype>(1, columns,
                          qMax<qsizetype>(1, sourcePixelCount()));
    if (m_columns == bounded) {
        return;
    }
    m_columns = bounded;
    m_keepColumns = true;
    updateLogicalSize(false);
    invalidateRenderCache();
    ensureRendered(true);
    emit packingChanged(m_columns, m_rows);
    update();
}

void VisualizeBitmapCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    for (int y = 0; y < height(); y += kCheckerSize) {
        for (int x = 0; x < width(); x += kCheckerSize) {
            const bool dark =
                ((x / kCheckerSize) + (y / kCheckerSize)) % 2 == 0;
            painter.fillRect(QRect(x, y, kCheckerSize, kCheckerSize),
                             dark ? QColor(0x99, 0x99, 0x99)
                                  : QColor(0xcc, 0xcc, 0xcc));
        }
    }

    if (sourcePixelCount() == 0 || m_logicalWidth <= 0 ||
        m_logicalHeight <= 0) {
        return;
    }
    const QRectF target = imageRect();
    if (!m_displayImage.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_displayImageZoom != m_zoom);
        painter.drawImage(target, m_displayImage);
    }
    QPen border(QColor(0xcc, 0xee, 0xff), kHandleWidth);
    border.setJoinStyle(Qt::MiterJoin);
    painter.setPen(border);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(target);
}

void VisualizeBitmapCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!m_originValid) {
        centerImage();
    }
}

void VisualizeBitmapCanvas::leaveEvent(QEvent* event) {
    if (!m_panning && m_dragHandle == DragHandle::None) {
        unsetCursor();
    }
    QWidget::leaveEvent(event);
}

void VisualizeBitmapCanvas::wheelEvent(QWheelEvent* event) {
    if (sourcePixelCount() == 0) {
        event->ignore();
        return;
    }
    const double steps = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(steps)) {
        event->ignore();
        return;
    }
    const double oldZoom = m_zoom;
    const double factor = std::pow(1.25, steps);
    m_zoom = qBound(kMinimumZoom, oldZoom * factor, kMaximumZoom);
    if (qFuzzyCompare(oldZoom, m_zoom)) {
        event->accept();
        return;
    }
    const QPointF cursor = event->position();
    m_imageOrigin =
        cursor - (cursor - m_imageOrigin) * (m_zoom / oldZoom);
    m_originValid = true;
    ++m_renderGeneration;
    if (m_renderThread != nullptr) {
        m_renderThread->requestInterruption();
    }
    m_refinePending = m_zoom > oldZoom;
    ensureRendered(true, m_refinePending);
    emit zoomChanged(m_zoom);
    update();
    event->accept();
}

void VisualizeBitmapCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && sourcePixelCount() != 0) {
        m_panning = true;
        m_dragStart = event->position();
        m_dragOrigin = m_imageOrigin;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_dragHandle = hitHandle(event->position());
        if (m_dragHandle != DragHandle::None) {
            m_dragStart = event->position();
            m_dragOrigin = m_imageOrigin;
            m_dragColumns = m_columns;
            m_dragRows = m_rows;
            m_dragWidth = m_logicalWidth * m_zoom;
            m_dragHeight = m_logicalHeight * m_zoom;
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void VisualizeBitmapCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning) {
        m_imageOrigin = m_dragOrigin + event->position() - m_dragStart;
        m_originValid = true;
        update();
        event->accept();
        return;
    }
    if (m_dragHandle != DragHandle::None) {
        if (isCornerHandle(m_dragHandle)) {
            resizeExtentFromDrag(event->position());
        } else {
            resizeShapeFromDrag(event->position());
        }
        event->accept();
        return;
    }
    updateCursorForPosition(event->position());
    QWidget::mouseMoveEvent(event);
}

void VisualizeBitmapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        updateCursorForPosition(event->position());
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton &&
        m_dragHandle != DragHandle::None) {
        m_dragHandle = DragHandle::None;
        ensureRendered(true);
        updateCursorForPosition(event->position());
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void VisualizeBitmapCanvas::updateLogicalSize(bool recenter) {
    if (sourcePixelCount() == 0) {
        m_logicalWidth = 0;
        m_logicalHeight = 0;
        return;
    }
    if (m_hasPlot) {
        m_plotMinimumX = qMin<qint64>(0, m_pixels.first().x);
        m_plotMinimumY = qMin<qint64>(0, m_pixels.first().y);
        qint64 maximumX = qMax<qint64>(0, m_pixels.first().x);
        qint64 maximumY = qMax<qint64>(0, m_pixels.first().y);
        for (const VisualizeBitmapPixel& pixel : m_pixels) {
            m_plotMinimumX = qMin(m_plotMinimumX, pixel.x);
            m_plotMinimumY = qMin(m_plotMinimumY, pixel.y);
            maximumX = qMax(maximumX, pixel.x);
            maximumY = qMax(maximumY, pixel.y);
        }
        const long double width =
            static_cast<long double>(maximumX) - m_plotMinimumX + 1.0L;
        const long double height =
            static_cast<long double>(maximumY) - m_plotMinimumY + 1.0L;
        m_logicalWidth = static_cast<qint64>(
            qBound(1.0L, width,
                   static_cast<long double>(
                       std::numeric_limits<qint64>::max())));
        m_logicalHeight = static_cast<qint64>(
            qBound(1.0L, height,
                   static_cast<long double>(
                       std::numeric_limits<qint64>::max())));
    } else {
        m_plotMinimumX = 0;
        m_plotMinimumY = 0;
        if (m_keepColumns) {
            m_columns = qBound<qsizetype>(
                1, m_columns, qMax<qsizetype>(1, sourcePixelCount()));
            m_rows =
                (sourcePixelCount() + m_columns - 1) / m_columns;
            m_keepColumns = false;
        } else {
            const double aspect =
                m_logicalWidth > 0 && m_logicalHeight > 0
                    ? static_cast<double>(m_logicalWidth) /
                          static_cast<double>(m_logicalHeight)
                    : 1.0;
            applySequentialAspect(aspect);
        }
        m_logicalWidth = m_columns;
        m_logicalHeight = qMax<qsizetype>(1, m_rows);
    }
    if (recenter || !m_originValid) {
        centerImage();
    }
}

void VisualizeBitmapCanvas::applySequentialAspect(double aspect) {
    const qsizetype count = qMax<qsizetype>(1, sourcePixelCount());
    aspect = std::isfinite(aspect) && aspect > 0.0 ? aspect : 1.0;
    m_columns = qBound<qsizetype>(
        1,
        static_cast<qsizetype>(
            std::lround(std::sqrt(static_cast<double>(count) * aspect))),
        count);
    m_rows = (count + m_columns - 1) / m_columns;
}

void VisualizeBitmapCanvas::centerImage() {
    m_imageOrigin =
        QPointF((width() - m_logicalWidth * m_zoom) / 2.0,
                (height() - m_logicalHeight * m_zoom) / 2.0);
    m_originValid = true;
}

void VisualizeBitmapCanvas::invalidateRenderCache() {
    ++m_renderGeneration;
    if (m_renderThread != nullptr) {
        m_renderThread->requestInterruption();
    }
    m_displayImage = {};
    m_zoomOutCache.clear();
    m_zoomOutLru.clear();
    m_zoomOutCacheBytes = 0;
    m_refinePending = false;
}

void VisualizeBitmapCanvas::ensureRendered(bool allowImmediateScale,
                                           bool refineScaledPreview) {
    m_refinePending = m_refinePending || refineScaledPreview;
    if (sourcePixelCount() == 0 || m_logicalWidth <= 0 ||
        m_logicalHeight <= 0) {
        m_displayImage = {};
        return;
    }
    const int key = cacheKey(m_zoom);
    if (m_zoomOutCache.contains(key)) {
        m_displayImage = m_zoomOutCache.value(key);
        m_displayImageZoom = m_zoom;
        m_zoomOutLru.removeAll(key);
        m_zoomOutLru.push_back(key);
        if (m_refinePending) {
            startBackgroundRender(m_zoom);
        }
        return;
    }

    const QSize destination = renderSize(m_zoom);
    if (allowImmediateScale && !m_displayImage.isNull() && m_zoom < 1.0) {
        m_displayImage =
            m_displayImage.scaled(destination, Qt::IgnoreAspectRatio,
                                  Qt::SmoothTransformation);
        m_displayImageZoom = m_zoom;
        insertZoomOutCache(key, m_displayImage);
        if (m_refinePending) {
            startBackgroundRender(m_zoom);
        }
        return;
    }

    const quint64 bytes =
        static_cast<quint64>(destination.width()) *
        static_cast<quint64>(destination.height()) * 4ULL;
    if ((!allowImmediateScale || m_displayImage.isNull()) &&
        bytes <= kSynchronousRenderBytes) {
        m_displayImage =
            renderPixels(m_pixels, m_packedBits, m_hasPlot, m_columns,
                         m_logicalWidth, m_logicalHeight, m_plotMinimumX,
                         m_plotMinimumY, destination);
        m_displayImageZoom = m_zoom;
        if (m_zoom < 1.0) {
            insertZoomOutCache(key, m_displayImage);
        }
        return;
    }
    startBackgroundRender(m_zoom);
}

void VisualizeBitmapCanvas::startBackgroundRender(double zoom) {
    if (m_renderThread != nullptr) {
        return;
    }
    const quint64 generation = m_renderGeneration;
    const QVector<VisualizeBitmapPixel> pixels = m_pixels;
    const QByteArray packedBits = m_packedBits;
    const bool hasPlot = m_hasPlot;
    const qsizetype columns = m_columns;
    const qint64 logicalWidth = m_logicalWidth;
    const qint64 logicalHeight = m_logicalHeight;
    const qint64 plotMinimumX = m_plotMinimumX;
    const qint64 plotMinimumY = m_plotMinimumY;
    const QSize destination = renderSize(zoom);
    auto rendered = std::make_shared<QImage>();

    QThread* thread = QThread::create(
        [pixels, packedBits, hasPlot, columns, logicalWidth, logicalHeight,
         plotMinimumX, plotMinimumY, destination, rendered]() {
            *rendered =
                renderPixels(pixels, packedBits, hasPlot, columns,
                             logicalWidth, logicalHeight, plotMinimumX,
                             plotMinimumY, destination);
        });
    m_renderThread = thread;
    connect(thread, &QThread::finished, this,
            [this, thread, generation, zoom, rendered]() {
                if (m_renderThread == thread) {
                    m_renderThread = nullptr;
                }
                thread->deleteLater();
                if (generation == m_renderGeneration &&
                    !rendered->isNull()) {
                    m_refinePending = false;
                    m_displayImage = std::move(*rendered);
                    m_displayImageZoom = zoom;
                    if (zoom < 1.0) {
                        insertZoomOutCache(cacheKey(zoom), m_displayImage);
                    }
                    update();
                } else if (generation != m_renderGeneration &&
                           m_renderThread == nullptr &&
                           sourcePixelCount() != 0) {
                    ensureRendered(false, m_refinePending);
                }
            });
    thread->start();
}

QSize VisualizeBitmapCanvas::renderSize(double zoom) const {
    long double width =
        qMax<long double>(1.0L, m_logicalWidth * static_cast<long double>(zoom));
    long double height = qMax<long double>(
        1.0L, m_logicalHeight * static_cast<long double>(zoom));
    constexpr long double maximumDimension = 65535.0L;
    const bool downsampling =
        width < static_cast<long double>(m_logicalWidth) ||
        height < static_cast<long double>(m_logicalHeight) ||
        width * height > kMaximumRenderBytes / 4.0L ||
        width > maximumDimension || height > maximumDimension;
    const long double bytesPerPixel = downsampling ? 8.0L : 4.0L;
    const long double maximumPixels =
        kMaximumRenderBytes / bytesPerPixel;
    if (width * height > maximumPixels) {
        const long double scale =
            std::sqrt(maximumPixels / (width * height));
        width *= scale;
        height *= scale;
    }
    if (width > maximumDimension || height > maximumDimension) {
        const long double scale =
            qMin(maximumDimension / width, maximumDimension / height);
        width *= scale;
        height *= scale;
    }
    return QSize(qMax(1, static_cast<int>(std::floor(width))),
                 qMax(1, static_cast<int>(std::floor(height))));
}

int VisualizeBitmapCanvas::cacheKey(double zoom) const {
    return qRound(zoom * 1'000'000.0);
}

VisualizeBitmapCanvas::DragHandle VisualizeBitmapCanvas::hitHandle(
    const QPointF& position) const {
    if (sourcePixelCount() == 0 || m_logicalWidth <= 0 ||
        m_logicalHeight <= 0) {
        return DragHandle::None;
    }
    const QRectF rect = imageRect();
    const QRectF expanded =
        rect.adjusted(-kEdgeHitSlop, -kEdgeHitSlop, kEdgeHitSlop,
                      kEdgeHitSlop);
    if (!expanded.contains(position)) {
        return DragHandle::None;
    }

    const QPointF cornerPoints[] = {
        rect.topLeft(), rect.topRight(), rect.bottomLeft(),
        rect.bottomRight()};
    const DragHandle cornerHandles[] = {
        DragHandle::TopLeft, DragHandle::TopRight, DragHandle::BottomLeft,
        DragHandle::BottomRight};
    qreal bestCornerDistance = std::numeric_limits<qreal>::max();
    DragHandle bestCorner = DragHandle::None;
    for (int index = 0; index < 4; ++index) {
        const qreal distance = std::max(
            std::abs(position.x() - cornerPoints[index].x()),
            std::abs(position.y() - cornerPoints[index].y()));
        if (distance < bestCornerDistance) {
            bestCornerDistance = distance;
            bestCorner = cornerHandles[index];
        }
    }
    if (bestCornerDistance <= kCornerSize) {
        return bestCorner;
    }
    if (m_hasPlot) {
        return DragHandle::None;
    }

    const struct {
        DragHandle handle;
        qreal distance;
    } edges[] = {
        {DragHandle::Left, std::abs(position.x() - rect.left())},
        {DragHandle::Right, std::abs(position.x() - rect.right())},
        {DragHandle::Top, std::abs(position.y() - rect.top())},
        {DragHandle::Bottom, std::abs(position.y() - rect.bottom())},
    };
    qreal bestEdgeDistance = std::numeric_limits<qreal>::max();
    DragHandle bestEdge = DragHandle::None;
    for (const auto& edge : edges) {
        if (edge.distance <= kEdgeHitSlop &&
            edge.distance < bestEdgeDistance) {
            bestEdgeDistance = edge.distance;
            bestEdge = edge.handle;
        }
    }
    return bestEdge;
}

void VisualizeBitmapCanvas::updateCursorForPosition(
    const QPointF& position) {
    switch (hitHandle(position)) {
        case DragHandle::Left:
        case DragHandle::Right:
            setCursor(Qt::SizeHorCursor);
            break;
        case DragHandle::Top:
        case DragHandle::Bottom:
            setCursor(Qt::SizeVerCursor);
            break;
        case DragHandle::TopLeft:
        case DragHandle::BottomRight:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case DragHandle::TopRight:
        case DragHandle::BottomLeft:
            setCursor(Qt::SizeBDiagCursor);
            break;
        default:
            unsetCursor();
            break;
    }
}

bool VisualizeBitmapCanvas::isCornerHandle(DragHandle handle) const {
    return handle == DragHandle::TopLeft ||
           handle == DragHandle::TopRight ||
           handle == DragHandle::BottomLeft ||
           handle == DragHandle::BottomRight;
}

void VisualizeBitmapCanvas::resizeShapeFromDrag(const QPointF& position) {
    if (sourcePixelCount() == 0 || m_hasPlot) {
        return;
    }
    const QPointF delta = position - m_dragStart;
    qsizetype columns = m_dragColumns;
    if (m_dragHandle == DragHandle::Left ||
        m_dragHandle == DragHandle::Right) {
        const double displayWidth = m_dragColumns * m_zoom;
        const double requested =
            displayWidth +
            (m_dragHandle == DragHandle::Right ? delta.x() : -delta.x());
        columns = static_cast<qsizetype>(
            std::lround(qMax(m_zoom, requested) / m_zoom));
    } else {
        const double displayHeight = m_dragRows * m_zoom;
        const double requested =
            displayHeight +
            (m_dragHandle == DragHandle::Bottom ? delta.y() : -delta.y());
        const qsizetype rows = qBound<qsizetype>(
            1,
            static_cast<qsizetype>(
                std::lround(qMax(m_zoom, requested) / m_zoom)),
            sourcePixelCount());
        columns = (sourcePixelCount() + rows - 1) / rows;
    }
    columns = qBound<qsizetype>(
        1, columns, qMax<qsizetype>(1, sourcePixelCount()));
    if (columns == m_columns) {
        return;
    }
    const qreal originalWidth = m_dragColumns * m_zoom;
    const qreal originalHeight = m_dragRows * m_zoom;
    m_columns = columns;
    m_keepColumns = true;
    updateLogicalSize(false);
    if (m_dragHandle == DragHandle::Left) {
        m_imageOrigin.setX(m_dragOrigin.x() + originalWidth -
                           m_logicalWidth * m_zoom);
    } else if (m_dragHandle == DragHandle::Top) {
        m_imageOrigin.setY(m_dragOrigin.y() + originalHeight -
                           m_logicalHeight * m_zoom);
    }
    invalidateRenderCache();
    ensureRendered(true);
    emit packingChanged(m_columns, m_rows);
    update();
}

void VisualizeBitmapCanvas::resizeExtentFromDrag(const QPointF& position) {
    if (sourcePixelCount() == 0 || m_dragWidth <= 0.0 ||
        m_dragHeight <= 0.0) {
        return;
    }

    QPointF pivot = m_dragOrigin;
    switch (m_dragHandle) {
        case DragHandle::TopLeft:
            pivot += QPointF(m_dragWidth, m_dragHeight);
            break;
        case DragHandle::TopRight:
            pivot += QPointF(0.0, m_dragHeight);
            break;
        case DragHandle::BottomLeft:
            pivot += QPointF(m_dragWidth, 0.0);
            break;
        default:
            break;
    }

    const QPointF startVec = m_dragStart - pivot;
    const QPointF nowVec = position - pivot;
    const qreal startLength = QPointF::dotProduct(startVec, startVec);
    if (startLength <= 0.0) {
        return;
    }
    const double linearScale = qMax(
        0.05, QPointF::dotProduct(nowVec, startVec) / startLength);
    const double areaScale = linearScale * linearScale;
    const bool keepEnd = m_dragHandle == DragHandle::TopLeft ||
                         m_dragHandle == DragHandle::TopRight;
    emit inputExtentResized(areaScale, keepEnd);
}

qsizetype VisualizeBitmapCanvas::sourcePixelCount() const {
    if (!m_packedBits.isEmpty()) {
        constexpr qsizetype maximum =
            std::numeric_limits<qsizetype>::max() / 8;
        return qMin(m_packedBits.size(), maximum) * 8;
    }
    return m_pixels.size();
}

void VisualizeBitmapCanvas::insertZoomOutCache(int key,
                                                const QImage& image) {
    if (image.isNull()) {
        return;
    }
    const quint64 bytes = static_cast<quint64>(image.sizeInBytes());
    if (bytes > kMaximumRenderBytes) {
        return;
    }
    if (const auto existing = m_zoomOutCache.constFind(key);
        existing != m_zoomOutCache.constEnd()) {
        m_zoomOutCacheBytes -=
            static_cast<quint64>(existing.value().sizeInBytes());
        m_zoomOutCache.remove(key);
        m_zoomOutLru.removeAll(key);
    }
    while (!m_zoomOutLru.isEmpty() &&
           m_zoomOutCacheBytes + bytes > kMaximumRenderBytes) {
        const int oldest = m_zoomOutLru.takeFirst();
        const auto cached = m_zoomOutCache.find(oldest);
        if (cached == m_zoomOutCache.end()) {
            continue;
        }
        m_zoomOutCacheBytes -=
            static_cast<quint64>(cached.value().sizeInBytes());
        m_zoomOutCache.erase(cached);
    }
    m_zoomOutCache.insert(key, image);
    m_zoomOutLru.push_back(key);
    m_zoomOutCacheBytes += bytes;
}

}  // namespace breco
