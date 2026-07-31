#include "panel/StructDeclarationPanel.h"

#include "struct/StructDeclarationParser.h"
#include "ui_StructDeclaration.h"

#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextCursor>

namespace breco {

StructDeclarationPanel::StructDeclarationPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::StructDeclaration>()) {
    m_ui->setupUi(this);
    connect(m_ui->structDeclarationEdit, &QPlainTextEdit::textChanged, this,
            [this]() { reparseDeclaration(); });
    connect(m_ui->entryComboBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) {
                highlightSelectedEntry();
                updateApplyEnabled();
            });
    connect(m_ui->applyButton, &QPushButton::clicked, this,
            &StructDeclarationPanel::applyRequested);
    updateStatusAndErrorHighlight();
}

StructDeclarationPanel::~StructDeclarationPanel() = default;

QPlainTextEdit* StructDeclarationPanel::structDeclarationEdit() const {
    return m_ui->structDeclarationEdit;
}

QComboBox* StructDeclarationPanel::entryComboBox() const { return m_ui->entryComboBox; }

QSpinBox* StructDeclarationPanel::entryCountSpinBox() const { return m_ui->entryCountSpinBox; }

QPushButton* StructDeclarationPanel::applyButton() const { return m_ui->applyButton; }

QVBoxLayout* StructDeclarationPanel::structDeclarationLayout() const {
    return m_ui->structDeclarationLayout;
}

bool StructDeclarationPanel::canVisualize() const {
    return m_parseValid && !m_graph.entryNames().isEmpty() &&
           m_ui->entryComboBox->currentIndex() >= 0;
}

QString StructDeclarationPanel::declarationText() const {
    return m_ui->structDeclarationEdit->toPlainText();
}

void StructDeclarationPanel::reparseDeclaration() {
    const QString text = m_ui->structDeclarationEdit->toPlainText();
    m_isEmpty = text.trimmed().isEmpty();
    const ParseResult result = parseStructDeclaration(text);
    m_parseValid = result.valid;
    m_parseError = result.errorMessage;
    m_parseErrorRange = result.errorRange;
    if (result.valid) {
        m_graph = result.graph;
    } else {
        m_graph.clear();
    }
    updateEntryCombo();
    updateStatusAndErrorHighlight();
    highlightSelectedEntry();
    updateApplyEnabled();
    emit parseStateChanged();
}

void StructDeclarationPanel::updateEntryCombo() {
    const QString previous = m_ui->entryComboBox->currentText();
    m_ui->entryComboBox->blockSignals(true);
    m_ui->entryComboBox->clear();
    if (m_parseValid) {
        m_ui->entryComboBox->addItems(m_graph.entryNames());
        const int idx = m_ui->entryComboBox->findText(previous);
        if (idx >= 0) {
            m_ui->entryComboBox->setCurrentIndex(idx);
        }
    }
    m_ui->entryComboBox->blockSignals(false);
}

void StructDeclarationPanel::updateApplyEnabled() {
    m_ui->applyButton->setEnabled(canVisualize());
}

void StructDeclarationPanel::updateStatusAndErrorHighlight() {
    QList<QTextEdit::ExtraSelection> selections;
    if (m_isEmpty) {
        m_ui->structDeclarationStatusLabel->setText(
            QStringLiteral("Insert a C struct"));
        m_ui->structDeclarationStatusLabel->setVisible(true);
    } else if (!m_parseValid) {
        m_ui->structDeclarationStatusLabel->setText(
            m_parseError.isEmpty()
                ? QStringLiteral("Invalid struct declaration")
                : m_parseError);
        m_ui->structDeclarationStatusLabel->setVisible(true);

        QTextEdit::ExtraSelection errorLine;
        errorLine.cursor = m_ui->structDeclarationEdit->textCursor();
        int errorPosition = m_parseErrorRange.start;
        if (errorPosition < 0) {
            errorPosition = 0;
        }
        const int maxPosition = m_ui->structDeclarationEdit->toPlainText().size();
        if (errorPosition > maxPosition) {
            errorPosition = maxPosition;
        }
        errorLine.cursor.setPosition(errorPosition);
        errorLine.cursor.select(QTextCursor::LineUnderCursor);
        errorLine.format.setBackground(QColor(255, 80, 80, 90));
        selections.push_back(errorLine);
    } else {
        m_ui->structDeclarationStatusLabel->setVisible(false);
    }
    if (!m_parseValid) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
    }
}

void StructDeclarationPanel::highlightSelectedEntry() {
    QList<QTextEdit::ExtraSelection> selections;
    if (!m_parseValid) {
        updateStatusAndErrorHighlight();
        return;
    }
    if (m_ui->entryComboBox->currentIndex() < 0) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
        return;
    }
    const QString entryName = m_ui->entryComboBox->currentText();
    const TextRange range = m_graph.nameRangeForEntry(entryName);
    if (range.start < 0 || range.end <= range.start) {
        m_ui->structDeclarationEdit->setExtraSelections(selections);
        return;
    }
    QTextEdit::ExtraSelection selection;
    selection.cursor = m_ui->structDeclarationEdit->textCursor();
    selection.cursor.setPosition(range.start);
    selection.cursor.setPosition(range.end, QTextCursor::KeepAnchor);
    selection.format.setBackground(QColor(255, 255, 0, 80));
    selections.push_back(selection);
    m_ui->structDeclarationEdit->setExtraSelections(selections);
}

}  // namespace breco
