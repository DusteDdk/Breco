#pragma once

#include <QWidget>

#include <memory>

QT_BEGIN_NAMESPACE
class QTableView;
namespace Ui {
class ResultsTablePanel;
}
QT_END_NAMESPACE

namespace breco {

class ResultsTablePanel : public QWidget {
    Q_OBJECT

public:
    explicit ResultsTablePanel(QWidget* parent = nullptr);
    ~ResultsTablePanel() override;

    QTableView* resultsTableView() const;

private:
    std::unique_ptr<Ui::ResultsTablePanel> m_ui;
};

}  // namespace breco
