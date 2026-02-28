#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QRadioButton;
class QVBoxLayout;
namespace Ui {
class TextViewPanel;
}
QT_END_NAMESPACE

namespace breco {

class TextViewPanel : public QWidget {
    Q_OBJECT

public:
    explicit TextViewPanel(QWidget* parent = nullptr);
    ~TextViewPanel() override;

    QComboBox* textModeCombo() const;
    QRadioButton* stringModeRadioButton() const;
    QRadioButton* byteModeRadioButton() const;
    QCheckBox* wrapModeCheckBox() const;
    QCheckBox* collapseCheckBox() const;
    QCheckBox* breatheCheckBox() const;
    QComboBox* newlineModeComboBox() const;
    QCheckBox* monospaceCheckBox() const;
    QComboBox* bytesPerLineComboBox() const;
    QWidget* textViewContainer() const;
    QHBoxLayout* textModeRowLayout() const;
    QVBoxLayout* textViewPanelLayout() const;

private:
    std::unique_ptr<Ui::TextViewPanel> m_ui;
};

}  // namespace breco
