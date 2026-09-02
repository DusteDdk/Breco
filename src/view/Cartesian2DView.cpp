#include "view/Cartesian2DView.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace breco {

Cartesian2DView::Cartesian2DView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(240, 180);
}

void Cartesian2DView::setPoints(const QVector<VisualizePoint>& points) {
    m_points = points;
    update();
}

void Cartesian2DView::setStyle(CartesianStyle style) {
    if (m_style == style) {
        return;
    }
    m_style = style;
    update();
}

void Cartesian2DView::setTickDistance(double distance) {
    distance = std::isfinite(distance) && distance > 0.0 ? distance : 0.0;
    if (qFuzzyCompare(m_tickDistance, distance)) {
        return;
    }
    m_tickDistance = distance;
    update();
}

QPointF Cartesian2DView::originOnScreen() const {
    return QPointF(width() * 0.5, height() * 0.5) + m_pan;
}

double Cartesian2DView::fittedScale() const {
    double maxX = 1.0;
    double maxY = 1.0;
    for (const VisualizePoint& point : m_points) {
        maxX = std::max(maxX, std::abs(point.x));
        maxY = std::max(maxY, std::abs(point.y));
    }
    const double availableWidth = std::max(1, width() - 40);
    const double availableHeight = std::max(1, height() - 40);
    return std::max(
        1.0e-9,
        std::min(availableWidth / (2.0 * maxX),
                 availableHeight / (2.0 * maxY)));
}

QPointF Cartesian2DView::mapPoint(const VisualizePoint& point) const {
    const double scale = fittedScale() * m_zoom;
    const QPointF origin = originOnScreen();
    return {origin.x() + point.x * scale, origin.y() - point.y * scale};
}

void Cartesian2DView::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());

    const QPointF origin = originOnScreen();
    QPen axisPen(palette().mid().color());
    axisPen.setWidthF(1.0);
    painter.setPen(axisPen);
    painter.drawLine(QPointF(0.0, origin.y()),
                     QPointF(width(), origin.y()));
    painter.drawLine(QPointF(origin.x(), 0.0),
                     QPointF(origin.x(), height()));

    if (m_points.isEmpty()) {
        painter.setPen(palette().text().color());
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No plottable points"));
        return;
    }

    if (m_tickDistance > 0.0) {
        double minimumX = m_points.first().x;
        double maximumX = minimumX;
        double minimumY = m_points.first().y;
        double maximumY = minimumY;
        for (const VisualizePoint& point : m_points) {
            minimumX = std::min(minimumX, point.x);
            maximumX = std::max(maximumX, point.x);
            minimumY = std::min(minimumY, point.y);
            maximumY = std::max(maximumY, point.y);
        }
        painter.setPen(axisPen);
        int drawn = 0;
        for (double value = std::ceil(minimumX / m_tickDistance) *
                            m_tickDistance;
             value <= maximumX && drawn < 100000;
             value += m_tickDistance, ++drawn) {
            const qreal x = mapPoint({value, 0.0, 0.0}).x();
            painter.drawLine(QPointF(x, origin.y() - 2.0),
                             QPointF(x, origin.y() + 2.0));
        }
        drawn = 0;
        for (double value = std::ceil(minimumY / m_tickDistance) *
                            m_tickDistance;
             value <= maximumY && drawn < 100000;
             value += m_tickDistance, ++drawn) {
            const qreal y = mapPoint({0.0, value, 0.0}).y();
            painter.drawLine(QPointF(origin.x() - 2.0, y),
                             QPointF(origin.x() + 2.0, y));
        }
    }

    if (m_style == CartesianStyle::Area ||
        m_style == CartesianStyle::Skin) {
        if (m_points.size() == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_points.first().color);
            painter.drawEllipse(mapPoint(m_points.first()), 3.0, 3.0);
        } else if (m_points.size() == 2) {
            painter.setPen(QPen(m_points.last().color, 1.5));
            painter.drawLine(mapPoint(m_points.first()),
                             mapPoint(m_points.last()));
        } else {
            for (qsizetype i = 2; i < m_points.size(); ++i) {
                QColor fill = m_points.at(i).color;
                fill.setAlpha(110);
                painter.setPen(QPen(m_points.at(i).color, 1.0));
                painter.setBrush(fill);
                painter.drawPolygon(
                    QPolygonF{mapPoint(m_points.at(i - 2)),
                              mapPoint(m_points.at(i - 1)),
                              mapPoint(m_points.at(i))});
            }
        }
        return;
    }

    if (m_style == CartesianStyle::Dot) {
        painter.setPen(Qt::NoPen);
        for (const VisualizePoint& point : m_points) {
            painter.setBrush(point.color);
            painter.drawEllipse(mapPoint(point), 3.0, 3.0);
        }
        return;
    }

    if (m_style == CartesianStyle::Bar) {
        for (const VisualizePoint& point : m_points) {
            const QPointF current = mapPoint(point);
            painter.setPen(QPen(point.color, 1.5));
            painter.drawLine(QPointF(current.x(), origin.y()), current);
        }
        return;
    }

    for (qsizetype i = 1; i < m_points.size(); ++i) {
        painter.setPen(QPen(m_points.at(i).color, 1.5));
        painter.drawLine(mapPoint(m_points.at(i - 1)),
                         mapPoint(m_points.at(i)));
    }
    if (m_points.size() == 1) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_points.first().color);
        painter.drawEllipse(mapPoint(m_points.first()), 3.0, 3.0);
    }
}

void Cartesian2DView::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }

    const QPointF cursor = event->position();
    const QPointF oldOrigin = originOnScreen();
    const double factor = std::pow(1.2, static_cast<double>(steps));
    const double newZoom = std::clamp(m_zoom * factor, 0.02, 500.0);
    const double applied = newZoom / m_zoom;
    m_zoom = newZoom;
    m_pan += (cursor - oldOrigin) * (1.0 - applied);
    update();
    event->accept();
}

void Cartesian2DView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastMousePosition = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Cartesian2DView::mouseMoveEvent(QMouseEvent* event) {
    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        m_pan += event->pos() - m_lastMousePosition;
        m_lastMousePosition = event->pos();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void Cartesian2DView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

}  // namespace breco
