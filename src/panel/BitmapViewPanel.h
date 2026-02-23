#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QToolButton;
class QVBoxLayout;
namespace Ui {
class BitmapViewPanel;
}
QT_END_NAMESPACE

namespace breco {

class BitmapViewPanel : public QWidget {
    Q_OBJECT

public:
    explicit BitmapViewPanel(QWidget* parent = nullptr);
    ~BitmapViewPanel() override;

    QComboBox* bitmapModeCombo() const;
    QCheckBox* resultOverlayCheckBox() const;
    QToolButton* bitmapZoomOutButton() const;
    QLabel* bitmapZoomLabel() const;
    QToolButton* bitmapZoomInButton() const;
    QWidget* bitmapViewContainer() const;
    QHBoxLayout* bitmapModeRowLayout() const;
    QVBoxLayout* bitmapViewPanelLayout() const;

private:
    std::unique_ptr<Ui::BitmapViewPanel> m_ui;
};

}  // namespace breco
