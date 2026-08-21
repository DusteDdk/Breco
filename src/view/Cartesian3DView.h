#pragma once

#include <QPoint>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include <optional>

#include "visualize/VisualizeData.h"

class QPainter;

namespace breco {

class Cartesian3DView final : public QWidget {
    Q_OBJECT

public:
    explicit Cartesian3DView(QWidget* parent = nullptr);

    void setPoints(const QVector<VisualizePoint>& points);
    void setStyle(CartesianStyle style);
    void setTickDistance(double distance);
    const QVector<VisualizePoint>& points() const { return m_points; }
    CartesianStyle style() const { return m_style; }
    double tickDistance() const { return m_tickDistance; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    double fittedScale() const;
    std::optional<QPointF> project(double x, double y, double z) const;
    void paintScene(QPainter& painter);

    QVector<VisualizePoint> m_points;
    CartesianStyle m_style = CartesianStyle::Line;
    double m_tickDistance = 0.0;
    QPointF m_pan;
    double m_zoom = 1.0;
    double m_yawDegrees = -35.0;
    double m_pitchDegrees = 25.0;
    bool m_panning = false;
    bool m_rotating = false;
    QPoint m_lastMousePosition;
};

}  // namespace breco
