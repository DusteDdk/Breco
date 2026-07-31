#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QRadioButton;
class QStackedWidget;
class QToolButton;
namespace Ui {
class DataViewShell;
}
QT_END_NAMESPACE

namespace breco {

class DataViewShellPanel : public QWidget {
    Q_OBJECT

public:
    enum class ControlMode {
        Raw,
        Struct,
        Image,
    };

    explicit DataViewShellPanel(QWidget* parent = nullptr);
    ~DataViewShellPanel() override;

    QComboBox* modeComboBox() const;
    QRadioButton* littleEndianRadioButton() const;
    QRadioButton* bigEndianRadioButton() const;
    QLabel* textInterpretationLabel() const;
    QComboBox* textInterpretationComboBox() const;
    QLabel* bitmapModeLabel() const;
    QComboBox* bitmapModeComboBox() const;
    QToolButton* zoomOutButton() const;
    QLabel* zoomLabel() const;
    QToolButton* zoomInButton() const;
    QStackedWidget* bodyStackedWidget() const;

    void setRawControlsVisible(bool visible);
    void setControlMode(ControlMode mode);

private:
    std::unique_ptr<Ui::DataViewShell> m_ui;
};

}  // namespace breco
