#include "model/ResultModel.h"

namespace breco {

namespace {
QString formatApproxOffset(quint64 bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    long double scaled = static_cast<long double>(bytes);
    int unitIdx = 0;
    constexpr int kMaxUnitIdx = 5;
    while (scaled >= 1024.0L && unitIdx < kMaxUnitIdx) {
        scaled /= 1024.0L;
        ++unitIdx;
    }
    quint64 rounded = static_cast<quint64>(scaled + 0.5L);
    if (rounded >= 1024 && unitIdx < kMaxUnitIdx) {
        rounded = 1;
        ++unitIdx;
    }
    return QStringLiteral("%1 %2")
        .arg(QString::number(rounded), QString::fromLatin1(kUnits[unitIdx]));
}

QString formatSearchTimeMs(quint64 elapsedNs) {
    const quint64 elapsedMs = elapsedNs / 1000000ULL;
    return QStringLiteral("%1 ms").arg(elapsedMs);
}
}  // namespace

ResultModel::ResultModel(QObject* parent) : QAbstractTableModel(parent) {}

int ResultModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_matches.size();
}

int ResultModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return 4;
}

QVariant ResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_matches.size()) {
        return {};
    }

    const MatchRecord& match = m_matches.at(index.row());
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case 0:
                return QString::number(match.threadId);
            case 1:
                return filePathForMatch(match);
            case 2:
                return formatApproxOffset(match.offset);
            case 3:
                return formatSearchTimeMs(match.searchTimeNs);
            default:
                return {};
        }
    }

    if (role == Qt::TextAlignmentRole) {
        return (index.column() == 1) ? QVariant(Qt::AlignLeft | Qt::AlignVCenter)
                                     : QVariant(Qt::AlignRight | Qt::AlignVCenter);
    }

    if (role == Qt::ToolTipRole) {
        if (index.column() == 2) {
            return QStringLiteral("%1 B").arg(QString::number(match.offset));
        }
        if (index.column() == 3) {
            return QStringLiteral("%1 ns").arg(QString::number(match.searchTimeNs));
        }
    }

    return {};
}

QVariant ResultModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }

    switch (section) {
        case 0:
            return QStringLiteral("Thread");
        case 1:
            return QStringLiteral("Filename");
        case 2:
            return QStringLiteral("Offset");
        case 3:
            return QStringLiteral("Search time");
        default:
            return {};
    }
}

void ResultModel::setScanTargets(const QVector<ScanTarget>* scanTargets) {
    m_scanTargets = scanTargets;
    if (rowCount() > 0) {
        emit dataChanged(index(0, 1), index(rowCount() - 1, 1));
    }
}

void ResultModel::appendBatch(const QVector<MatchRecord>& matches) {
    if (matches.isEmpty()) {
        return;
    }

    const int start = m_matches.size();
    const int end = start + matches.size() - 1;
    beginInsertRows(QModelIndex(), start, end);
    for (const MatchRecord& match : matches) {
        m_matches.push_back(match);
    }
    endInsertRows();
}

void ResultModel::clear() {
    beginResetModel();
    m_matches.clear();
    endResetModel();
}

const MatchRecord* ResultModel::matchAt(int row) const {
    if (row < 0 || row >= m_matches.size()) {
        return nullptr;
    }
    return &m_matches.at(row);
}

const QVector<MatchRecord>& ResultModel::allMatches() const { return m_matches; }

QString ResultModel::filePathForRow(int row) const {
    if (row < 0 || row >= m_matches.size()) {
        return {};
    }
    return filePathForMatch(m_matches.at(row));
}

QString ResultModel::filePathForMatch(const MatchRecord& match) const {
    if (m_scanTargets == nullptr || match.scanTargetIdx < 0 ||
        match.scanTargetIdx >= m_scanTargets->size()) {
        return QStringLiteral("-");
    }
    return m_scanTargets->at(match.scanTargetIdx).filePath;
}

}  // namespace breco
