#pragma once

#include <QAbstractTableModel>
#include <QVector>

#include "model/ResultTypes.h"

namespace breco {

class ResultModel : public QAbstractTableModel {
    Q_OBJECT

public:
    explicit ResultModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setScanTargets(const QVector<ScanTarget>* scanTargets);
    void appendBatch(const QVector<MatchRecord>& matches);
    void clear();
    const MatchRecord* matchAt(int row) const;
    const QVector<MatchRecord>& allMatches() const;
    QString filePathForRow(int row) const;

private:
    QString filePathForMatch(const MatchRecord& match) const;

    QVector<MatchRecord> m_matches;
    const QVector<ScanTarget>* m_scanTargets = nullptr;
};

}  // namespace breco
