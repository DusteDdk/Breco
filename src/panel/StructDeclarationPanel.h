#pragma once

#include <QWidget>

#include <memory>

#include "struct/StructureGraph.h"

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QVBoxLayout;
namespace Ui {
class StructDeclaration;
}
QT_END_NAMESPACE

namespace breco {

class StructDeclarationPanel : public QWidget {
    Q_OBJECT

public:
    explicit StructDeclarationPanel(QWidget* parent = nullptr);
    ~StructDeclarationPanel() override;

    QPlainTextEdit* structDeclarationEdit() const;
    QComboBox* entryComboBox() const;
    QSpinBox* entryCountSpinBox() const;
    QPushButton* applyButton() const;
    QVBoxLayout* structDeclarationLayout() const;

    const StructureGraph& structureGraph() const { return m_graph; }
    bool isParseValid() const { return m_parseValid; }
    bool canVisualize() const;
    QString declarationText() const;

    void reparseDeclaration();
    void highlightSelectedEntry();

signals:
    void applyRequested();
    void parseStateChanged();

private:
    void updateEntryCombo();
    void updateApplyEnabled();
    void updateStatusAndErrorHighlight();

    std::unique_ptr<Ui::StructDeclaration> m_ui;
    StructureGraph m_graph;
    bool m_parseValid = false;
    bool m_isEmpty = true;
    QString m_parseError;
    TextRange m_parseErrorRange;
};

}  // namespace breco
