#pragma once

#include <QPoint>
#include <QPointF>
#include <QVector>
#include <QWidget>

#include "visualize/VisualizeData.h"

namespace breco {

class Cartesian2DView final : public QWidget {
    Q_OBJECT

public:
    explicit Cartesian2DView(QWidget* parent = nullptr);

    void setPoints(const QVector<VisualizePoint>& points);
    void setStyle(CartesianStyle style);
    void setTickDistance(double distance);
    const QVector<VisualizePoint>& points() const { return m_points; }
    CartesianStyle style() const { return m_style; }
    double tickDistance() const { return m_tickDistance; }
    double zoom() const { return m_zoom; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QPointF mapPoint(const VisualizePoint& point) const;
    QPointF originOnScreen() const;
    double fittedScale() const;

    QVector<VisualizePoint> m_points;
    CartesianStyle m_style = CartesianStyle::Line;
    double m_tickDistance = 0.0;
    QPointF m_pan;
    double m_zoom = 1.0;
    bool m_panning = false;
    QPoint m_lastMousePosition;
};

}  // namespace breco
