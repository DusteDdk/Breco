#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSize>
#include <QSplitter>
#include <QSpinBox>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTableView>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#endif

#include <memory>
#include <utility>

#define private public
#include "app/MainWindow.h"
#undef private
#include "io/ProtectedSourceOpener.h"
#include "panel/CurrentByteInfoPanel.h"
#include "panel/DataViewByteAndBitmapPanel.h"
#include "panel/DataViewImagePanel.h"
#include "panel/DataViewShellPanel.h"
#include "panel/DataViewStructuredPanel.h"
#include "panel/HexViewControlsPanel.h"
#include "panel/ResultsTablePanel.h"
#include "panel/ScanControlsPanel.h"
#include "panel/StructDataViewPanel.h"
#include "panel/StructModeLeftPanel.h"
#include "struct/StructVisualizedTreeModel.h"
#include "view/TextViewWidget.h"

namespace {

class FakeProtectedSourceOpener final : public breco::ProtectedSourceOpener {
public:
    ~FakeProtectedSourceOpener() override {
#ifdef Q_OS_UNIX
        if (result.fd >= 0) {
            ::close(result.fd);
        }
#endif
    }

    bool isAvailable(const QString&, breco::ProtectedSourceKind) const override { return available; }

    breco::ProtectedOpenResult open(const QString&, breco::ProtectedSourceKind) override {
        ++openCount;
        breco::ProtectedOpenResult out = result;
        result.fd = -1;
        return out;
    }

    bool available = false;
    int openCount = 0;
    breco::ProtectedOpenResult result = breco::ProtectedOpenResult::unavailable();
};

class SettingsValueGuard {
public:
    explicit SettingsValueGuard(QString key) : m_key(std::move(key)) {
        QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
        m_existed = settings.contains(m_key);
        m_value = settings.value(m_key);
    }

    ~SettingsValueGuard() {
        QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
        if (m_existed) {
            settings.setValue(m_key, m_value);
        } else {
            settings.remove(m_key);
        }
    }

private:
    QString m_key;
    QVariant m_value;
    bool m_existed = false;
};

class MainWindowIntegrationTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void lifecycleCardLogsAndResetsPerScan();
    void selectingResultRowUpdatesPreviewBuffers();
    void twoColumnCompositionAndDataViewToolbar();
    void navigatorLabelsAndDataViewEndianFollowSelection();
    void currentBytePanelShowsEndianAndWidthAwareValues();
    void shiftMarksCurrentBufferDirtyAndRestoresOnDeselect();
    void sourcePathInputValidatesAndOpensTargets();
    void sourcePathAutocompleteKeepsTypingFocusAndLimitsSuggestions();
#ifdef Q_OS_UNIX
    void protectedSourceOpenUsesConfirmationAndFd();
#endif
    void structModePanelPreviewViewsAndSnippets();
    void restoresStructDefinitionAndPreviewOnStartup();
    void imageModeScansAndJumpsToResult();
    void imageModeStopPreservesPartialResults();
    void imagePanelAnimatesGifAndHighlightsHover();
};

QByteArray makePngBytes() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(QColor(0xCC, 0x44, 0x22));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

void MainWindowIntegrationTests::initTestCase() {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
}

void MainWindowIntegrationTests::lifecycleCardLogsAndResetsPerScan() {
    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    QVERIFY(window.m_scanControlsPanel != nullptr);
    QVERIFY(window.m_scanControlsPanel->lifecycleCard() != nullptr);
    QVERIFY(window.m_scanControlsPanel->lifecycleLogListWidget() != nullptr);
    QVERIFY(!window.m_scanControlsPanel->lifecycleCard()->isVisible());

    window.onScanStarted(3, 1024);
    QCoreApplication::processEvents();

    QVERIFY(window.m_scanControlsPanel->lifecycleCard()->isVisible());
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 1);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(0)->text(),
             QStringLiteral("Scanning..."));

    window.onResultsBatchReady({}, 5);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 2);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(1)->text(),
             QStringLiteral("Merged results: 5"));

    window.onScanFinished(false, false);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 3);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(2)->text(),
             QStringLiteral("Scan finished"));

    QVERIFY(window.statusBar() != nullptr);
    QVERIFY(window.statusBar()->currentMessage().startsWith(QStringLiteral("Current buffer:")));

    window.m_scanControlsPanel->hideLifecycleCardButton()->click();
    QCoreApplication::processEvents();
    QVERIFY(!window.m_scanControlsPanel->lifecycleCard()->isVisible());

    window.onScanStarted(1, 64);
    QCoreApplication::processEvents();
    QVERIFY(window.m_scanControlsPanel->lifecycleCard()->isVisible());
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->count(), 1);
    QCOMPARE(window.m_scanControlsPanel->lifecycleLogListWidget()->item(0)->text(),
             QStringLiteral("Scanning..."));
}

void MainWindowIntegrationTests::selectingResultRowUpdatesPreviewBuffers() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("preview.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("ABCDEFGHIJKLMNO");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 4;
    match.searchTimeNs = 1000;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    QTableView* table = window.m_resultsPanel->resultsTableView();
    QVERIFY(table != nullptr);
    table->selectRow(0);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_activePreviewRow, 0);
    QVERIFY(!window.m_textHoverBuffer.data.isEmpty());
    QVERIFY(!window.m_bitmapHoverBuffer.data.isEmpty());
    QVERIFY(window.m_sharedCenterOffset >= match.offset);
}

void MainWindowIntegrationTests::twoColumnCompositionAndDataViewToolbar() {
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/dataViewImageMaxPixelsK"));
    settings.remove(QStringLiteral("ui/dataViewImageMaxResults"));
    settings.remove(QStringLiteral("ui/dataViewImageJobs"));

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    auto* mainSplitter = window.findChild<QSplitter*>(QStringLiteral("mainSplitter"));
    QVERIFY(mainSplitter != nullptr);
    QCOMPARE(mainSplitter->count(), 2);
    QVERIFY(window.m_hexControlsPanel != nullptr);
    QVERIFY(window.m_textView != nullptr);
    QVERIFY(window.m_dataViewShellPanel != nullptr);
    QVERIFY(window.m_dataViewByteAndBitmapPanel != nullptr);
    QVERIFY(window.m_dataViewStructuredPanel != nullptr);
    QVERIFY(window.m_dataViewImagePanel != nullptr);
    QVERIFY(window.m_structModeLeftPanel != nullptr);
    QCOMPARE(window.m_dataViewShellPanel->bodyStackedWidget()->count(), 3);
    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(0);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_dataViewShellPanel->bodyStackedWidget()->currentWidget(),
             static_cast<QWidget*>(window.m_dataViewByteAndBitmapPanel));

    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_dataViewShellPanel->bodyStackedWidget()->currentWidget(),
             static_cast<QWidget*>(window.m_dataViewStructuredPanel));
    QVERIFY(window.m_dataViewShellPanel->bitmapModeComboBox()->isHidden());
    QVERIFY(window.m_dataViewShellPanel->zoomInButton()->isHidden());
    QVERIFY(!window.m_dataViewShellPanel->littleEndianRadioButton()->isHidden());

    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(2);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_dataViewShellPanel->bodyStackedWidget()->currentWidget(),
             static_cast<QWidget*>(window.m_dataViewImagePanel));
    QVERIFY(window.m_dataViewShellPanel->bitmapModeComboBox()->isHidden());
    QVERIFY(window.m_dataViewShellPanel->zoomInButton()->isHidden());
    QVERIFY(window.m_dataViewShellPanel->littleEndianRadioButton()->isHidden());
    QCOMPARE(window.m_dataViewImagePanel->maxPixelsKSpinBox()->value(), 4096);
    QCOMPARE(window.m_dataViewImagePanel->maxResultsSpinBox()->value(), 5);
    QCOMPARE(window.m_dataViewImagePanel->jobsSpinBox()->value(),
             qMin(256, qMax(1, QThread::idealThreadCount())));
    QVERIFY(!window.m_dataViewImagePanel->resultsProgressBar()->isHidden());

    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(0);
    QCoreApplication::processEvents();
    QVERIFY(window.m_dataViewImagePanel->resultsProgressBar()->isHidden());
    window.m_dataViewImagePanel->jobsSpinBox()->setValue(1);
    QCoreApplication::processEvents();
    breco::MainWindow restoredWindow;
    restoredWindow.show();
    QCoreApplication::processEvents();
    QCOMPARE(restoredWindow.m_dataViewImagePanel->jobsSpinBox()->value(), 1);

    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(0);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_dataViewShellPanel->bodyStackedWidget()->currentWidget(),
             static_cast<QWidget*>(window.m_dataViewByteAndBitmapPanel));
    QVERIFY(!window.m_dataViewShellPanel->bitmapModeComboBox()->isHidden());
    QVERIFY(!window.m_dataViewShellPanel->zoomInButton()->isHidden());
}

void MainWindowIntegrationTests::navigatorLabelsAndDataViewEndianFollowSelection() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("labels.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("HELLO WORLD");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;
    window.m_shiftValueSpin->setValue(0);

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 1;
    match.searchTimeNs = 1;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    window.showMatchPreview(0, match);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_hexControlsPanel->fileNameValueLabel()->text(), QStringLiteral("labels.bin"));
    QCOMPARE(window.m_hexControlsPanel->offsetValueLabel()->text(), QStringLiteral("0X0"));
    QVERIFY(window.m_hexControlsPanel->fileSizeValueLabel()->text().contains(QStringLiteral("B")));

    window.m_activeTextSelectionRange = qMakePair<quint64, quint64>(1, 4);
    window.updateHexInfoPanel();
    window.refreshDataViewFromNavigator();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_hexControlsPanel->selectedValueLabel()->text(),
             QStringLiteral("0X1 (+3 bytes)"));
    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("E"));

    window.m_dataViewShellPanel->bigEndianRadioButton()->setChecked(true);
    QCoreApplication::processEvents();
    QVERIFY(window.m_currentByteInfoPanel->bigEndianCheckBox()->isChecked());
}

void MainWindowIntegrationTests::currentBytePanelShowsEndianAndWidthAwareValues() {
    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::MainWindow::HoverBuffer hover;
    hover.filePath = QStringLiteral("in-memory");
    hover.baseOffset = 100;
    hover.data = QByteArray::fromHex("4100FF");

    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(true);
    window.m_currentByteInfoPanel->decimalModeRadioButton()->setChecked(true);
    QCoreApplication::processEvents();
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("A"));
    QCOMPARE(window.m_currentByteInfoPanel->u8ValueLabel()->text(), QStringLiteral("65"));
    QCOMPARE(window.m_currentByteInfoPanel->u16ValueLabel()->text(), QStringLiteral("16640"));
    QCOMPARE(window.m_currentByteInfoPanel->u32ValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->u64ValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("A"));
    QCOMPARE(window.m_currentByteInfoPanel->hexStr8BytesValueLabel()->text(),
             QStringLiteral("0 x 41 00 FF -- -- -- -- --"));

    window.m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(false);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("-"));

    window.updateCurrentByteInfoFromHover(hover, 101);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("."));
    QCOMPARE(window.m_currentByteInfoPanel->u16ValueLabel()->text(), QStringLiteral("65280"));
}

void MainWindowIntegrationTests::shiftMarksCurrentBufferDirtyAndRestoresOnDeselect() {
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/hexShiftBitsValue"));
    settings.setValue(QStringLiteral("ui/hexShiftBitsValue"), 42);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("dirty-buffer.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes = QByteArray::fromHex("112233445566");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_shiftValueSpin->value(), 0);

    breco::ScanTarget target;
    target.filePath = filePath;
    target.fileSize = static_cast<quint64>(bytes.size());
    window.m_scanTargets = {target};
    window.m_sourceMode = breco::MainWindow::SourceMode::SingleFile;

    breco::ResultBuffer buffer;
    buffer.scanTargetIdx = 0;
    buffer.fileOffset = 0;
    buffer.bytes = bytes;
    buffer.dirty = false;
    window.m_resultBuffers = {buffer};
    window.m_matchBufferIndices = {0};

    breco::MatchRecord match;
    match.scanTargetIdx = 0;
    match.threadId = 1;
    match.offset = 2;
    match.searchTimeNs = 1;
    window.m_resultModel.clear();
    window.m_resultModel.appendBatch({match});
    window.rebuildTargetMatchIntervals();

    QVERIFY(window.m_shiftValueSpin != nullptr);
    window.m_shiftValueSpin->setValue(1);
    QCoreApplication::processEvents();

    window.showMatchPreview(0, match);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_activePreviewRow, 0);
    QVERIFY(window.m_resultBuffers.at(0).dirty);
    QVERIFY(window.m_resultBuffers.at(0).bytes != bytes);

    window.onResultActivated(QModelIndex());
    QCoreApplication::processEvents();
    QCOMPARE(window.m_activePreviewRow, -1);
    QVERIFY(!window.m_resultBuffers.at(0).dirty);
    QCOMPARE(window.m_resultBuffers.at(0).bytes, bytes);
    settings.remove(QStringLiteral("ui/hexShiftBitsValue"));
}

void MainWindowIntegrationTests::sourcePathInputValidatesAndOpensTargets() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("source.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("SOURCE");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();

    const QString childPath = tempDir.filePath(QStringLiteral("child.bin"));
    QFile child(childPath);
    QVERIFY(child.open(QIODevice::WriteOnly));
    QCOMPARE(child.write(QByteArray("DIR")), 3);
    child.close();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    auto noElevation = std::make_unique<FakeProtectedSourceOpener>();
    noElevation->available = false;
    window.setProtectedSourceOpenerForTests(std::move(noElevation));

    QLineEdit* sourcePathEdit = window.m_scanControlsPanel->sourcePathLineEdit();
    QLabel* sourceTypeIcon = window.m_scanControlsPanel->selectedSourceTypeIconLabel();
    QVERIFY(sourcePathEdit != nullptr);
    QVERIFY(sourceTypeIcon != nullptr);
    QSize expectedIconSize;
    for (const QString& iconPath :
         {QStringLiteral(":/res/none.png"), QStringLiteral(":/res/file.png"),
          QStringLiteral(":/res/dev.png"), QStringLiteral(":/res/dir.png")}) {
        expectedIconSize = expectedIconSize.expandedTo(QPixmap(iconPath).size());
    }
    QCOMPARE(sourceTypeIcon->minimumSize(), expectedIconSize);
    QCOMPARE(sourceTypeIcon->maximumSize(), expectedIconSize);
    QCOMPARE(window.m_scanControlsPanel->openFileButton()->toolTip(), QStringLiteral("Select file"));
    QCOMPARE(window.m_scanControlsPanel->openDirButton()->toolTip(), QStringLiteral("Select directory"));

    const QString missingPath = tempDir.filePath(QStringLiteral("missing.bin"));
    sourcePathEdit->setText(missingPath);
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Not found: %1").arg(missingPath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#fff3a3")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("No source selected"));

    sourcePathEdit->setText(filePath);
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    const QString absoluteFilePath = QFileInfo(filePath).absoluteFilePath();
    QCOMPARE(sourcePathEdit->text(), filePath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

    QVERIFY(window.applySourcePath(filePath, true));
    QCoreApplication::processEvents();
    QCOMPARE(sourcePathEdit->text(), absoluteFilePath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::SingleFile);
    QCOMPARE(window.m_scanTargets.size(), 1);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absoluteFilePath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#c8f7c5")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

    QFile denied(filePath);
    QVERIFY(QFile::setPermissions(filePath, QFileDevice::Permissions()));
    const bool deniedCanOpen = denied.open(QIODevice::ReadOnly);
    if (deniedCanOpen) {
        denied.close();
    } else {
        sourcePathEdit->setText(filePath);
        window.validateSourcePathInput();
        QCoreApplication::processEvents();
        QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
        QCOMPARE(window.m_scanTargets.size(), 0);
        QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
        QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));

        QVERIFY(!window.applySourcePath(filePath, true));
        QCoreApplication::processEvents();
        QCOMPARE(window.statusBar()->currentMessage(),
                 QStringLiteral("Permission denied: %1").arg(absoluteFilePath));
        QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#ffc9c9")));
        QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("File"));
    }
    QVERIFY(QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));

    sourcePathEdit->setText(tempDir.path());
    window.validateSourcePathInput();
    QCoreApplication::processEvents();
    const QString absoluteDirPath = QFileInfo(tempDir.path()).absoluteFilePath();
    QCOMPARE(sourcePathEdit->text(), tempDir.path());
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
    QCOMPARE(window.m_scanTargets.size(), 0);
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("background-color: white")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("Directory"));

    QVERIFY(window.applySourcePath(tempDir.path(), true));
    QCoreApplication::processEvents();
    QCOMPARE(sourcePathEdit->text(), absoluteDirPath);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::Directory);
    QVERIFY(window.m_scanTargets.size() >= 1);
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absoluteDirPath));
    QVERIFY(sourcePathEdit->styleSheet().contains(QStringLiteral("#c8f7c5")));
    QCOMPARE(sourceTypeIcon->toolTip(), QStringLiteral("Directory"));
}

void MainWindowIntegrationTests::sourcePathAutocompleteKeepsTypingFocusAndLimitsSuggestions() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    for (const QString& name :
         {QStringLiteral("match"), QStringLiteral("matcher"), QStringLiteral("matchbox"),
          QStringLiteral("matching"), QStringLiteral("matchstick"), QStringLiteral("matchwork")}) {
        QFile file(tempDir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.close();
    }
    const QString matchingDirectory = tempDir.filePath(QStringLiteral("matchdir"));
    QVERIFY(QDir().mkdir(matchingDirectory));

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    QLineEdit* sourcePathEdit = window.m_scanControlsPanel->sourcePathLineEdit();
    QVERIFY(sourcePathEdit != nullptr);
    QCompleter* completer = sourcePathEdit->completer();
    QVERIFY(completer != nullptr);
    QCOMPARE(completer->maxVisibleItems(), 5);
    QCOMPARE(completer->popup()->focusPolicy(), Qt::NoFocus);

    sourcePathEdit->setFocus();
    QTRY_VERIFY(sourcePathEdit->hasFocus());
    const QString prefix = QDir(tempDir.path()).filePath(QStringLiteral("mat"));
    QTest::keyClicks(sourcePathEdit, prefix);
    QTRY_COMPARE(completer->completionModel()->rowCount(), 5);
    QTRY_VERIFY(completer->popup()->isVisible());
    QVERIFY(sourcePathEdit->hasFocus());

    QTest::keyClick(sourcePathEdit, Qt::Key_C);
    QTRY_COMPARE(sourcePathEdit->text(),
                 QDir(tempDir.path()).filePath(QStringLiteral("matc")));
    QTRY_COMPARE(completer->completionModel()->rowCount(), 5);
    QVERIFY(sourcePathEdit->hasFocus());
    QCOMPARE(completer->completionModel()->index(0, 0).data().toString(),
             tempDir.filePath(QStringLiteral("match")));
    bool includesDirectory = false;
    for (int row = 0; row < completer->completionModel()->rowCount(); ++row) {
        includesDirectory |=
            completer->completionModel()->index(row, 0).data().toString() ==
            QDir::toNativeSeparators(matchingDirectory + QStringLiteral("/"));
    }
    QVERIFY(includesDirectory);

    QTest::keyClick(sourcePathEdit, Qt::Key_Down);
    QCoreApplication::processEvents();
    const QModelIndex afterDown = completer->popup()->currentIndex();
    QVERIFY(afterDown.isValid());
    QTest::keyClick(sourcePathEdit, Qt::Key_Up);
    QCoreApplication::processEvents();
    const QModelIndex selectedIndex = completer->popup()->currentIndex();
    QVERIFY(selectedIndex.isValid());
    QVERIFY(selectedIndex.row() <= afterDown.row());
    const QString selectedPath = selectedIndex.data().toString();
    QTest::keyClick(sourcePathEdit, Qt::Key_Enter);
    QTRY_COMPARE(sourcePathEdit->text(), selectedPath);
    QVERIFY(sourcePathEdit->hasFocus());
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
}

#ifdef Q_OS_UNIX
void MainWindowIntegrationTests::protectedSourceOpenUsesConfirmationAndFd() {
    SettingsValueGuard sourcePathGuard(
        QStringLiteral("ui/rememberedSingleFilePath"));
    SettingsValueGuard sourceOffsetGuard(
        QStringLiteral("ui/rememberedSingleFileOffset"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("protected.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray bytes("PROTECTED");
    QCOMPARE(f.write(bytes), bytes.size());
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    auto unavailable = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* unavailablePtr = unavailable.get();
    unavailablePtr->available = false;
    window.setProtectedSourceOpenerForTests(std::move(unavailable));
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(unavailablePtr->openCount, 0);

    auto declined = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* declinedPtr = declined.get();
    declinedPtr->available = true;
    window.setProtectedSourceOpenerForTests(std::move(declined));
    window.m_protectedSourceDialogAnswerForTests = QMessageBox::Cancel;
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(declinedPtr->openCount, 0);

    auto failed = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* failedPtr = failed.get();
    failedPtr->available = true;
    failedPtr->result = breco::ProtectedOpenResult::failed(QStringLiteral("nope"));
    window.setProtectedSourceOpenerForTests(std::move(failed));
    window.m_protectedSourceDialogAnswerForTests = QMessageBox::Yes;
    QVERIFY(!window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(failedPtr->openCount, 1);

    const int fd = ::open(absolutePath.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC);
    QVERIFY(fd >= 0);
    auto opened = std::make_unique<FakeProtectedSourceOpener>();
    FakeProtectedSourceOpener* openedPtr = opened.get();
    openedPtr->available = true;
    openedPtr->result =
        breco::ProtectedOpenResult::opened(fd, static_cast<quint64>(bytes.size()));
    window.setProtectedSourceOpenerForTests(std::move(opened));
    window.m_protectedSourceDialogAnswerForTests = QMessageBox::Yes;
    QVERIFY(window.tryOpenProtectedSource(absolutePath, breco::MainWindow::SourceTargetKind::File));
    QCOMPARE(openedPtr->openCount, 1);
    QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::SingleFile);
    QCOMPARE(window.m_scanTargets.size(), 1);
    QCOMPARE(window.m_scanTargets.first().filePath, absolutePath);
    QCOMPARE(window.m_scanTargets.first().fileSize, static_cast<quint64>(bytes.size()));
    QCOMPARE(window.statusBar()->currentMessage(), QStringLiteral("Open: %1").arg(absolutePath));

    const auto chunk = window.m_filePool.readChunk(absolutePath, 0, static_cast<quint64>(bytes.size()));
    QVERIFY(chunk.has_value());
    QCOMPARE(chunk.value(), bytes);

    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    QVERIFY(!settings.contains(QStringLiteral("ui/rememberedSingleFilePath")));
    QVERIFY(!settings.contains(QStringLiteral("ui/rememberedSingleFileOffset")));
}
#endif

void MainWindowIntegrationTests::structModePanelPreviewViewsAndSnippets() {
    SettingsValueGuard definitionPathGuard(
        QStringLiteral("ui/lastStructDefinitionFilePath"));
    SettingsValueGuard declarationTextGuard(
        QStringLiteral("ui/structDeclarationText"));
    SettingsValueGuard previewEnabledGuard(
        QStringLiteral("ui/structPreviewEnabled"));
    SettingsValueGuard viewsVisibleGuard(
        QStringLiteral("ui/structViewsVisible"));
    SettingsValueGuard languageVisibleGuard(
        QStringLiteral("ui/structLanguageVisible"));
    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.remove(QStringLiteral("ui/lastStructDefinitionFilePath"));
    settings.remove(QStringLiteral("ui/structDeclarationText"));
    settings.remove(QStringLiteral("ui/structPreviewEnabled"));
    settings.remove(QStringLiteral("ui/structViewsVisible"));
    settings.remove(QStringLiteral("ui/structLanguageVisible"));
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("struct-view.bin"));
    QFile source(filePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    const QByteArray sourceBytes = QByteArray::fromHex("01020304");
    QCOMPARE(source.write(sourceBytes), sourceBytes.size());
    source.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.selectSourcePath(absolutePath));
    QCoreApplication::processEvents();
    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(1);
    QCoreApplication::processEvents();

    auto* panel = window.m_structModeLeftPanel;
    QVERIFY(panel != nullptr);
    auto* editorToggle =
        panel->findChild<QCheckBox*>(QStringLiteral("editorCheckBox"));
    auto* viewsToggle =
        panel->findChild<QCheckBox*>(QStringLiteral("viewsCheckBox"));
    auto* languageToggle =
        panel->findChild<QCheckBox*>(QStringLiteral("languageCheckBox"));
    auto* previewToggle = panel->previewEnabledCheckBox();
    auto* statusLabel = panel->findChild<QLabel*>(
        QStringLiteral("structDeclarationStatusLabel"));
    auto* editorWidget =
        panel->findChild<QWidget*>(QStringLiteral("structEditorWidgetPlaceholder"));
    auto* viewsWidget =
        panel->findChild<QWidget*>(QStringLiteral("structViewEntriesWidget"));
    auto* languageWidget =
        panel->findChild<QWidget*>(QStringLiteral("languageReferenceWidget"));
    QVERIFY(editorToggle != nullptr);
    QVERIFY(viewsToggle != nullptr);
    QVERIFY(languageToggle != nullptr);
    QVERIFY(previewToggle != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(panel->structDeclarationLayout()->indexOf(
                panel->structDeclarationEdit()) <
            panel->structDeclarationLayout()->indexOf(statusLabel));
    QVERIFY(!statusLabel->isHidden());
    QCOMPARE(statusLabel->text(), QStringLiteral("0 lines, no errors"));
    QVERIFY(editorToggle->isChecked());
    QVERIFY(!viewsToggle->isChecked());
    QVERIFY(!languageToggle->isChecked());
    QVERIFY(previewToggle->isChecked());
    QVERIFY(!previewToggle->isEnabled());
    QVERIFY(!editorWidget->isHidden());
    QVERIFY(viewsWidget->isHidden());
    QVERIFY(languageWidget->isHidden());
    viewsToggle->setChecked(true);
    languageToggle->setChecked(true);
    QCoreApplication::processEvents();
    QVERIFY(!viewsWidget->isHidden());
    QVERIFY(!languageWidget->isHidden());
    QCOMPARE(settings.value(QStringLiteral("ui/structViewsVisible")).toBool(),
             true);
    QCOMPARE(settings.value(QStringLiteral("ui/structLanguageVisible")).toBool(),
             true);
    breco::StructModeLeftPanel restoredSectionPanel;
    QVERIFY(restoredSectionPanel
                .findChild<QCheckBox*>(QStringLiteral("viewsCheckBox"))
                ->isChecked());
    QVERIFY(restoredSectionPanel
                .findChild<QCheckBox*>(QStringLiteral("languageCheckBox"))
                ->isChecked());

    breco::ScanTarget target;
    target.filePath = absolutePath;
    target.fileSize = static_cast<quint64>(sourceBytes.size());
    window.m_scanTargets = {target};
    window.m_textHoverBuffer.filePath = absolutePath;
    window.m_textHoverBuffer.baseOffset = 0;
    window.m_textHoverBuffer.data = sourceBytes.left(2);
    window.m_textView->setData(sourceBytes.left(2), 0, std::nullopt,
                               static_cast<quint64>(sourceBytes.size()));
    window.m_textView->setSelectedOffset(1, true);

    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("/default Second\n"
                       "struct First { uint8 value; }\n"
                       "struct Second { uint8 value; }"));
    QCoreApplication::processEvents();
    QVERIFY(panel->isParseValid());
    QCOMPARE(panel->entryComboBox()->currentText(), QStringLiteral("Second"));

    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("struct S {\n  uint8 value;\n}"));
    QCoreApplication::processEvents();
    QVERIFY(panel->isParseValid());
    QVERIFY(panel->canPreview());
    QCOMPARE(statusLabel->text(), QStringLiteral("3 lines, no errors"));
    QVERIFY(previewToggle->isEnabled());
    QVERIFY(previewToggle->isChecked());
    QCOMPARE(previewToggle->text(), QStringLiteral("Enable"));
    QVERIFY(panel->previewActive());
    QCOMPARE(panel->addViewButton()->text(), QStringLiteral("Add previewed"));
    QVERIFY(panel->addViewButton()->isEnabled());
    QTreeView* tree = window.m_structDataViewPanel->structDataTreeView();
    QVERIFY(tree != nullptr);
    QCOMPARE(tree->model()->rowCount(), 1);
    const QModelIndex previewView = tree->model()->index(0, 0);
    QCOMPARE(tree->model()->data(previewView).toString(),
             QStringLiteral("Preview"));
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Type))
                 .toString(),
             QStringLiteral("S"));
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Value))
                 .toString(),
             QString());
    QVERIFY(window.m_structPreview.has_value());
    QCOMPARE(window.m_structPreview->offset, 1ULL);
    QCOMPARE(window.m_structPreview->decodedRoot.children.first()
                 .children.first()
                 .rawBytes.toHex()
                 .toUpper(),
             QByteArray("02"));
    const QModelIndex previewMember =
        tree->model()->index(0, 0, previewView);
    QCOMPARE(tree->model()->data(previewMember).toString(),
             QStringLiteral("value"));
    tree->clicked(previewView);
    QCoreApplication::processEvents();
    QCOMPARE(panel->structDeclarationEdit()->textCursor().block().text(),
             QStringLiteral("struct S {"));
    tree->clicked(previewMember);
    QCoreApplication::processEvents();
    QCOMPARE(panel->structDeclarationEdit()->textCursor().block().text().trimmed(),
             QStringLiteral("uint8 value;"));
    const QList<QTextEdit::ExtraSelection> declarationSelections =
        panel->structDeclarationEdit()->extraSelections();
    QVERIFY(!declarationSelections.isEmpty());
    QCOMPARE(declarationSelections.first().cursor.block().text().trimmed(),
             QStringLiteral("uint8 value;"));
    QCOMPARE(
        tree->model()
            ->data(previewMember,
                   breco::StructVisualizedTreeModel::SourceOffsetRole)
            .toULongLong(),
        1ULL);
    QCOMPARE(
        tree->model()
            ->data(previewMember,
                   breco::StructVisualizedTreeModel::SourceFilePathRole)
            .toString(),
        absolutePath);
    QCOMPARE(
        tree->model()
            ->data(previewMember,
                   breco::StructVisualizedTreeModel::SourceLengthRole)
            .toULongLong(),
        1ULL);
    window.m_sharedCenterOffset = 0;
    const QRect previewMemberRect = tree->visualRect(previewMember);
    QVERIFY(previewMemberRect.isValid());
    tree->clicked(previewMember);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_sharedCenterOffset, 1ULL);
    QVERIFY(window.m_structSourceHighlightRange.has_value());
    QCOMPARE(window.m_structSourceHighlightRange->first, 1ULL);
    QCOMPARE(window.m_structSourceHighlightRange->second, 2ULL);
    QVERIFY(panel->previewActive());

    panel->addViewButton()->click();
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    QCOMPARE(panel->currentViewsTableWidget()->rowCount(), 1);
    QCOMPARE(tree->model()->rowCount(), 2);
    previewToggle->setChecked(false);
    QCoreApplication::processEvents();
    QVERIFY(!panel->previewActive());
    QVERIFY(!panel->addViewButton()->isEnabled());
    QCOMPARE(settings.value(QStringLiteral("ui/structPreviewEnabled")).toBool(),
             false);
    QCOMPARE(tree->model()->rowCount(), 1);
    const QModelIndex savedView = tree->model()->index(0, 0);
    QCOMPARE(tree->model()->data(savedView).toString(),
             QStringLiteral("S@0x1"));
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Type))
                 .toString(),
             QStringLiteral("S"));
    QCOMPARE(tree->model()->rowCount(savedView), 1);
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(0, 0, savedView))
                 .toString(),
             QStringLiteral("value"));

    QTableWidget* views = panel->currentViewsTableWidget();
    QCOMPARE(views->item(0, 0)->text(), QStringLiteral("S@0x1"));
    QVERIFY((views->item(0, 1)->flags() & Qt::ItemIsEditable) == 0);
    QCOMPARE(window.m_currentStructViews.first().offset, 1ULL);
    window.m_sharedCenterOffset = 0;
    const QRect viewNameRect = views->visualItemRect(views->item(0, 0));
    QVERIFY(viewNameRect.isValid());
    views->cellClicked(0, 0);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_sharedCenterOffset, 1ULL);
    QVERIFY(window.m_structSourceHighlightRange.has_value());
    QCOMPARE(window.m_structSourceHighlightRange->first, 1ULL);
    QCOMPARE(window.m_structSourceHighlightRange->second, 2ULL);
    views->item(0, 0)->setText(QStringLiteral("Saved header"));
    QCoreApplication::processEvents();
    QCOMPARE(tree->model()->data(tree->model()->index(0, 0)).toString(),
             QStringLiteral("Saved header"));

    auto* repeatSpin = qobject_cast<QSpinBox*>(views->cellWidget(0, 2));
    QVERIFY(repeatSpin != nullptr);
    repeatSpin->setValue(3);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentStructViews.first().repeat, 3);
    QCOMPARE(window.m_currentStructViews.first().decodedRoot.children.size(), 3);
    const QModelIndex repeatedView = tree->model()->index(0, 0);
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Type))
                 .toString(),
             QString());
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Value))
                 .toString(),
             QStringLiteral("3 items"));
    const QModelIndex repeatedItem =
        tree->model()->index(0, 0, repeatedView);
    QCOMPARE(tree->model()->data(repeatedItem).toString(),
             QStringLiteral("S[0]"));
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Type, repeatedView))
                 .toString(),
             QStringLiteral("S"));
    views->cellClicked(0, 0);
    QCoreApplication::processEvents();
    QVERIFY(window.m_structSourceHighlightRange.has_value());
    QCOMPARE(window.m_structSourceHighlightRange->first, 1ULL);
    QCOMPARE(window.m_structSourceHighlightRange->second, 4ULL);
    QCOMPARE(window.m_currentStructViews.first()
                 .decodedRoot.children.last()
                 .children.first()
                 .rawBytes.toHex()
                 .toUpper(),
             QByteArray("04"));
    repeatSpin->setValue(1);
    QCoreApplication::processEvents();
    QCOMPARE(tree->model()
                 ->data(tree->model()->index(
                     0, breco::StructVisualizedTreeModel::Type))
                 .toString(),
             QStringLiteral("S"));

    views->item(0, 3)->setText(QStringLiteral("0x3"));
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentStructViews.first().offset, 3ULL);
    QCOMPARE(window.m_currentStructViews.first()
                 .decodedRoot.children.first()
                 .children.first()
                 .rawBytes.toHex()
                 .toUpper(),
             QByteArray("04"));

    repeatSpin->setValue(2);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentStructViews.first().repeat, 2);

    views->selectRow(0);
    QCoreApplication::processEvents();
    QVERIFY(panel->removeViewButton()->isEnabled());
    panel->removeViewButton()->click();
    QCoreApplication::processEvents();
    QCOMPARE(views->rowCount(), 0);
    QVERIFY(!panel->removeViewButton()->isEnabled());
    QCOMPARE(tree->model()->rowCount(), 0);

    window.m_textView->setSelectedOffset(1, true);
    previewToggle->setChecked(true);
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    window.m_textView->setSelectedOffset(0, true);
    window.onTextByteClicked(0);
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    QVERIFY(window.m_structPreview.has_value());
    QCOMPARE(window.m_structPreview->offset, 0ULL);
    QCOMPARE(window.m_structPreview->decodedRoot.children.first()
                 .children.first()
                 .rawBytes.toHex()
                 .toUpper(),
             QByteArray("01"));

    window.m_textView->setData(sourceBytes.left(2), 0, std::nullopt,
                               static_cast<quint64>(sourceBytes.size()));
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    QVERIFY(window.m_structPreview.has_value());
    QCOMPARE(window.m_structPreview->offset, 0ULL);

    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("struct S {\n  uint8 value;\n} // changed"));
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("struct S {\n"
                       "  uint8 value;\n"
                       "  nope"));
    QCoreApplication::processEvents();
    QVERIFY(!panel->previewActive());
    QVERIFY(!statusLabel->isHidden());
    QVERIFY(statusLabel->text().startsWith(QStringLiteral("Line 3: ")));
    QVERIFY(previewToggle->isChecked());
    QVERIFY(!previewToggle->isEnabled());
    QVERIFY(!panel->addViewButton()->isEnabled());
    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("struct S {\n  uint8 value;\n}"));
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    QVERIFY(previewToggle->isChecked());
    QVERIFY(previewToggle->isEnabled());
    QVERIFY(panel->addViewButton()->isEnabled());
    window.clearStructPreview();
    QCoreApplication::processEvents();
    QVERIFY(!panel->previewActive());
    QVERIFY(previewToggle->isChecked());
    QVERIFY(panel->addViewButton()->isEnabled());
    window.onTextByteClicked(1);
    QCoreApplication::processEvents();
    QVERIFY(panel->previewActive());
    QVERIFY(window.m_structPreview.has_value());
    QCOMPARE(window.m_structPreview->offset, 1ULL);
    window.clearResultBufferCacheState();
    QCoreApplication::processEvents();
    QVERIFY(!panel->previewActive());
    QVERIFY(previewToggle->isChecked());

    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("struct S {\n  uint8 value;\n}"));
    const QString savedDeclaration =
        tempDir.filePath(QStringLiteral("definition.struct"));
    QVERIFY(panel->saveDeclarationToFile(savedDeclaration));
    panel->structDeclarationEdit()->clear();
    QVERIFY(panel->loadDeclarationFromFile(savedDeclaration));
    QCOMPARE(panel->declarationText(),
             QStringLiteral("struct S {\n  uint8 value;\n}"));
    QCOMPARE(settings.value(QStringLiteral("ui/lastStructDefinitionFilePath"))
                 .toString(),
             QFileInfo(savedDeclaration).absoluteFilePath());

    panel->structDeclarationEdit()->setPlainText(
        QStringLiteral("uint8 first;\nuint8 second;"));
    QTextCursor cursor(panel->structDeclarationEdit()->document());
    cursor.setPosition(2);
    panel->structDeclarationEdit()->setTextCursor(cursor);
    panel->insertLanguageSnippet(QStringLiteral("int8 inserted;"));
    QCOMPARE(panel->declarationText(),
             QStringLiteral("uint8 first;\nint8 inserted;\nuint8 second;"));
    const QList<QLabel*> languageLabels = languageWidget->findChildren<QLabel*>();
    for (QLabel* label : languageLabels) {
        const QString snippet =
            breco::StructModeLeftPanel::languageSnippetForObjectName(
                label->objectName());
        QVERIFY2(!snippet.isEmpty(), qPrintable(label->objectName()));
        panel->structDeclarationEdit()->setPlainText(snippet);
        QVERIFY2(panel->isParseValid(), qPrintable(label->objectName()));
    }
}

void MainWindowIntegrationTests::restoresStructDefinitionAndPreviewOnStartup() {
    SettingsValueGuard definitionPathGuard(
        QStringLiteral("ui/lastStructDefinitionFilePath"));
    SettingsValueGuard sourcePathGuard(
        QStringLiteral("ui/rememberedSingleFilePath"));
    SettingsValueGuard declarationTextGuard(
        QStringLiteral("ui/structDeclarationText"));
    SettingsValueGuard entryNameGuard(QStringLiteral("ui/structEntryName"));
    SettingsValueGuard entryCountGuard(QStringLiteral("ui/structEntryCount"));
    SettingsValueGuard dataViewModeGuard(QStringLiteral("ui/dataViewModeIndex"));
    SettingsValueGuard previewEnabledGuard(
        QStringLiteral("ui/structPreviewEnabled"));

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString definitionPath =
        tempDir.filePath(QStringLiteral("startup.struct"));
    QFile definition(definitionPath);
    QVERIFY(definition.open(QIODevice::WriteOnly));
    const QByteArray declaration(
        "/default Restored\n"
        "struct Remembered { uint8 value; }\n"
        "struct Restored { /cond(=0x42) uint8 magic; uint8 tail; }");
    QCOMPARE(definition.write(declaration), declaration.size());
    definition.close();

    const QString sourcePath = tempDir.filePath(QStringLiteral("startup.bin"));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    const QByteArray sourceBytes = QByteArray::fromHex("0099427F");
    QCOMPARE(source.write(sourceBytes), sourceBytes.size());
    source.close();

    QSettings settings(QStringLiteral("breco"), QStringLiteral("breco"));
    settings.setValue(QStringLiteral("ui/lastStructDefinitionFilePath"),
                      definitionPath);
    settings.setValue(QStringLiteral("ui/rememberedSingleFilePath"),
                      sourcePath);
    settings.setValue(QStringLiteral("ui/rememberedSingleFileOffset"), 2);
    settings.setValue(QStringLiteral("ui/structDeclarationText"),
                      QStringLiteral("uint8 stale;"));
    settings.setValue(QStringLiteral("ui/structEntryName"),
                      QStringLiteral("Remembered"));
    settings.setValue(QStringLiteral("ui/structEntryCount"), 1);
    settings.setValue(QStringLiteral("ui/dataViewModeIndex"), 1);
    settings.setValue(QStringLiteral("ui/structPreviewEnabled"), true);

    {
        breco::MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        QCOMPARE(window.m_structModeLeftPanel->declarationText(),
                 QString::fromUtf8(declaration));
        QVERIFY(window.m_structModeLeftPanel->isParseValid());
        QVERIFY(window.m_structModeLeftPanel->previewEnabledCheckBox()
                    ->isChecked());
        QCOMPARE(window.m_structModeLeftPanel->entryComboBox()->currentText(),
                 QStringLiteral("Restored"));
        QVERIFY(window.m_structModeLeftPanel->canPreview());
        QCOMPARE(window.m_textHoverBuffer.filePath,
                 QFileInfo(sourcePath).absoluteFilePath());
        QCOMPARE(window.m_textHoverBuffer.data, sourceBytes);
        QCOMPARE(window.m_sharedCenterOffset, 2ULL);
        QVERIFY(window.m_structModeLeftPanel->previewActive());
        QVERIFY(window.m_structPreview.has_value());
        QCOMPARE(window.m_structPreview->offset, 2ULL);
        QCOMPARE(window.m_structPreview->filePath,
                 QFileInfo(sourcePath).absoluteFilePath());
        QCOMPARE(window.m_structPreview->decodedRoot.children.first()
                     .children.first()
                     .rawBytes.toHex()
                     .toUpper(),
                 QByteArray("42"));

        QTreeView* tree =
            window.m_structDataViewPanel->structDataTreeView();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->model()->columnCount(), 5);
        QCOMPARE(
            tree->header()->sectionResizeMode(
                breco::StructVisualizedTreeModel::Name),
            QHeaderView::ResizeToContents);
        const QModelIndex restored = tree->model()->index(0, 0);
        const QModelIndex conditionName = tree->model()->index(
            0, breco::StructVisualizedTreeModel::Name, restored);
        const QModelIndex conditionValid = tree->model()->index(
            0, breco::StructVisualizedTreeModel::Valid, restored);
        QCOMPARE(tree->model()->data(conditionName).toString(),
                 QStringLiteral("magic"));
        QCOMPARE(tree->model()->data(conditionValid).toString(),
                 QStringLiteral("true"));
        QCOMPARE(
            tree->model()->data(conditionName, Qt::BackgroundRole).value<QColor>(),
            QColor(235, 255, 235));
        QCOMPARE(tree->model()
                     ->data(conditionName,
                            breco::StructVisualizedTreeModel::SourceOffsetRole)
                     .toULongLong(),
                 2ULL);
    }

    settings.setValue(QStringLiteral("ui/rememberedSingleFileOffset"), 999);
    settings.setValue(QStringLiteral("ui/structPreviewEnabled"), false);
    {
        breco::MainWindow window;
        QCoreApplication::processEvents();
        QCOMPARE(window.m_sharedCenterOffset, 3ULL);
        QVERIFY(!window.m_structModeLeftPanel->previewEnabledCheckBox()
                     ->isChecked());
        QVERIFY(!window.m_structPreview.has_value());
        QCOMPARE(settings.value(QStringLiteral("ui/rememberedSingleFileOffset"))
                     .toULongLong(),
                 3ULL);
    }

    settings.setValue(QStringLiteral("ui/structPreviewEnabled"), true);
    QVERIFY(QFile::remove(definitionPath));
    {
        breco::MainWindow window;
        QCoreApplication::processEvents();
        QVERIFY(window.m_structModeLeftPanel->isParseValid());
        QVERIFY(window.m_structModeLeftPanel->previewActive());
        QVERIFY(window.m_structPreview.has_value());
        QCOMPARE(window.m_structPreview->offset, 3ULL);
    }

    QVERIFY(QFile::remove(sourcePath));
    settings.setValue(QStringLiteral("ui/lastStructDefinitionFilePath"),
                      definitionPath);
    settings.setValue(QStringLiteral("ui/rememberedSingleFilePath"),
                      sourcePath);
    settings.setValue(QStringLiteral("ui/rememberedSingleFileOffset"), 999);
    {
        breco::MainWindow window;
        QCoreApplication::processEvents();
        QCOMPARE(window.m_scanControlsPanel->sourcePathLineEdit()->text(),
                 sourcePath);
        QCOMPARE(window.m_sourceMode, breco::MainWindow::SourceMode::None);
        QVERIFY(!window.m_structPreview.has_value());
        QCOMPARE(settings.value(QStringLiteral("ui/rememberedSingleFilePath"))
                     .toString(),
                 sourcePath);
    }
}

void MainWindowIntegrationTests::imageModeScansAndJumpsToResult() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("embedded-image.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray png = makePngBytes();
    const quint64 pngOffset = 16ULL * 1024ULL * 1024ULL + 4096ULL;
    QByteArray prefix(1024 * 1024, '\0');
    quint64 written = 0;
    while (written < pngOffset) {
        const quint64 remaining = pngOffset - written;
        const qsizetype chunkSize =
            static_cast<qsizetype>(qMin<quint64>(remaining, static_cast<quint64>(prefix.size())));
        QCOMPARE(f.write(prefix.constData(), chunkSize), chunkSize);
        written += static_cast<quint64>(chunkSize);
    }
    QCOMPARE(f.write(png), png.size());
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.selectSourcePath(absolutePath));
    QCoreApplication::processEvents();
    QVERIFY(window.m_activePreviewRow >= 0);
    QVERIFY(window.m_textHoverBuffer.baseOffset <= 1024);

    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(2);
    window.m_dataViewImagePanel->setSelectedFormats(breco::EmbeddedImageFormat::Png);
    window.m_dataViewImagePanel->setSelectedScope(breco::EmbeddedImageScope::FromStart);
    window.m_dataViewImagePanel->maxPixelsKSpinBox()->setValue(4);
    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(5);
    QCoreApplication::processEvents();

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->resultCount(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->scanButton()->text(),
                              QStringLiteral("Scan"), 10000);
    QCOMPARE(window.m_dataViewImagePanel->fileProgressBar()->value(), 1000);
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().contains(QStringLiteral(" / ")));
    QVERIFY(window.m_dataViewImagePanel->fileProgressBar()->format().contains(QStringLiteral("MiB")));
    QVERIFY(!window.m_dataViewImagePanel->resultsProgressBar()->isHidden());
    QCOMPARE(window.m_dataViewImagePanel->resultsProgressBar()->format(), QStringLiteral("1 / 5"));

    QLabel* imageLabel = nullptr;
    bool sawNumberedTitle = false;
    const auto labels = window.m_dataViewImagePanel->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (label->text().startsWith(QStringLiteral("Image: 1"))) {
            sawNumberedTitle = true;
        }
        if (imageLabel == nullptr && label->toolTip().contains(QStringLiteral("Click to jump"))) {
            imageLabel = label;
        }
    }
    QVERIFY(sawNumberedTitle);
    QVERIFY(imageLabel != nullptr);
    QTest::mouseClick(imageLabel, Qt::LeftButton, Qt::NoModifier, imageLabel->rect().center());
    QCoreApplication::processEvents();

    QCOMPARE(window.m_sharedCenterOffset, pngOffset);
    QVERIFY(window.m_textHoverBuffer.baseOffset <= pngOffset);
    QVERIFY(pngOffset < window.m_textHoverBuffer.baseOffset +
                           static_cast<quint64>(window.m_textHoverBuffer.data.size()));
}

void MainWindowIntegrationTests::imageModeStopPreservesPartialResults() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString filePath = tempDir.filePath(QStringLiteral("stop-image-scan.bin"));
    QFile f(filePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    const QByteArray png = makePngBytes();
    QCOMPARE(f.write(png), png.size());
    QVERIFY(f.resize(512LL * 1024LL * 1024LL));
    f.close();
    const QString absolutePath = QFileInfo(filePath).absoluteFilePath();

    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();
    QVERIFY(window.selectSourcePath(absolutePath));
    QCoreApplication::processEvents();

    window.m_dataViewShellPanel->modeComboBox()->setCurrentIndex(2);
    window.m_dataViewImagePanel->setSelectedFormats(breco::EmbeddedImageFormat::Png);
    window.m_dataViewImagePanel->setSelectedScope(breco::EmbeddedImageScope::FromStart);
    window.m_dataViewImagePanel->jobsSpinBox()->setValue(1);
    window.m_dataViewImagePanel->maxPixelsKSpinBox()->setValue(4);
    window.m_dataViewImagePanel->maxResultsSpinBox()->setValue(0);
    QCoreApplication::processEvents();
    QVERIFY(window.m_dataViewImagePanel->resultsProgressBar()->isHidden());

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->resultCount(), 1, 10000);
    QCOMPARE(window.m_dataViewImagePanel->scanButton()->text(), QStringLiteral("Stop"));

    window.m_dataViewImagePanel->scanButton()->click();
    QTRY_COMPARE_WITH_TIMEOUT(window.m_dataViewImagePanel->scanButton()->text(),
                              QStringLiteral("Scan"), 10000);
    QCOMPARE(window.m_dataViewImagePanel->resultCount(), 1);
    QVERIFY(window.m_dataViewImagePanel->statusLabel()->text().contains(QStringLiteral("cancelled")));
}

void MainWindowIntegrationTests::imagePanelAnimatesGifAndHighlightsHover() {
    breco::DataViewImagePanel panel;
    panel.resize(420, 320);
    panel.show();

    QImage firstFrame(2, 2, QImage::Format_ARGB32);
    firstFrame.fill(Qt::red);
    QImage secondFrame(2, 2, QImage::Format_ARGB32);
    secondFrame.fill(Qt::blue);

    breco::EmbeddedImageResult result;
    result.offset = 42;
    result.format = breco::EmbeddedImageFormat::Gif;
    result.formatName = QStringLiteral("GIF");
    result.size = firstFrame.size();
    result.image = firstFrame;
    result.encodedData = QByteArray("GIF89a animated payload");
    result.animationFrames = {firstFrame, secondFrame};
    result.frameDelaysMs = {16, 16};
    panel.addResult(result);
    QCoreApplication::processEvents();

    QLabel* title = panel.findChild<QLabel*>(QStringLiteral("imageResultTitle"));
    QLabel* preview = panel.findChild<QLabel*>(QStringLiteral("imagePreviewLabel"));
    QFrame* card = panel.findChild<QFrame*>(QStringLiteral("imageResultCard"));
    QTimer* timer = panel.findChild<QTimer*>(QStringLiteral("imageAnimationTimer"));
    QVERIFY(title != nullptr);
    QVERIFY(preview != nullptr);
    QVERIFY(card != nullptr);
    QVERIFY(timer != nullptr);
    QVERIFY(title->text().contains(QStringLiteral("2 frames")));
    QVERIFY(preview->toolTip().contains(QStringLiteral("Right-click to save")));
    QVERIFY(timer->isActive());
    QCOMPARE(timer->interval(), 16);

    const QColor initialColor = preview->pixmap().toImage().pixelColor(0, 0);
    QTRY_VERIFY_WITH_TIMEOUT(preview->pixmap().toImage().pixelColor(0, 0) != initialColor, 1000);

    QEnterEvent enterEvent(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QCoreApplication::sendEvent(preview, &enterEvent);
    QVERIFY(card->autoFillBackground());
    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(preview, &leaveEvent);
    QVERIFY(!card->autoFillBackground());
}

}  // namespace

QTEST_MAIN(MainWindowIntegrationTests)
#include "mainwindow_integration_tests.moc"
