#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QSplitter;
namespace Ui {
class DataViewStructured;
}
QT_END_NAMESPACE

namespace breco {

class DataViewStructuredPanel : public QWidget {
    Q_OBJECT

public:
    explicit DataViewStructuredPanel(QWidget* parent = nullptr);
    ~DataViewStructuredPanel() override;

    QWidget* structEditorHost() const;
    QWidget* structViewHost() const;
    QSplitter* splitter() const;

private:
    std::unique_ptr<Ui::DataViewStructured> m_ui;
};

}  // namespace breco
