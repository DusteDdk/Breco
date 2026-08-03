#include "panel/ScanControlsPanel.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QCompleter>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QSize>
#include <QStringListModel>
#include <QToolButton>
#include <QSpinBox>

#include "ui_ScanControlsPanel.h"

namespace breco {

namespace {

constexpr int kMaxSourcePathSuggestions = 5;

QSize largestSourceTypeIconSize() {
    QSize largest;
    for (const QString& iconPath :
         {QStringLiteral(":/res/none.png"), QStringLiteral(":/res/file.png"),
          QStringLiteral(":/res/dev.png"), QStringLiteral(":/res/dir.png")}) {
        largest = largest.expandedTo(QPixmap(iconPath).size());
    }
    return largest;
}

QStringList sourcePathSuggestions(const QString& text) {
    const QString normalizedText = QDir::fromNativeSeparators(text);
    if (normalizedText.isEmpty()) {
        return {};
    }

    const qsizetype separatorIndex = normalizedText.lastIndexOf(QLatin1Char('/'));
    const QString completionBase =
        separatorIndex >= 0 ? normalizedText.left(separatorIndex + 1) : QString();
    const QString directoryPath =
        separatorIndex >= 0 ? completionBase : QDir::currentPath();
    const QString fragment =
        separatorIndex >= 0 ? normalizedText.mid(separatorIndex + 1) : normalizedText;

    QDir directory(directoryPath);
    if (!directory.exists()) {
        return {};
    }

    QDir::Filters filters = QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot;
    if (fragment.startsWith(QLatin1Char('.'))) {
        filters |= QDir::Hidden;
    }

    QList<QFileInfo> matches;
    for (const QFileInfo& entry : directory.entryInfoList(filters, QDir::NoSort)) {
        if (entry.fileName().startsWith(fragment, Qt::CaseInsensitive)) {
            matches.push_back(entry);
        }
    }

    std::sort(matches.begin(), matches.end(),
              [&fragment](const QFileInfo& left, const QFileInfo& right) {
                  const QString leftName = left.fileName();
                  const QString rightName = right.fileName();
                  if (!fragment.isEmpty()) {
                      const bool leftCaseMatch = leftName.startsWith(fragment, Qt::CaseSensitive);
                      const bool rightCaseMatch = rightName.startsWith(fragment, Qt::CaseSensitive);
                      if (leftCaseMatch != rightCaseMatch) {
                          return leftCaseMatch;
                      }
                      if (leftName.size() != rightName.size()) {
                          return leftName.size() < rightName.size();
                      }
                  }
                  const int insensitiveOrder =
                      QString::compare(leftName, rightName, Qt::CaseInsensitive);
                  return insensitiveOrder != 0 ? insensitiveOrder < 0 : leftName < rightName;
              });

    QStringList suggestions;
    suggestions.reserve(kMaxSourcePathSuggestions);
    for (const QFileInfo& match : matches) {
        QString suggestion = completionBase + match.fileName();
        if (match.isDir()) {
            suggestion += QLatin1Char('/');
        }
        suggestions.push_back(QDir::toNativeSeparators(suggestion));
        if (suggestions.size() == kMaxSourcePathSuggestions) {
            break;
        }
    }
    return suggestions;
}

}  // namespace

ScanControlsPanel::ScanControlsPanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::ScanControlsPanel>()) {
    m_ui->setupUi(this);
    m_ui->lifecycleCard->setVisible(false);

    const QPixmap noneIcon(QStringLiteral(":/res/none.png"));
    m_ui->selectedSourceTypeIconLabel->setFixedSize(largestSourceTypeIconSize());
    m_ui->selectedSourceTypeIconLabel->setAlignment(Qt::AlignCenter);
    m_ui->selectedSourceTypeIconLabel->setPixmap(noneIcon);
    m_ui->selectedSourceTypeIconLabel->setToolTip(QStringLiteral("No source selected"));

    const QPixmap fileDeviceIcon(QStringLiteral(":/res/file.png"));
    m_ui->openFileButton->setText({});
    m_ui->openFileButton->setIcon(QIcon(fileDeviceIcon));
    m_ui->openFileButton->setIconSize(fileDeviceIcon.size());
    m_ui->openFileButton->setToolTip(QStringLiteral("Select file"));

    const QPixmap directoryIcon(QStringLiteral(":/res/dir.png"));
    m_ui->openDirButton->setText({});
    m_ui->openDirButton->setIcon(QIcon(directoryIcon));
    m_ui->openDirButton->setIconSize(directoryIcon.size());
    m_ui->openDirButton->setToolTip(QStringLiteral("Select directory"));

    m_sourcePathCompletionModel = new QStringListModel(this);
    m_sourcePathCompleter = new QCompleter(m_sourcePathCompletionModel, this);
    m_sourcePathCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_sourcePathCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_sourcePathCompleter->setFilterMode(Qt::MatchStartsWith);
    m_sourcePathCompleter->setMaxVisibleItems(kMaxSourcePathSuggestions);
    m_sourcePathCompleter->popup()->setFocusPolicy(Qt::NoFocus);
    m_sourcePathCompleter->popup()->setAttribute(Qt::WA_ShowWithoutActivating);
    m_ui->sourcePathLineEdit->setCompleter(m_sourcePathCompleter);
    m_ui->sourcePathLineEdit->installEventFilter(this);
    connect(m_ui->sourcePathLineEdit, &QLineEdit::textEdited, this,
            &ScanControlsPanel::updateSourcePathSuggestions);
}

ScanControlsPanel::~ScanControlsPanel() = default;

bool ScanControlsPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != m_ui->sourcePathLineEdit || event->type() != QEvent::KeyPress ||
        m_sourcePathCompleter == nullptr || !m_sourcePathCompleter->popup()->isVisible()) {
        return QWidget::eventFilter(watched, event);
    }

    auto* keyEvent = static_cast<QKeyEvent*>(event);
    QAbstractItemView* popup = m_sourcePathCompleter->popup();
    const int rowCount = popup->model()->rowCount();
    if (rowCount == 0) {
        return QWidget::eventFilter(watched, event);
    }

    if (keyEvent->key() == Qt::Key_Down || keyEvent->key() == Qt::Key_Up) {
        int row = popup->currentIndex().row();
        if (keyEvent->key() == Qt::Key_Down) {
            row = row < 0 ? 0 : qMin(row + 1, rowCount - 1);
        } else {
            row = row < 0 ? rowCount - 1 : qMax(row - 1, 0);
        }
        popup->setCurrentIndex(popup->model()->index(row, 0));
        popup->scrollTo(popup->currentIndex());
        return true;
    }

    if (keyEvent->key() == Qt::Key_Enter || keyEvent->key() == Qt::Key_Return) {
        QModelIndex selected = popup->currentIndex();
        if (!selected.isValid()) {
            selected = popup->model()->index(0, 0);
        }
        m_ui->sourcePathLineEdit->setText(selected.data().toString());
        m_ui->sourcePathLineEdit->setCursorPosition(m_ui->sourcePathLineEdit->text().size());
        popup->hide();
        return true;
    }

    return QWidget::eventFilter(watched, event);
}

QLineEdit* ScanControlsPanel::searchTermLineEdit() const { return m_ui->searchTermLineEdit; }

QCheckBox* ScanControlsPanel::ignoreCaseCheckBox() const { return m_ui->ignoreCaseCheckBox; }

QCheckBox* ScanControlsPanel::prefillOnMergeCheckBox() const {
    return m_ui->prefillOnMergeCheckBox;
}

QPushButton* ScanControlsPanel::startScanButton() const { return m_ui->startScanButton; }

QToolButton* ScanControlsPanel::openFileButton() const { return m_ui->openFileButton; }

QToolButton* ScanControlsPanel::openDirButton() const { return m_ui->openDirButton; }

QLabel* ScanControlsPanel::blockSizeLabel() const { return m_ui->blockSizeLabel; }

QSpinBox* ScanControlsPanel::blockSizeSpin() const { return m_ui->blockSizeSpin; }

QComboBox* ScanControlsPanel::blockSizeUnitCombo() const { return m_ui->blockSizeUnitCombo; }

QComboBox* ScanControlsPanel::workerCountCombo() const { return m_ui->workerCountCombo; }

QLabel* ScanControlsPanel::filesCountValueLabel() const { return m_ui->filesCountValueLabel; }

QLabel* ScanControlsPanel::searchSpaceValueLabel() const { return m_ui->searchSpaceValueLabel; }

QLabel* ScanControlsPanel::scannedValueLabel() const {
    return findChild<QLabel*>(QStringLiteral("scannedValueLabel"));
}

QProgressBar* ScanControlsPanel::scanProgressBar() const { return m_ui->scanProgressBar; }

QLabel* ScanControlsPanel::selectedSourceTypeIconLabel() const {
    return m_ui->selectedSourceTypeIconLabel;
}

QLineEdit* ScanControlsPanel::sourcePathLineEdit() const { return m_ui->sourcePathLineEdit; }

QWidget* ScanControlsPanel::advancedSearchGroup() const { return m_ui->advancedSearch; }

QWidget* ScanControlsPanel::lifecycleCard() const { return m_ui->lifecycleCard; }

QToolButton* ScanControlsPanel::hideLifecycleCardButton() const { return m_ui->btnHideLifecycleCard; }

QListWidget* ScanControlsPanel::lifecycleLogListWidget() const {
    return m_ui->lifecycleLogListWidget;
}

void ScanControlsPanel::showLifecycleCard() { m_ui->lifecycleCard->setVisible(true); }

void ScanControlsPanel::hideLifecycleCard() { m_ui->lifecycleCard->setVisible(false); }

void ScanControlsPanel::clearLifecycleLog() { m_ui->lifecycleLogListWidget->clear(); }

void ScanControlsPanel::appendLifecycleMessage(const QString& message) {
    m_ui->lifecycleLogListWidget->addItem(message);
    m_ui->lifecycleLogListWidget->scrollToBottom();
}

void ScanControlsPanel::updateSourcePathSuggestions(const QString& text) {
    const QStringList suggestions = sourcePathSuggestions(text);
    m_sourcePathCompletionModel->setStringList(suggestions);
    m_sourcePathCompleter->setCompletionPrefix(
        QDir::toNativeSeparators(QDir::fromNativeSeparators(text)));
    if (suggestions.isEmpty()) {
        m_sourcePathCompleter->popup()->hide();
        return;
    }
    m_sourcePathCompleter->complete();
}

}  // namespace breco
