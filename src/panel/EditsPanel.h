#pragma once

#include <QWidget>
#include <memory>

#include "edit/EditQueue.h"

QT_BEGIN_NAMESPACE
class QPushButton;
class QTableWidget;
QT_END_NAMESPACE

namespace breco {

class EditsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EditsPanel(QWidget* parent = nullptr);

    void setQueue(EditQueue* queue);
    void refresh();
    QTableWidget* table() const { return m_table; }
    QPushButton* applyButton() const { return m_applyButton; }
    QPushButton* saveAsButton() const { return m_saveAsButton; }
    QVector<int> selectedRows() const;

signals:
    void editActivated(int index);
    void deleteRequested();
    void applyRequested();
    void saveAsRequested();
    void editValueChanged(int index, const QString& hex);

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    EditQueue* m_queue = nullptr;
    QTableWidget* m_table = nullptr;
    QPushButton* m_applyButton = nullptr;
    QPushButton* m_saveAsButton = nullptr;
};

}  // namespace breco
