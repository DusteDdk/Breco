#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QRadioButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#define private public
#include "app/MainWindow.h"
#undef private
#include "panel/CurrentByteInfoPanel.h"
#include "panel/ResultsTablePanel.h"
#include "panel/ScanControlsPanel.h"
#include "view/TextViewWidget.h"

namespace {

class MainWindowIntegrationTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void lifecycleCardLogsAndResetsPerScan();
    void selectingResultRowUpdatesPreviewBuffers();
    void currentBytePanelShowsEndianAndWidthAwareValues();
    void shiftMarksCurrentBufferDirtyAndRestoresOnDeselect();
};

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

void MainWindowIntegrationTests::currentBytePanelShowsEndianAndWidthAwareValues() {
    breco::MainWindow window;
    window.show();
    QCoreApplication::processEvents();

    breco::MainWindow::HoverBuffer hover;
    hover.filePath = QStringLiteral("in-memory");
    hover.baseOffset = 100;
    hover.data = QByteArray::fromHex("4100FF");

    window.m_currentByteInfoPanel->bigEndianCharModeRadioButton()->setChecked(true);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();

    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("A"));
    QCOMPARE(window.m_currentByteInfoPanel->u8ValueLabel()->text(), QStringLiteral("65"));
    QCOMPARE(window.m_currentByteInfoPanel->u16LeValueLabel()->text(), QStringLiteral("65"));
    QCOMPARE(window.m_currentByteInfoPanel->u16BeValueLabel()->text(), QStringLiteral("16640"));
    QCOMPARE(window.m_currentByteInfoPanel->u32LeValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->u64LeValueLabel()->text(), QStringLiteral("n/a"));
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("A"));

    window.m_currentByteInfoPanel->littleEndianCharModeRadioButton()->setChecked(true);
    window.updateCurrentByteInfoFromHover(hover, 100);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->byteInterpretationLargeLabel()->text(), QStringLiteral("-"));

    window.updateCurrentByteInfoFromHover(hover, 101);
    QCoreApplication::processEvents();
    QCOMPARE(window.m_currentByteInfoPanel->asciiValueLabel()->text(), QStringLiteral("."));
    QCOMPARE(window.m_currentByteInfoPanel->u16LeValueLabel()->text(), QStringLiteral("65280"));
    QCOMPARE(window.m_currentByteInfoPanel->u16BeValueLabel()->text(), QStringLiteral("255"));
}

void MainWindowIntegrationTests::shiftMarksCurrentBufferDirtyAndRestoresOnDeselect() {
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

    QVERIFY(window.m_shiftUnitCombo != nullptr);
    QVERIFY(window.m_shiftValueSpin != nullptr);
    window.m_shiftUnitCombo->setCurrentIndex(0);
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
}

}  // namespace

QTEST_MAIN(MainWindowIntegrationTests)
#include "mainwindow_integration_tests.moc"
