#include "panel/DataViewByteAndBitmapPanel.h"

#include "ui_DataViewByteAndBitmap.h"

namespace breco {

DataViewByteAndBitmapPanel::DataViewByteAndBitmapPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::DataViewByteAndBitmap>()) {
    m_ui->setupUi(this);
}

DataViewByteAndBitmapPanel::~DataViewByteAndBitmapPanel() = default;

QWidget* DataViewByteAndBitmapPanel::currentCharacterHost() const {
    return m_ui->currentCharacterHost;
}

QWidget* DataViewByteAndBitmapPanel::bitmapHost() const { return m_ui->bitmapHost; }

QSplitter* DataViewByteAndBitmapPanel::splitter() const { return m_ui->byteBitmapSplitter; }

}  // namespace breco
