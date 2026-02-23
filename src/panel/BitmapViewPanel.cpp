#include "panel/BitmapViewPanel.h"

#include "ui_BitmapViewPanel.h"

namespace breco {

BitmapViewPanel::BitmapViewPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::BitmapViewPanel>()) {
    m_ui->setupUi(this);
}

BitmapViewPanel::~BitmapViewPanel() = default;

QComboBox* BitmapViewPanel::bitmapModeCombo() const { return m_ui->bitmapModeCombo; }

QCheckBox* BitmapViewPanel::resultOverlayCheckBox() const { return m_ui->resultOverlayCheckBox; }

QToolButton* BitmapViewPanel::bitmapZoomOutButton() const { return m_ui->bitmapZoomOutButton; }

QLabel* BitmapViewPanel::bitmapZoomLabel() const { return m_ui->bitmapZoomLabel; }

QToolButton* BitmapViewPanel::bitmapZoomInButton() const { return m_ui->bitmapZoomInButton; }

QWidget* BitmapViewPanel::bitmapViewContainer() const { return m_ui->bitmapViewContainer; }

QHBoxLayout* BitmapViewPanel::bitmapModeRowLayout() const { return m_ui->bitmapModeRowLayout; }

QVBoxLayout* BitmapViewPanel::bitmapViewPanelLayout() const { return m_ui->bitmapViewPanelLayout; }

}  // namespace breco
