#include "panel/EditsPanel.h"

#include <QAbstractItemView>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace breco {

EditsPanel::EditsPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("editsTable"));
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("File"), QStringLiteral("Offset"),
                                        QStringLiteral("Length"), QStringLiteral("Original"),
                                        QStringLiteral("New"), QStringLiteral("Status")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_table);

    auto* buttons = new QHBoxLayout();
    m_applyButton = new QPushButton(QStringLiteral("Apply changes to file(s)."), this);
    m_saveAsButton = new QPushButton(QStringLiteral("Save as new file(s)."), this);
    buttons->addWidget(m_applyButton);
    buttons->addWidget(m_saveAsButton);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
        emit editActivated(row);
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item == nullptr || item->column() != 4 || m_queue == nullptr) {
            return;
        }
        emit editValueChanged(item->row(), item->text());
    });
    connect(m_applyButton, &QPushButton::clicked, this, &EditsPanel::applyRequested);
    connect(m_saveAsButton, &QPushButton::clicked, this, &EditsPanel::saveAsRequested);
}

void EditsPanel::setQueue(EditQueue* queue) {
    m_queue = queue;
    refresh();
}

void EditsPanel::refresh() {
    if (m_table == nullptr) {
        return;
    }
    const bool blocked = m_table->blockSignals(true);
    m_table->setRowCount(m_queue == nullptr ? 0 : m_queue->size());
    if (m_queue == nullptr) {
        m_table->blockSignals(blocked);
        return;
    }
    for (int row = 0; row < m_queue->size(); ++row) {
        const QueuedEdit& edit = m_queue->at(row);
        auto setText = [&](int column, const QString& text, bool editable = false) {
            auto* item = new QTableWidgetItem(text);
            if (!editable) {
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            }
            if (edit.matchesOriginal()) {
                item->setBackground(QColor(180, 230, 180));
            }
            m_table->setItem(row, column, item);
        };
        setText(0, edit.filePath);
        setText(1, QStringLiteral("0x%1").arg(edit.offset, 0, 16));
        setText(2, QString::number(edit.newBytes.size()));
        setText(3, EditQueue::bytesToHex(edit.originalBytes));
        setText(4, EditQueue::bytesToHex(edit.newBytes), true);
        setText(5, edit.matchesOriginal() ? QStringLiteral("unchanged")
                                          : QStringLiteral("queued"));
    }
    m_table->blockSignals(blocked);
}

QVector<int> EditsPanel::selectedRows() const {
    QVector<int> rows;
    if (m_table == nullptr) {
        return rows;
    }
    const auto selected = m_table->selectionModel()->selectedRows();
    for (const QModelIndex& index : selected) {
        rows.push_back(index.row());
    }
    return rows;
}

void EditsPanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Delete) {
        emit deleteRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace breco
