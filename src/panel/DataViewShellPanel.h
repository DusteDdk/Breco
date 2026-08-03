#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QRadioButton;
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
    };

    explicit DataViewShellPanel(ControlMode mode, QWidget* parent = nullptr);
    ~DataViewShellPanel() override;

    QRadioButton* littleEndianRadioButton() const;
    QRadioButton* bigEndianRadioButton() const;
    QLabel* textInterpretationLabel() const;
    QComboBox* textInterpretationComboBox() const;
    QLabel* bitmapModeLabel() const;
    QComboBox* bitmapModeComboBox() const;
    QToolButton* zoomOutButton() const;
    QLabel* zoomLabel() const;
    QToolButton* zoomInButton() const;
    QWidget* bodyHost() const;

    ControlMode controlMode() const;

private:
    std::unique_ptr<Ui::DataViewShell> m_ui;
    ControlMode m_mode;
};

}  // namespace breco
