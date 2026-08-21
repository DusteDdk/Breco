#pragma once

#include <QScrollArea>

class QResizeEvent;
class QWidget;

namespace breco {

class BitmapViewWidget;

class ResizableBitmapCanvas final : public QScrollArea {
public:
    explicit ResizableBitmapCanvas(QWidget* parent = nullptr);

    BitmapViewWidget* bitmapView() const;

    void setRequiredSourcePixels(quint64 pixels);
    quint64 requiredSourcePixels() const;

    int bitmapWidth() const;
    int minimumBitmapWidth() const;
    void setBitmapWidth(int width);
    void resetBitmapWidth();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateCanvasLayout();

    QWidget* m_canvas = nullptr;
    BitmapViewWidget* m_bitmapView = nullptr;
    QWidget* m_resizeHandle = nullptr;
    quint64 m_requiredSourcePixels = 0;
    int m_requestedBitmapWidth = 0;
    int m_bitmapWidth = 1;
    bool m_userSized = false;
    bool m_updatingLayout = false;
};

}  // namespace breco
