#include "view/ResizableBitmapCanvas.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QWidget>
#include <functional>
#include <limits>

#include "view/BitmapViewWidget.h"

namespace breco {
namespace {

constexpr int kResizeHandleWidth = 10;
constexpr int kMinimumUserWidth = 32;

class HorizontalResizeHandle final : public QWidget {
public:
    HorizontalResizeHandle(std::function<int()> widthGetter,
                           std::function<void(int)> widthSetter, QWidget* parent)
        : QWidget(parent),
          m_widthGetter(std::move(widthGetter)),
          m_widthSetter(std::move(widthSetter)) {
        setCursor(Qt::SizeHorCursor);
        setToolTip(QStringLiteral("Drag horizontally to change bytes per row"));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QColor fill = palette().mid().color();
        fill.setAlpha(150);
        painter.fillRect(rect(), fill);

        painter.setPen(palette().light().color());
        const int center = width() / 2;
        painter.drawLine(center - 2, height() / 2 - 12, center - 2, height() / 2 + 12);
        painter.drawLine(center + 1, height() / 2 - 12, center + 1, height() / 2 + 12);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressGlobalX = event->globalPosition().x();
        m_pressWidth = m_widthGetter();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!(event->buttons() & Qt::LeftButton)) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        const int delta =
            qRound(event->globalPosition().x() - m_pressGlobalX);
        m_widthSetter(m_pressWidth + delta);
        event->accept();
    }

private:
    std::function<int()> m_widthGetter;
    std::function<void(int)> m_widthSetter;
    qreal m_pressGlobalX = 0.0;
    int m_pressWidth = 0;
};

quint64 divideRoundingUp(quint64 numerator, quint64 denominator) {
    if (denominator == 0) {
        return numerator;
    }
    return numerator / denominator + (numerator % denominator != 0 ? 1ULL : 0ULL);
}

}  // namespace

ResizableBitmapCanvas::ResizableBitmapCanvas(QWidget* parent) : QScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(false);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_canvas = new QWidget();
    m_canvas->setAutoFillBackground(true);
    m_canvas->setBackgroundRole(QPalette::Window);
    setWidget(m_canvas);

    m_bitmapView = new BitmapViewWidget(m_canvas);
    m_bitmapView->setDataOriginAtTopLeft(true);

    m_resizeHandle = new HorizontalResizeHandle(
        [this]() { return m_bitmapWidth; },
        [this](int width) { setBitmapWidth(width); }, m_canvas);
    m_resizeHandle->raise();

    updateCanvasLayout();
}

BitmapViewWidget* ResizableBitmapCanvas::bitmapView() const { return m_bitmapView; }

void ResizableBitmapCanvas::setRequiredSourcePixels(quint64 pixels) {
    m_requiredSourcePixels = pixels;
    updateCanvasLayout();
}

quint64 ResizableBitmapCanvas::requiredSourcePixels() const {
    return m_requiredSourcePixels;
}

int ResizableBitmapCanvas::bitmapWidth() const { return m_bitmapWidth; }

int ResizableBitmapCanvas::minimumBitmapWidth() const {
    const int scale =
        qMax(1, m_bitmapView->baseCellSize() * m_bitmapView->zoom());
    const int sourceRows = qMax(1, viewport()->height() / scale);
    const quint64 sourceColumns =
        divideRoundingUp(m_requiredSourcePixels, static_cast<quint64>(sourceRows));
    const quint64 requiredWidth =
        sourceColumns * static_cast<quint64>(scale);
    return qBound(kMinimumUserWidth,
                  static_cast<int>(qMin<quint64>(
                      requiredWidth, static_cast<quint64>(QWIDGETSIZE_MAX -
                                                          kResizeHandleWidth))),
                  QWIDGETSIZE_MAX - kResizeHandleWidth);
}

void ResizableBitmapCanvas::setBitmapWidth(int width) {
    m_userSized = true;
    m_requestedBitmapWidth =
        qBound(kMinimumUserWidth, width, QWIDGETSIZE_MAX - kResizeHandleWidth);
    updateCanvasLayout();
}

void ResizableBitmapCanvas::resetBitmapWidth() {
    m_userSized = false;
    m_requestedBitmapWidth = 0;
    updateCanvasLayout();
}

void ResizableBitmapCanvas::resizeEvent(QResizeEvent* event) {
    QScrollArea::resizeEvent(event);
    updateCanvasLayout();
}

void ResizableBitmapCanvas::updateCanvasLayout() {
    if (m_updatingLayout || m_canvas == nullptr || m_bitmapView == nullptr) {
        return;
    }
    QScopedValueRollback<bool> guard(m_updatingLayout, true);

    const int viewportWidth = qMax(1, viewport()->width());
    const int viewportHeight = qMax(1, viewport()->height());
    const int scale =
        qMax(1, m_bitmapView->baseCellSize() * m_bitmapView->zoom());
    const int maximumBitmapWidth = QWIDGETSIZE_MAX - kResizeHandleWidth;

    int bitmapHeight = viewportHeight;
    const int sourceRows = qMax(1, bitmapHeight / scale);
    const quint64 requiredColumns =
        divideRoundingUp(m_requiredSourcePixels, static_cast<quint64>(sourceRows));
    const quint64 requiredWidth =
        requiredColumns * static_cast<quint64>(scale);

    int minimumWidth = qMax(
        kMinimumUserWidth,
        static_cast<int>(qMin<quint64>(
            requiredWidth, static_cast<quint64>(maximumBitmapWidth))));

    if (requiredWidth > static_cast<quint64>(maximumBitmapWidth)) {
        const quint64 maximumColumns =
            qMax<quint64>(1, static_cast<quint64>(maximumBitmapWidth / scale));
        const quint64 requiredRows =
            divideRoundingUp(m_requiredSourcePixels, maximumColumns);
        const quint64 requiredHeight =
            requiredRows * static_cast<quint64>(scale);
        bitmapHeight = qMax(
            bitmapHeight,
            static_cast<int>(qMin<quint64>(
                requiredHeight, static_cast<quint64>(QWIDGETSIZE_MAX))));
    }

    const int defaultWidth = qMax(1, viewportWidth - kResizeHandleWidth);
    const int requestedWidth =
        m_userSized ? qMax(kMinimumUserWidth, m_requestedBitmapWidth)
                    : defaultWidth;
    m_bitmapWidth = qBound(minimumWidth, requestedWidth, maximumBitmapWidth);

    const int canvasWidth =
        qMax(viewportWidth, m_bitmapWidth + kResizeHandleWidth);
    const int canvasHeight = qMax(viewportHeight, bitmapHeight);
    m_canvas->resize(canvasWidth, canvasHeight);
    m_bitmapView->setGeometry(0, 0, m_bitmapWidth, bitmapHeight);
    m_resizeHandle->setGeometry(m_bitmapWidth, 0, kResizeHandleWidth,
                                bitmapHeight);
    m_resizeHandle->raise();
}

}  // namespace breco
