#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QSplitter;
namespace Ui {
class DataViewByteAndBitmap;
}
QT_END_NAMESPACE

namespace breco {

class DataViewByteAndBitmapPanel : public QWidget {
    Q_OBJECT

public:
    explicit DataViewByteAndBitmapPanel(QWidget* parent = nullptr);
    ~DataViewByteAndBitmapPanel() override;

    QWidget* currentCharacterHost() const;
    QWidget* bitmapHost() const;
    QSplitter* splitter() const;

private:
    std::unique_ptr<Ui::DataViewByteAndBitmap> m_ui;
};

}  // namespace breco
