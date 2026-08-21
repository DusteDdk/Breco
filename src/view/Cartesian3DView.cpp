#include "view/Cartesian3DView.h"

#include <QApplication>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QVector4D>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <optional>

namespace breco {

Cartesian3DView::Cartesian3DView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 180);
    setMouseTracking(true);
}

void Cartesian3DView::setPoints(const QVector<VisualizePoint>& points) {
    m_points = points;
    update();
}

void Cartesian3DView::setStyle(CartesianStyle style) {
    if (style == CartesianStyle::Bar) {
        style = CartesianStyle::Dot;
    }
    if (m_style == style) {
        return;
    }
    m_style = style;
    update();
}

void Cartesian3DView::setTickDistance(double distance) {
    distance = std::isfinite(distance) && distance > 0.0 ? distance : 0.0;
    if (qFuzzyCompare(m_tickDistance, distance)) {
        return;
    }
    m_tickDistance = distance;
    update();
}

double Cartesian3DView::fittedScale() const {
    double extent = 1.0;
    for (const VisualizePoint& point : m_points) {
        extent = std::max(
            extent, std::sqrt(point.x * point.x + point.y * point.y +
                              point.z * point.z));
    }
    return 0.9 / extent;
}

std::optional<QPointF> Cartesian3DView::project(double x, double y,
                                                double z) const {
    if (width() <= 0 || height() <= 0) {
        return std::nullopt;
    }

    QMatrix4x4 model;
    model.rotate(static_cast<float>(m_pitchDegrees), 1.0F, 0.0F, 0.0F);
    model.rotate(static_cast<float>(m_yawDegrees), 0.0F, 1.0F, 0.0F);

    QMatrix4x4 view;
    view.translate(0.0F, 0.0F, static_cast<float>(-4.0 / m_zoom));

    QMatrix4x4 projection;
    projection.perspective(
        45.0F, static_cast<float>(width()) / static_cast<float>(height()),
        0.1F, 100.0F);

    const float scale = static_cast<float>(fittedScale());
    const QVector4D clip =
        projection * view * model *
        QVector4D(static_cast<float>(x) * scale, static_cast<float>(y) * scale,
                  static_cast<float>(z) * scale, 1.0F);
    if (clip.w() <= 0.0F) {
        return std::nullopt;
    }
    const QVector3D normalized = clip.toVector3DAffine();
    return QPointF((normalized.x() * 0.5 + 0.5) * width() + m_pan.x(),
                   (0.5 - normalized.y() * 0.5) * height() + m_pan.y());
}

void Cartesian3DView::paintScene(QPainter& painter) {
    const QRect viewport(0, 0, width(), height());
    painter.fillRect(viewport, QApplication::palette().base());

    const std::optional<QPointF> origin = project(0.0, 0.0, 0.0);
    const double axisExtent = 1.0 / fittedScale();
    if (origin.has_value()) {
        const struct {
            double x;
            double y;
            double z;
            QColor color;
        } axes[] = {
            {axisExtent, 0.0, 0.0, QColor(220, 70, 70)},
            {0.0, axisExtent, 0.0, QColor(70, 180, 90)},
            {0.0, 0.0, axisExtent, QColor(70, 120, 220)},
        };
        for (const auto& axis : axes) {
            const std::optional<QPointF> axisEnd =
                project(axis.x, axis.y, axis.z);
            if (axisEnd.has_value()) {
                painter.setPen(QPen(axis.color, 1.25));
                painter.drawLine(*origin, *axisEnd);
            }
        }
    }

    if (m_points.isEmpty()) {
        painter.setPen(QApplication::palette().text().color());
        painter.drawText(viewport, Qt::AlignCenter,
                         QStringLiteral("No plottable points"));
        return;
    }

    if (m_tickDistance > 0.0) {
        double minimumX = m_points.first().x;
        double maximumX = minimumX;
        double minimumY = m_points.first().y;
        double maximumY = minimumY;
        double minimumZ = m_points.first().z;
        double maximumZ = minimumZ;
        for (const VisualizePoint& point : m_points) {
            minimumX = std::min(minimumX, point.x);
            maximumX = std::max(maximumX, point.x);
            minimumY = std::min(minimumY, point.y);
            maximumY = std::max(maximumY, point.y);
            minimumZ = std::min(minimumZ, point.z);
            maximumZ = std::max(maximumZ, point.z);
        }
        const double startX =
            std::ceil(minimumX / m_tickDistance) * m_tickDistance;
        const double startY =
            std::ceil(minimumY / m_tickDistance) * m_tickDistance;
        const double startZ =
            std::ceil(minimumZ / m_tickDistance) * m_tickDistance;
        painter.setPen(QPen(QApplication::palette().mid().color(), 1.0));
        int drawn = 0;
        for (double x = startX; x <= maximumX && drawn < 250000;
             x += m_tickDistance) {
            for (double y = startY; y <= maximumY && drawn < 250000;
                 y += m_tickDistance) {
                for (double z = startZ; z <= maximumZ && drawn < 250000;
                     z += m_tickDistance, ++drawn) {
                    if (const auto tick = project(x, y, z);
                        tick.has_value()) {
                        painter.drawPoint(*tick);
                    }
                }
            }
        }
    }

    if (m_style == CartesianStyle::Area && m_points.size() >= 2) {
        painter.setPen(Qt::NoPen);
        for (qsizetype i = 1; i < m_points.size(); ++i) {
            const VisualizePoint& a = m_points.at(i - 1);
            const VisualizePoint& b = m_points.at(i);
            const std::optional<QPointF> aTop = project(a.x, a.y, a.z);
            const std::optional<QPointF> bTop = project(b.x, b.y, b.z);
            const std::optional<QPointF> bFloor = project(b.x, b.y, 0.0);
            const std::optional<QPointF> aFloor = project(a.x, a.y, 0.0);
            if (aTop && bTop && bFloor && aFloor) {
                QColor fill = b.color;
                fill.setAlpha(70);
                painter.setBrush(fill);
                QPolygonF ribbon;
                ribbon << *aTop << *bTop << *bFloor << *aFloor;
                painter.drawPolygon(ribbon);
            }
        }
    }

    if (m_style == CartesianStyle::Skin) {
        if (m_points.size() == 1) {
            if (const auto point =
                    project(m_points.first().x, m_points.first().y,
                            m_points.first().z);
                point.has_value()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_points.first().color);
                painter.drawEllipse(*point, 3.0, 3.0);
            }
        } else if (m_points.size() == 2) {
            const auto first =
                project(m_points.first().x, m_points.first().y,
                        m_points.first().z);
            const auto second =
                project(m_points.last().x, m_points.last().y,
                        m_points.last().z);
            if (first && second) {
                painter.setPen(QPen(m_points.last().color, 1.5));
                painter.drawLine(*first, *second);
            }
        } else {
            for (qsizetype i = 2; i < m_points.size(); ++i) {
                const auto a =
                    project(m_points.at(i - 2).x, m_points.at(i - 2).y,
                            m_points.at(i - 2).z);
                const auto b =
                    project(m_points.at(i - 1).x, m_points.at(i - 1).y,
                            m_points.at(i - 1).z);
                const auto c =
                    project(m_points.at(i).x, m_points.at(i).y,
                            m_points.at(i).z);
                if (a && b && c) {
                    QColor fill = m_points.at(i).color;
                    fill.setAlpha(110);
                    painter.setPen(QPen(m_points.at(i).color, 1.0));
                    painter.setBrush(fill);
                    painter.drawPolygon(QPolygonF{*a, *b, *c});
                }
            }
        }
        return;
    }

    std::optional<QPointF> previous;
    for (qsizetype i = 0; i < m_points.size(); ++i) {
        const VisualizePoint& point = m_points.at(i);
        const std::optional<QPointF> current =
            project(point.x, point.y, point.z);
        if (!current.has_value()) {
            previous.reset();
            continue;
        }
        if (m_style != CartesianStyle::Dot && previous.has_value()) {
            painter.setPen(QPen(point.color, 1.5));
            painter.drawLine(*previous, *current);
        }
        if (m_style == CartesianStyle::Dot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(point.color);
            painter.drawEllipse(*current, 3.0, 3.0);
        }
        previous = current;
    }
    if (m_points.size() == 1 && m_style != CartesianStyle::Dot) {
        if (const auto point =
                project(m_points.first().x, m_points.first().y,
                        m_points.first().z);
            point.has_value()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_points.first().color);
            painter.drawEllipse(*point, 3.0, 3.0);
        }
    }
}

void Cartesian3DView::paintEvent(QPaintEvent* event) {
    QWidget::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintScene(painter);
}

void Cartesian3DView::wheelEvent(QWheelEvent* event) {
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    m_zoom = std::clamp(m_zoom * std::pow(1.2, static_cast<double>(steps)),
                        0.05, 50.0);
    update();
    event->accept();
}

void Cartesian3DView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastMousePosition = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_rotating = true;
        m_lastMousePosition = event->pos();
        setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Cartesian3DView::mouseMoveEvent(QMouseEvent* event) {
    const QPoint delta = event->pos() - m_lastMousePosition;
    if (m_panning && (event->buttons() & Qt::MiddleButton)) {
        m_pan += delta;
        m_lastMousePosition = event->pos();
        update();
        event->accept();
        return;
    }
    if (m_rotating && (event->buttons() & Qt::LeftButton)) {
        m_yawDegrees += delta.x() * 0.5;
        m_pitchDegrees =
            std::clamp(m_pitchDegrees + delta.y() * 0.5, -89.0, 89.0);
        m_lastMousePosition = event->pos();
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void Cartesian3DView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        unsetCursor();
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton && m_rotating) {
        m_rotating = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

}  // namespace breco
