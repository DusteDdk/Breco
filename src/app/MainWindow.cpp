#include "app/MainWindow.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>

#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QComboBox>
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QStringList>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "debug/SelectionTrace.h"
#include "io/FileEnumerator.h"
#include "io/ProtectedSourceOpener.h"
#include "image/EmbeddedImageScanner.h"
#include "panel/BitmapViewPanel.h"
#include "panel/CurrentByteInfoPanel.h"
#include "panel/DataViewByteAndBitmapPanel.h"
#include "panel/DataViewImagePanel.h"
#include "panel/DataViewShellPanel.h"
#include "panel/DataViewStructuredPanel.h"
#include "panel/HexViewControlsPanel.h"
#include "panel/MainTabsPanel.h"
#include "panel/ResultsTablePanel.h"
#include "panel/ScanControlsPanel.h"
#include "panel/StructDataViewPanel.h"
#include "panel/StructModeLeftPanel.h"
#include "struct/StructVisualizer.h"
#include "struct/VisualizedNode.h"
#include "panel/TextViewPanel.h"
#include "scan/ShiftTransform.h"
#include "settings/AppSettings.h"
#include "ui_AboutDialog.h"
#include "ui_EditStack.h"
#include "ui_MainWindow.h"
#include "view/BitmapViewWidget.h"
#include "view/TextViewWidget.h"

namespace breco {

namespace {
constexpr quint64 kEvictedWindowRadiusBytes = 8ULL * 1024ULL * 1024ULL;
constexpr quint64 kResultBufferCacheBudgetBytes = 2048ULL * 1024ULL * 1024ULL;
constexpr quint64 kNotEmptyInitialBytes = 16ULL * 1024ULL * 1024ULL;
constexpr quint64 kTextChunkExpandStepBytes = 8ULL * 1024ULL * 1024ULL;
constexpr quint64 kBinarySaveChunkBytes = 16ULL * 1024ULL * 1024ULL;
constexpr int kTopPaneMinHeightPx = 180;
constexpr int kAdvancedSnapHideThresholdPx = 190;
constexpr int kAdvancedSnapShowThresholdPx = 260;

bool isRegularOrBlockDevice(const QFileInfo& info) {
    if (!info.exists()) {
        return false;
    }
    if (info.isFile()) {
        return true;
    }

#ifdef Q_OS_LINUX
    struct stat st {};
    const QByteArray pathBytes = info.absoluteFilePath().toLocal8Bit();
    if (::stat(pathBytes.constData(), &st) != 0) {
        return false;
    }
    return S_ISBLK(st.st_mode);
#else
    return false;
#endif
}

quint64 fileSizeWithBlockDeviceSupport(const QFileInfo& info) {
    if (!isRegularOrBlockDevice(info)) {
        return 0;
    }
    if (info.isFile()) {
        const qint64 size = info.size();
        return size > 0 ? static_cast<quint64>(size) : 0;
    }

#ifdef Q_OS_LINUX
    const QByteArray pathBytes = info.absoluteFilePath().toLocal8Bit();
    const int fd = ::open(pathBytes.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    quint64 bytes = 0;
    const int rc = ::ioctl(fd, BLKGETSIZE64, &bytes);
    ::close(fd);
    if (rc != 0) {
        return 0;
    }
    return bytes;
#else
    return 0;
#endif
}

quint64 readUnsignedLittle(const QByteArray& bytes, int start, int widthBytes, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (start < 0 || widthBytes <= 0 || start + widthBytes > bytes.size()) {
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < widthBytes; ++i) {
        value |= (static_cast<quint64>(static_cast<unsigned char>(bytes.at(start + i))) << (8 * i));
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

quint64 readUnsignedBig(const QByteArray& bytes, int start, int widthBytes, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (start < 0 || widthBytes <= 0 || start + widthBytes > bytes.size()) {
        return 0;
    }
    quint64 value = 0;
    for (int i = 0; i < widthBytes; ++i) {
        value = (value << 8U) | static_cast<quint64>(static_cast<unsigned char>(bytes.at(start + i)));
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return value;
}

QString printableAsciiChar(unsigned char byte) {
    if (byte >= 0x20 && byte <= 0x7E) {
        return QString(QChar::fromLatin1(static_cast<char>(byte)));
    }
    if (byte == '\n') {
        return QStringLiteral("\\n");
    }
    if (byte == '\r') {
        return QStringLiteral("\\r");
    }
    if (byte == '\t') {
        return QStringLiteral("\\t");
    }
    return QStringLiteral(".");
}

QString utf8Glyph(const QByteArray& bytes, int start) {
    if (start < 0 || start >= bytes.size()) {
        return QStringLiteral("n/a");
    }
    const QByteArray slice = bytes.mid(start, qMin(4, bytes.size() - start));
    const QString decoded = QString::fromUtf8(slice);
    if (decoded.isEmpty()) {
        return QStringLiteral("n/a");
    }
    return decoded.left(1);
}

std::optional<quint32> utf16CodepointAt(const QByteArray& bytes, int start, bool littleEndian) {
    if (start < 0 || start + 1 >= bytes.size()) {
        return std::nullopt;
    }

    const auto readUnit = [&bytes, littleEndian](int index) {
        const quint16 first = static_cast<unsigned char>(bytes.at(index));
        const quint16 second = static_cast<unsigned char>(bytes.at(index + 1));
        return littleEndian ? static_cast<quint16>(first | (second << 8U))
                            : static_cast<quint16>((first << 8U) | second);
    };

    const quint16 first = readUnit(start);
    if (first >= 0xD800U && first <= 0xDBFFU) {
        if (start + 3 >= bytes.size()) {
            return std::nullopt;
        }
        const quint16 second = readUnit(start + 2);
        if (second < 0xDC00U || second > 0xDFFFU) {
            return std::nullopt;
        }
        return 0x10000U +
               ((static_cast<quint32>(first - 0xD800U) << 10U) |
                static_cast<quint32>(second - 0xDC00U));
    }
    if (first >= 0xDC00U && first <= 0xDFFFU) {
        return std::nullopt;
    }
    return first;
}

QString utf16Glyph(const QByteArray& bytes, int start, bool littleEndian,
                   const QString& unavailableText) {
    const std::optional<quint32> codepoint = utf16CodepointAt(bytes, start, littleEndian);
    if (!codepoint.has_value()) {
        return unavailableText;
    }
    const char32_t glyph = static_cast<char32_t>(codepoint.value());
    return QString::fromUcs4(&glyph, 1);
}

QString formatHex(quint64 value, int widthNibbles) {
    return QStringLiteral("0x%1").arg(value, widthNibbles, 16, QChar('0')).toUpper();
}

QString utf16DisplayGlyph(const QByteArray& bytes, int start, bool littleEndian) {
    const std::optional<quint32> codepoint = utf16CodepointAt(bytes, start, littleEndian);
    if (!codepoint.has_value() || codepoint.value() < 0x20U ||
        (codepoint.value() >= 0x7FU && codepoint.value() <= 0x9FU)) {
        return QStringLiteral("-");
    }
    const char32_t glyph = static_cast<char32_t>(codepoint.value());
    return QString::fromUcs4(&glyph, 1);
}

QString signedValueString(quint64 value, int widthBytes) {
    switch (widthBytes) {
        case 1:
            return QString::number(static_cast<qint8>(value & 0xFFU));
        case 2:
            return QString::number(static_cast<qint16>(value & 0xFFFFU));
        case 4:
            return QString::number(static_cast<qint32>(value & 0xFFFFFFFFULL));
        case 8:
            return QString::number(static_cast<qint64>(value));
        default:
            return QStringLiteral("n/a");
    }
}

qint64 signedValueFromWidth(quint64 value, int widthBytes) {
    switch (widthBytes) {
        case 1:
            return static_cast<qint8>(value & 0xFFU);
        case 2:
            return static_cast<qint16>(value & 0xFFFFU);
        case 4:
            return static_cast<qint32>(value & 0xFFFFFFFFULL);
        case 8:
            return static_cast<qint64>(value);
        default:
            return 0;
    }
}

enum class NumberSystem { Decimal = 0, Hex = 1, Octal = 2 };

NumberSystem currentNumberSystem(const CurrentByteInfoPanel* panel) {
    if (panel == nullptr) {
        return NumberSystem::Decimal;
    }
    if (panel->hexModeRadioButton()->isChecked()) {
        return NumberSystem::Hex;
    }
    if (panel->octalModeRadioButton()->isChecked()) {
        return NumberSystem::Octal;
    }
    return NumberSystem::Decimal;
}

QString formatUnsignedByNumberSystem(quint64 value, int widthBytes, NumberSystem system) {
    switch (system) {
        case NumberSystem::Hex:
            return QStringLiteral("0x%1").arg(value, widthBytes * 2, 16, QChar('0')).toUpper();
        case NumberSystem::Octal:
            return QStringLiteral("0o%1").arg(QString::number(value, 8));
        case NumberSystem::Decimal:
        default:
            return QString::number(value);
    }
}

QString formatSignedByNumberSystem(quint64 value, int widthBytes, NumberSystem system) {
    if (system == NumberSystem::Decimal) {
        return signedValueString(value, widthBytes);
    }

    const qint64 signedValue = signedValueFromWidth(value, widthBytes);
    if (signedValue >= 0) {
        return formatUnsignedByNumberSystem(static_cast<quint64>(signedValue), widthBytes, system);
    }
    const quint64 magnitude = static_cast<quint64>(-(signedValue + 1)) + 1ULL;
    const QString prefix = (system == NumberSystem::Hex) ? QStringLiteral("0x") : QStringLiteral("0o");
    const int base = (system == NumberSystem::Hex) ? 16 : 8;
    const QString digits =
        (system == NumberSystem::Hex)
            ? QStringLiteral("%1").arg(magnitude, widthBytes * 2, base, QChar('0')).toUpper()
            : QString::number(magnitude, base);
    return QStringLiteral("-%1%2").arg(prefix, digits);
}

QString formatHexWindow8(const QByteArray& bytes, int start, bool bigEndian) {
    std::array<QString, 8> parts;
    for (int i = 0; i < 8; ++i) {
        const int idx = bigEndian ? (start + i) : (start + (7 - i));
        if (idx >= 0 && idx < bytes.size()) {
            const unsigned char b = static_cast<unsigned char>(bytes.at(idx));
            parts[static_cast<std::size_t>(i)] =
                QStringLiteral("%1").arg(static_cast<int>(b), 2, 16, QChar('0')).toUpper();
        } else {
            parts[static_cast<std::size_t>(i)] = QStringLiteral("--");
        }
    }
    return QStringLiteral("0 x %1")
        .arg(QStringList(parts.begin(), parts.end()).join(QLatin1Char(' ')));
}

void assignVisualizedSource(VisualizedNode& node, const QString& filePath,
                            quint64 dataBaseOffset) {
    node.sourceFilePath = filePath;
    if (node.hasSourceOffset) {
        if (node.sourceOffset <=
            std::numeric_limits<quint64>::max() - dataBaseOffset) {
            node.sourceOffset += dataBaseOffset;
        } else {
            node.hasSourceOffset = false;
        }
    }
    for (VisualizedNode& child : node.children) {
        assignVisualizedSource(child, filePath, dataBaseOffset);
    }
}
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      m_ui(std::make_unique<Ui::MainWindow>()),
      m_resultModel(this),
      m_filePool(),
      m_windowLoader(&m_filePool),
      m_scanController(&m_filePool, this) {
    const QString rememberedStructDefinitionPath =
        AppSettings::lastStructDefinitionFilePath();
    const QString rememberedStructDeclaration =
        AppSettings::structDeclarationText();
    const QString rememberedStructEntryName =
        AppSettings::structEntryName();
    const int rememberedStructEntryCount =
        AppSettings::structEntryCount();
    const QString rememberedSingleFile =
        AppSettings::rememberedSingleFilePath();
    const quint64 rememberedSingleFileOffset =
        AppSettings::rememberedSingleFileOffset();

    m_ui->setupUi(this);
    m_protectedSourceOpener = std::make_unique<DefaultProtectedSourceOpener>();
    m_imageScanController = std::make_unique<EmbeddedImageScanController>();

    m_mainTabsPanel = m_ui->mainTabsPanel;
    m_scanControlsPanel = m_mainTabsPanel->scanControlsPanel();
    m_mainTabsPanel->scanTabLayout()->setStretch(0, 0);
    m_mainTabsPanel->scanTabLayout()->setStretch(1, 1);

    Ui::EditStack editStackUi;
    editStackUi.setupUi(m_mainTabsPanel->editStack());
    m_mainTabsPanel->editStack()->setVisible(false);
    m_ui->viewsStackLayout->setStretch(0, 1);

    auto* resultsHostLayout = new QVBoxLayout(m_mainTabsPanel->resultsPanelHost());
    resultsHostLayout->setContentsMargins(0, 0, 0, 0);
    resultsHostLayout->setSpacing(0);
    m_resultsPanel = new ResultsTablePanel(m_mainTabsPanel->resultsPanelHost());
    resultsHostLayout->addWidget(m_resultsPanel);
    m_mainTabsPanel->resultsPanelHost()->setMinimumHeight(kTopPaneMinHeightPx);

    auto* rawDataHostLayout = new QVBoxLayout(m_mainTabsPanel->rawDataHost());
    rawDataHostLayout->setContentsMargins(0, 0, 0, 0);
    rawDataHostLayout->setSpacing(0);
    m_rawDataViewShellPanel = new DataViewShellPanel(
        DataViewShellPanel::ControlMode::Raw, m_mainTabsPanel->rawDataHost());
    rawDataHostLayout->addWidget(m_rawDataViewShellPanel);
    auto* rawBodyLayout = new QVBoxLayout(m_rawDataViewShellPanel->bodyHost());
    rawBodyLayout->setContentsMargins(0, 0, 0, 0);
    rawBodyLayout->setSpacing(0);
    m_dataViewByteAndBitmapPanel =
        new DataViewByteAndBitmapPanel(m_rawDataViewShellPanel->bodyHost());
    rawBodyLayout->addWidget(m_dataViewByteAndBitmapPanel);

    auto* structDataHostLayout = new QVBoxLayout(m_mainTabsPanel->structDataHost());
    structDataHostLayout->setContentsMargins(0, 0, 0, 0);
    structDataHostLayout->setSpacing(0);
    m_structDataViewShellPanel = new DataViewShellPanel(
        DataViewShellPanel::ControlMode::Struct, m_mainTabsPanel->structDataHost());
    structDataHostLayout->addWidget(m_structDataViewShellPanel);
    auto* structBodyLayout = new QVBoxLayout(m_structDataViewShellPanel->bodyHost());
    structBodyLayout->setContentsMargins(0, 0, 0, 0);
    structBodyLayout->setSpacing(0);
    m_dataViewStructuredPanel =
        new DataViewStructuredPanel(m_structDataViewShellPanel->bodyHost());
    structBodyLayout->addWidget(m_dataViewStructuredPanel);

    auto* imageDataHostLayout = new QVBoxLayout(m_mainTabsPanel->imageDataHost());
    imageDataHostLayout->setContentsMargins(0, 0, 0, 0);
    imageDataHostLayout->setSpacing(0);
    m_dataViewImagePanel = new DataViewImagePanel(m_mainTabsPanel->imageDataHost());
    imageDataHostLayout->addWidget(m_dataViewImagePanel);

    auto* hexControlsHostLayout = new QVBoxLayout(m_ui->hexViewControlsPanelHost);
    hexControlsHostLayout->setContentsMargins(0, 0, 0, 0);
    hexControlsHostLayout->setSpacing(0);
    m_hexControlsPanel = new HexViewControlsPanel(m_ui->hexViewControlsPanelHost);
    hexControlsHostLayout->addWidget(m_hexControlsPanel);
    m_shiftValueSpin = m_hexControlsPanel->shiftBitsSpinBox();

    auto* textHostLayout = new QVBoxLayout(m_ui->textViewPanelHost);
    textHostLayout->setContentsMargins(0, 0, 0, 0);
    textHostLayout->setSpacing(0);
    m_textPanel = new TextViewPanel(m_ui->textViewPanelHost);
    textHostLayout->addWidget(m_textPanel);

    auto* currentByteHostLayout =
        new QVBoxLayout(m_dataViewByteAndBitmapPanel->currentCharacterHost());
    currentByteHostLayout->setContentsMargins(0, 0, 0, 0);
    currentByteHostLayout->setSpacing(0);
    m_dataViewByteAndBitmapPanel->currentCharacterHost()->setSizePolicy(
        QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_dataViewByteAndBitmapPanel->currentCharacterHost()->setMinimumSize(0, 0);
    m_currentByteInfoPanel =
        new CurrentByteInfoPanel(m_dataViewByteAndBitmapPanel->currentCharacterHost());
    m_currentByteInfoPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_currentByteInfoPanel->setTitle(QStringLiteral("Current Character"));
    currentByteHostLayout->addWidget(m_currentByteInfoPanel);
    m_currentByteInfoPanel->bigEndianCheckBox()->setVisible(false);
    m_currentByteInfoPanel->decimalModeRadioButton()->setChecked(true);

    auto* bitmapHostLayout = new QVBoxLayout(m_dataViewByteAndBitmapPanel->bitmapHost());
    bitmapHostLayout->setContentsMargins(0, 0, 0, 0);
    bitmapHostLayout->setSpacing(0);
    m_bitmapPanel = new BitmapViewPanel(m_dataViewByteAndBitmapPanel->bitmapHost());
    bitmapHostLayout->addWidget(m_bitmapPanel);
    if (QWidget* bitmapChrome = m_bitmapPanel->bitmapModeCombo()->parentWidget();
        bitmapChrome != nullptr) {
        bitmapChrome->setVisible(false);
    }

    auto* structEditorLayout = new QVBoxLayout(m_dataViewStructuredPanel->structEditorHost());
    structEditorLayout->setContentsMargins(0, 0, 0, 0);
    structEditorLayout->setSpacing(0);
    m_structModeLeftPanel =
        new StructModeLeftPanel(m_dataViewStructuredPanel->structEditorHost());
    m_structModeLeftPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    structEditorLayout->addWidget(m_structModeLeftPanel);

    auto* structViewLayout = new QVBoxLayout(m_dataViewStructuredPanel->structViewHost());
    structViewLayout->setContentsMargins(0, 0, 0, 0);
    structViewLayout->setSpacing(0);
    m_structDataViewPanel = new StructDataViewPanel(m_dataViewStructuredPanel->structViewHost());
    structViewLayout->addWidget(m_structDataViewPanel);

    QTableView* resultsTable = m_resultsPanel->resultsTableView();
    resultsTable->setModel(&m_resultModel);
    resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    resultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int col = 2; col < m_resultModel.columnCount(); ++col) {
        resultsTable->horizontalHeader()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    }

    m_textPanel->textModeCombo()->addItems(
        {QStringLiteral("ASCII"), QStringLiteral("UTF-8"), QStringLiteral("UTF-16")});
    if (QWidget* textChrome = m_textPanel->textModeCombo()->parentWidget();
        textChrome != nullptr) {
        textChrome->setVisible(false);
    }
    m_scanControlsPanel->workerCountCombo()->clear();
    const int threadCount = qMax(1, QThread::idealThreadCount());
    for (int workers = 1; workers <= threadCount; ++workers) {
        m_scanControlsPanel->workerCountCombo()->addItem(QString::number(workers), workers);
    }
    m_scanControlsPanel->workerCountCombo()->setCurrentIndex(threadCount - 1);
    const int defaultBlockSizeValue = qMax(1, threadCount * 16);
    const int restoredBlockSizeValue =
        qBound(m_scanControlsPanel->blockSizeSpin()->minimum(),
               AppSettings::scanBlockSizeValue(defaultBlockSizeValue),
               m_scanControlsPanel->blockSizeSpin()->maximum());
    const int restoredBlockSizeUnitIndex =
        qBound(0, AppSettings::scanBlockSizeUnitIndex(),
               m_scanControlsPanel->blockSizeUnitCombo()->count() - 1);
    m_scanControlsPanel->blockSizeSpin()->setValue(restoredBlockSizeValue);
    m_scanControlsPanel->blockSizeUnitCombo()->setCurrentIndex(restoredBlockSizeUnitIndex);

    m_textView = new TextViewWidget(m_textPanel->textViewContainer());
    m_bitmapView = new BitmapViewWidget(m_bitmapPanel->bitmapViewContainer());
    m_textView->setMinimumHeight(220);
    m_bitmapView->setMinimumHeight(220);
    m_textView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_bitmapView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_textPanel->textViewContainer()->setMinimumHeight(220);
    m_bitmapPanel->bitmapViewContainer()->setMinimumHeight(220);

    auto* textLayout = new QVBoxLayout(m_textPanel->textViewContainer());
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);
    textLayout->addWidget(m_textView);

    auto* bitmapLayout = new QVBoxLayout(m_bitmapPanel->bitmapViewContainer());
    bitmapLayout->setContentsMargins(0, 0, 0, 0);
    bitmapLayout->setSpacing(0);
    bitmapLayout->addWidget(m_bitmapView);

    m_textPanel->textViewPanelLayout()->setStretch(1, 1);
    m_bitmapPanel->bitmapViewPanelLayout()->setStretch(1, 1);
    m_ui->verticalLayout->setStretch(1, 1);
    m_ui->verticalLayout->setStretch(2, 2);

    m_sourcePathValidationTimer = new QTimer(this);
    m_sourcePathValidationTimer->setSingleShot(true);
    m_sourcePathValidationTimer->setInterval(250);
    connect(m_sourcePathValidationTimer, &QTimer::timeout, this,
            &MainWindow::validateSourcePathInput);
    connect(m_scanControlsPanel->sourcePathLineEdit(), &QLineEdit::textChanged, this,
            &MainWindow::onSourcePathTextChanged);
    connect(m_scanControlsPanel->sourcePathLineEdit(), &QLineEdit::returnPressed, this, [this]() {
        applySourcePath(m_scanControlsPanel->sourcePathLineEdit()->text(), true);
    });
    connect(m_scanControlsPanel->openFileButton(), &QToolButton::clicked, this,
            &MainWindow::onOpenFile);
    connect(m_scanControlsPanel->openDirButton(), &QToolButton::clicked, this,
            &MainWindow::onOpenDirectory);
    connect(m_scanControlsPanel->startScanButton(), &QPushButton::clicked, this,
            &MainWindow::onStartScan);
    connect(m_scanControlsPanel->searchTermLineEdit(), &QLineEdit::returnPressed, this,
            &MainWindow::onStartScan);
    connect(m_hexControlsPanel->offsetValueEdit(), &QLineEdit::returnPressed, this,
            [this]() { commitHexNavigatorEdit(HexNavigatorField::Offset); });
    connect(m_hexControlsPanel->selectedValueEdit(), &QLineEdit::returnPressed, this,
            [this]() { commitHexNavigatorEdit(HexNavigatorField::Selected); });
    connect(m_hexControlsPanel->selectToValueEdit(), &QLineEdit::returnPressed, this,
            [this]() { commitHexNavigatorEdit(HexNavigatorField::SelectTo); });
    for (QLineEdit* edit : {m_hexControlsPanel->offsetValueEdit(),
                            m_hexControlsPanel->selectedValueEdit(),
                            m_hexControlsPanel->selectToValueEdit()}) {
        connect(edit, &QLineEdit::editingFinished, this,
                [this]() { updateHexInfoPanel(); });
    }
    connect(m_structModeLeftPanel, &StructModeLeftPanel::structureScanRequested,
            this, &MainWindow::onStartStructureScan);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::structureScanStopRequested,
            this, &MainWindow::onStopScan);
    connect(resultsTable->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) { onResultActivated(current); });
    connect(m_hexControlsPanel->showAsComboBox(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::onTextModeChanged);
    connect(m_hexControlsPanel->wrapCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setWrapMode(checked);
                AppSettings::setTextWrapModeEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->collapseCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setCollapseRunsEnabled(checked);
                AppSettings::setTextCollapseEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->breatheCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setBreatheEnabled(checked);
                AppSettings::setTextBreatheEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->newlineModeComboBox(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int idx) {
                m_textView->setNewlineMode(static_cast<TextNewlineMode>(qBound(0, idx, 4)));
                AppSettings::setTextNewlineModeIndex(idx);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->monospaceCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setMonospaceEnabled(checked);
                AppSettings::setTextMonospaceEnabled(checked);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->bytesPerLineComboBox(),
            qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
                m_textView->setByteLineMode(static_cast<ByteLineMode>(qBound(0, idx, 4)));
                AppSettings::setTextByteLineModeIndex(idx);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->stringsOnlyCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setStringsOnlyEnabled(checked);
                AppSettings::setHexStringsOnlyEnabled(checked);
                updateHexControlsVisibility();
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->highlightResultCheckBox(), &QCheckBox::toggled, this,
            [this](bool checked) {
                m_textView->setResultHighlightEnabled(checked);
                m_bitmapView->setResultOverlayEnabled(checked);
                AppSettings::setHexHighlightResultEnabled(checked);
            });
    connect(m_hexControlsPanel->littleEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (!checked) {
                    return;
                }
                m_textView->setUtf16LittleEndian(true);
                AppSettings::setHexBigEndianEnabled(false);
                scheduleSharedPreviewUpdate();
            });
    connect(m_hexControlsPanel->bigEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (!checked) {
                    return;
                }
                m_textView->setUtf16LittleEndian(false);
                AppSettings::setHexBigEndianEnabled(true);
                scheduleSharedPreviewUpdate();
            });

    connect(m_rawDataViewShellPanel->bitmapModeComboBox(), qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MainWindow::onBitmapModeChanged);
    connect(m_rawDataViewShellPanel->textInterpretationComboBox(),
            qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
                m_bitmapView->setTextMode(selectedDataViewTextMode());
                AppSettings::setDataViewTextModeIndex(idx);
                refreshDataViewFromNavigator();
            });
    connect(m_rawDataViewShellPanel->littleEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (checked) {
                    setDataViewBigEndianEnabled(false);
                }
            });
    connect(m_rawDataViewShellPanel->bigEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (checked) {
                    setDataViewBigEndianEnabled(true);
                }
            });
    connect(m_structDataViewShellPanel->littleEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (checked) {
                    setDataViewBigEndianEnabled(false);
                }
            });
    connect(m_structDataViewShellPanel->bigEndianRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (checked) {
                    setDataViewBigEndianEnabled(true);
                }
            });
    connect(m_dataViewImagePanel, &DataViewImagePanel::scanRequested, this,
            &MainWindow::startImageScan);
    connect(m_dataViewImagePanel, &DataViewImagePanel::resultActivated, this,
            &MainWindow::jumpToAbsoluteOffset);
    connect(m_imageScanController.get(), &EmbeddedImageScanController::progressUpdated, this,
            [this](quint64 scanId, const ScanProgressSnapshot& progress,
                   int resultsFound, int resultsLimit) {
                if (scanId == m_activeImageScanId && m_dataViewImagePanel != nullptr) {
                    m_dataViewImagePanel->updateProgress(progress, resultsFound, resultsLimit);
                }
            });
    connect(m_imageScanController.get(), &EmbeddedImageScanController::resultReady, this,
            [this](quint64 scanId, const EmbeddedImageResult& result) {
                if (scanId == m_activeImageScanId && m_dataViewImagePanel != nullptr) {
                    m_dataViewImagePanel->addResult(result);
                }
            });
    connect(m_imageScanController.get(), &EmbeddedImageScanController::scanFinished, this,
            &MainWindow::finishImageScan);
    const auto persistImageFormats = [this]() {
        AppSettings::setDataViewImageFormatMask(m_dataViewImagePanel->selectedFormats().toInt());
    };
    for (const EmbeddedImageFormat format :
         {EmbeddedImageFormat::Tga, EmbeddedImageFormat::Tiff, EmbeddedImageFormat::Png,
          EmbeddedImageFormat::Jpeg, EmbeddedImageFormat::Bmp, EmbeddedImageFormat::Ico,
          EmbeddedImageFormat::Gif, EmbeddedImageFormat::Xbm, EmbeddedImageFormat::Xpm,
          EmbeddedImageFormat::Svg}) {
        if (QCheckBox* checkBox = m_dataViewImagePanel->formatCheckBox(format);
            checkBox != nullptr) {
            connect(checkBox, &QCheckBox::toggled, this, persistImageFormats);
        }
    }
    connect(m_dataViewImagePanel->scopeComboBox(), qOverload<int>(&QComboBox::currentIndexChanged),
            this, [](int idx) { AppSettings::setDataViewImageScopeIndex(idx); });
    connect(m_dataViewImagePanel->maxPixelsKSpinBox(), qOverload<int>(&QSpinBox::valueChanged),
            this, [](int value) { AppSettings::setDataViewImageMaxPixelsK(value); });
    connect(m_dataViewImagePanel->maxResultsSpinBox(), qOverload<int>(&QSpinBox::valueChanged),
            this, [](int value) { AppSettings::setDataViewImageMaxResults(value); });
    connect(m_dataViewImagePanel->jobsSpinBox(), qOverload<int>(&QSpinBox::valueChanged),
            this, [](int value) { AppSettings::setDataViewImageJobs(value); });
    connect(m_scanControlsPanel->blockSizeSpin(), qOverload<int>(&QSpinBox::valueChanged), this,
            [this](int value) {
                AppSettings::setScanBlockSizeValue(value);
                updateBlockSizeLabel();
            });
    connect(m_scanControlsPanel->blockSizeUnitCombo(), qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
                AppSettings::setScanBlockSizeUnitIndex(index);
                updateBlockSizeLabel();
            });

    if (m_shiftValueSpin != nullptr) {
        connect(m_shiftValueSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) {
            cancelImageScan();
            const QModelIndex current = m_resultsPanel->resultsTableView()->currentIndex();
            if (current.isValid()) {
                onResultActivated(current);
            } else if (m_activePreviewRow >= 0) {
                selectResultRow(m_activePreviewRow);
            }
        });
    }

    connect(m_textView, &TextViewWidget::centerAnchorOffsetChanged, this,
            &MainWindow::onTextCenterAnchorRequested);
    connect(m_textView, &TextViewWidget::hoverAbsoluteOffsetChanged, this,
            &MainWindow::onTextHoverOffsetChanged);
    connect(m_textView, &TextViewWidget::hoverLeft, this, &MainWindow::onHoverLeft);
    connect(m_textView, &TextViewWidget::selectionRangeChanged, this,
            [this](bool hasRange, quint64 start, quint64 end) {
                if (!m_structNavigationInProgress) {
                    if (!m_previewSyncInProgress) {
                        clearStructSourceHighlight();
                    }
                }
                if (!hasRange) {
                    m_activeTextSelectionRange.reset();
                    m_bitmapView->setExternalSelectionRange(
                        m_structSourceHighlightRange);
                    updateHexInfoPanel();
                    refreshDataViewFromNavigator();
                    return;
                }
                m_activeTextSelectionRange = qMakePair(start, end);
                m_bitmapView->setExternalSelectionRange(m_activeTextSelectionRange);
                updateHexInfoPanel();
                refreshDataViewFromNavigator();
            });
    connect(m_textView, &TextViewWidget::viewportFirstByteOffsetChanged, this,
            [this](bool, quint64) {
                updateHexInfoPanel();
                refreshDataViewFromNavigator();
            });
    connect(m_textView, &TextViewWidget::backingScrollRequested, this,
            &MainWindow::onTextBackingScrollRequested);
    connect(m_textView, &TextViewWidget::verticalScrollDragStateChanged, this,
            [this](bool dragging) {
                m_textScrollDragInProgress = dragging;
                if (!dragging && m_pendingPreviewAfterTextScrollDrag) {
                    m_pendingPreviewAfterTextScrollDrag = false;
                    scheduleSharedPreviewUpdate();
                }
            });
    connect(m_textView, &TextViewWidget::verticalScrollDragReleased, this,
            [this](int value, int maximum) {
                requestSharedCenterFromTextScrollPosition(value, maximum);
            });
    connect(m_textView, &TextViewWidget::pageNavigationRequested, this,
            [this](int direction, quint64 edgeOffset) {
                m_pendingPageDirection = (direction < 0) ? -1 : ((direction > 0) ? 1 : 0);
                m_pendingPageEdgeOffset = edgeOffset;
                scheduleSharedPreviewUpdate();
            });
    connect(m_textView, &TextViewWidget::fileEdgeNavigationRequested, this, [this](int edge) {
        m_pendingFileEdgeNavigation = (edge < 0) ? -1 : ((edge > 0) ? 1 : 0);
        m_pendingPageDirection = 0;
        m_pendingPageEdgeOffset.reset();
        scheduleSharedPreviewUpdate();
    });
    connect(m_textView, &TextViewWidget::chunkEdgeExpansionRequested, this, [this](int direction) {
        if (!expandActivePreviewBuffer(direction)) {
            return;
        }
        scheduleSharedPreviewUpdate();
        updateBufferStatusLine();
    });
    connect(m_textView, &TextViewWidget::byteClicked, this, &MainWindow::onTextByteClicked);
    connect(m_textView, &TextViewWidget::saveBinarySelectionRequested, this,
            &MainWindow::saveSelectedBinaryRange);
    connect(m_textView, &TextViewWidget::saveBinaryFromHereRequested, this,
            &MainWindow::saveBinaryFromHere);
    connect(m_bitmapView, &BitmapViewWidget::hoverAbsoluteOffsetChanged, this,
            &MainWindow::onBitmapHoverOffsetChanged);
    connect(m_bitmapView, &BitmapViewWidget::byteClicked, this, &MainWindow::onBitmapByteClicked);
    connect(m_bitmapView, &BitmapViewWidget::hoverLeft, this, &MainWindow::onHoverLeft);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::previewRequested, this, [this]() {
        syncStructPreviewToControls();
    });
    connect(m_structModeLeftPanel, &StructModeLeftPanel::previewClearRequested,
            this, &MainWindow::clearStructPreview);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::addViewRequested,
            this, &MainWindow::addCurrentStructView);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::currentViewsRemoved,
            this, &MainWindow::removeCurrentStructViews);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::currentViewChanged,
            this, &MainWindow::updateCurrentStructView);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::sourceLocationActivated,
            this, &MainWindow::navigateToStructSource);
    connect(m_structDataViewPanel, &StructDataViewPanel::sourceLocationActivated,
            this, &MainWindow::navigateToStructSource);
    connect(m_structDataViewPanel,
            &StructDataViewPanel::declarationLocationActivated,
            m_structModeLeftPanel,
            &StructModeLeftPanel::focusDeclarationRange);
    connect(m_structModeLeftPanel, &StructModeLeftPanel::parseStateChanged, this, [this]() {
        AppSettings::setStructDeclarationText(m_structModeLeftPanel->declarationText());
        syncStructPreviewToControls();
    });
    connect(m_structModeLeftPanel, &StructModeLeftPanel::declarationFileLoaded,
            this, [this](const QString& filePath) {
                m_externalStructSourcePaths.clear();
                AppSettings::setLastStructDefinitionFilePath(filePath);
            });
    connect(m_structModeLeftPanel->entryComboBox(), &QComboBox::currentTextChanged, this,
            [this](const QString& text) {
                AppSettings::setStructEntryName(text);
                syncStructPreviewToControls();
            });
    connect(m_structModeLeftPanel->entryCountSpinBox(), qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int count) {
                AppSettings::setStructEntryCount(count);
                syncStructPreviewToControls();
            });
    connect(m_currentByteInfoPanel->bigEndianCheckBox(), &QCheckBox::toggled, this, [this](bool checked) {
        AppSettings::setCurrentByteInfoBigEndianEnabled(checked);
        refreshCurrentByteInfoFromLastHover();
    });
    connect(m_currentByteInfoPanel->decimalModeRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (!checked) {
                    return;
                }
                AppSettings::setCurrentByteInfoNumberSystemIndex(0);
                refreshCurrentByteInfoFromLastHover();
            });
    connect(m_currentByteInfoPanel->hexModeRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (!checked) {
                    return;
                }
                AppSettings::setCurrentByteInfoNumberSystemIndex(1);
                refreshCurrentByteInfoFromLastHover();
            });
    connect(m_currentByteInfoPanel->octalModeRadioButton(), &QRadioButton::toggled, this,
            [this](bool checked) {
                if (!checked) {
                    return;
                }
                AppSettings::setCurrentByteInfoNumberSystemIndex(2);
                refreshCurrentByteInfoFromLastHover();
            });
    auto syncViewMenuChecks = [this]() {
        m_ui->actionViewScanLog->setChecked(m_scanControlsPanel->lifecycleCard()->isVisible());
        m_ui->actionViewEdits->setChecked(m_mainTabsPanel->editStack()->isVisible());
    };
    connect(m_ui->actionOpenFile, &QAction::triggered, this, [this]() { onOpenFile(); });
    connect(m_ui->actionOpenDirectory, &QAction::triggered, this, [this]() { onOpenDirectory(); });
    connect(m_ui->actionQuit, &QAction::triggered, this, [this]() { close(); });
    connect(m_ui->actionViewScanLog, &QAction::triggered, this, [this, syncViewMenuChecks](bool checked) {
        if (checked) {
            m_scanControlsPanel->showLifecycleCard();
        } else {
            m_scanControlsPanel->hideLifecycleCard();
        }
        AppSettings::setViewScanLogVisible(m_scanControlsPanel->lifecycleCard()->isVisible());
        syncViewMenuChecks();
    });
    connect(m_ui->actionViewEdits, &QAction::triggered, this, [this, syncViewMenuChecks](bool checked) {
        m_mainTabsPanel->editStack()->setVisible(checked);
        AppSettings::setViewEditsVisible(checked);
        syncViewMenuChecks();
    });
    connect(m_ui->actionAbout, &QAction::triggered, this, [this]() {
        QDialog aboutDialog(this);
        Ui::AboutDialog aboutUi;
        aboutUi.setupUi(&aboutDialog);
        aboutDialog.exec();
    });
    connect(m_scanControlsPanel->hideLifecycleCardButton(), &QToolButton::clicked, this, [this]() {
        m_scanControlsPanel->hideLifecycleCard();
        AppSettings::setViewScanLogVisible(false);
        m_ui->actionViewScanLog->setChecked(false);
    });
    m_scanControlsPanel->lifecycleCard()->setVisible(false);
    m_mainTabsPanel->editStack()->setVisible(AppSettings::viewEditsVisible());
    syncViewMenuChecks();

    connect(m_rawDataViewShellPanel->zoomOutButton(), &QToolButton::clicked, this, [this]() {
        const int next = qMax(1, m_bitmapView->zoom() - 1);
        m_bitmapView->setZoom(next);
        m_rawDataViewShellPanel->zoomLabel()->setText(QStringLiteral("%1x").arg(next));
        AppSettings::setDataViewBitmapZoom(next);
    });
    connect(m_rawDataViewShellPanel->zoomInButton(), &QToolButton::clicked, this, [this]() {
        const int next = qMin(32, m_bitmapView->zoom() + 1);
        m_bitmapView->setZoom(next);
        m_rawDataViewShellPanel->zoomLabel()->setText(QStringLiteral("%1x").arg(next));
        AppSettings::setDataViewBitmapZoom(next);
    });
    connect(m_bitmapView, &BitmapViewWidget::zoomChanged, this,
            [this](int zoom) {
                m_rawDataViewShellPanel->zoomLabel()->setText(QStringLiteral("%1x").arg(zoom));
                AppSettings::setDataViewBitmapZoom(zoom);
                scheduleSharedPreviewUpdate();
            });

    connect(&m_scanController, &ScanController::scanStarted, this, &MainWindow::onScanStarted);
    connect(&m_scanController, &ScanController::progressUpdated, this,
            &MainWindow::onProgressUpdated);
    connect(&m_scanController, &ScanController::lifecycleMessage, this,
            [this](const QString& message) {
                m_scanControlsPanel->appendLifecycleMessage(message);
            });
    connect(&m_scanController, &ScanController::resultsBatchReady, this,
            &MainWindow::onResultsBatchReady);
    connect(&m_scanController, &ScanController::scanFinished, this,
            &MainWindow::onScanFinished);
    connect(&m_scanController, &ScanController::scanError, this,
            [this](const QString& message) {
                restoreTransientScanUi();
                QMessageBox::warning(this, "Breco", message);
            });

    const QList<int> savedMainSplitterSizes = AppSettings::mainSplitterSizes();
    if (savedMainSplitterSizes.size() == 2) {
        m_ui->mainSplitter->setSizes(savedMainSplitterSizes);
    } else {
        m_ui->mainSplitter->setSizes({28, 72});
    }
    const QList<int> savedRawDataSplitterSizes =
        AppSettings::dataViewByteAndBitmapSplitterSizes();
    if (savedRawDataSplitterSizes.size() == 2) {
        m_dataViewByteAndBitmapPanel->splitter()->setSizes(savedRawDataSplitterSizes);
    } else {
        m_dataViewByteAndBitmapPanel->splitter()->setSizes({35, 65});
    }
    if (m_ui->contentSplitter != nullptr && m_ui->contentSplitter->count() == 2) {
        m_ui->contentSplitter->setHandleWidth(8);
        m_ui->contentSplitter->setChildrenCollapsible(false);
        const QList<int> savedContentSplitterSizes = AppSettings::contentSplitterSizes();
        if (savedContentSplitterSizes.size() == 2) {
            m_ui->contentSplitter->setSizes(savedContentSplitterSizes);
        } else {
            m_ui->contentSplitter->setSizes({35, 65});
        }
    }
    m_ui->mainSplitter->setHandleWidth(8);
    m_ui->mainSplitter->setStyleSheet(
        QStringLiteral("QSplitter::handle { background-color: palette(mid); }"
                       "QSplitter::handle:horizontal { border-left: 1px solid palette(dark); border-right: 1px solid palette(light); }"
                       "QSplitter::handle:vertical { border-top: 1px solid palette(dark); border-bottom: 1px solid palette(light); }"));
    if (m_ui->contentSplitter != nullptr) {
        m_ui->contentSplitter->setStyleSheet(
            QStringLiteral("QSplitter::handle { background-color: palette(mid); }"
                           "QSplitter::handle:horizontal { border-left: 1px solid palette(dark); border-right: 1px solid palette(light); }"
                           "QSplitter::handle:vertical { border-top: 1px solid palette(dark); border-bottom: 1px solid palette(light); }"));
    }
    for (int i = 1; i < m_ui->mainSplitter->count(); ++i) {
        if (QWidget* handle = m_ui->mainSplitter->handle(i); handle != nullptr) {
            handle->installEventFilter(this);
        }
    }
    connect(m_ui->mainSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        if (!m_mainSplitterHandleDragInProgress) {
            return;
        }
        const QList<int> sizes = m_ui->mainSplitter->sizes();
        if (sizes.size() == 2) {
            AppSettings::setMainSplitterSizes(sizes);
        }
    });
    connect(m_dataViewByteAndBitmapPanel->splitter(), &QSplitter::splitterMoved,
            this, [this](int, int) {
                const QList<int> sizes = m_dataViewByteAndBitmapPanel->splitter()->sizes();
                if (sizes.size() == 2) {
                    AppSettings::setDataViewByteAndBitmapSplitterSizes(sizes);
                }
            });
    connect(m_ui->contentSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        const QList<int> sizes = m_ui->contentSplitter->sizes();
        if (sizes.size() == 2) {
            AppSettings::setContentSplitterSizes(sizes);
            if (QWidget* advanced = m_scanControlsPanel->advancedSearchGroup(); advanced != nullptr) {
                if (advanced->isVisible() && sizes.at(0) <= kAdvancedSnapHideThresholdPx) {
                    advanced->setVisible(false);
                } else if (!advanced->isVisible() && sizes.at(0) >= kAdvancedSnapShowThresholdPx) {
                    advanced->setVisible(true);
                }
            }
        }
    });
    const int controlsH = m_hexControlsPanel->showAsComboBox()->sizeHint().height();
    m_rawDataViewShellPanel->zoomOutButton()->setFixedHeight(controlsH);
    m_rawDataViewShellPanel->zoomInButton()->setFixedHeight(controlsH);
    m_rawDataViewShellPanel->zoomLabel()->setFixedHeight(controlsH);
    m_rawDataViewShellPanel->zoomLabel()->setMinimumWidth(36);

    setScanButtonMode(false);

    const int hexShowAsIdx =
        qBound(0, AppSettings::hexShowAsIndex(), m_hexControlsPanel->showAsComboBox()->count() - 1);
    const bool hexBigEndian = AppSettings::hexBigEndianEnabled();
    const bool stringsOnly = AppSettings::hexStringsOnlyEnabled();
    const bool highlightResult = AppSettings::hexHighlightResultEnabled();
    const bool wrap = AppSettings::textWrapModeEnabled();
    const bool collapse = AppSettings::textCollapseEnabled();
    const bool breathe = AppSettings::textBreatheEnabled();
    const bool monospace = AppSettings::textMonospaceEnabled();
    const int newlineModeIdx =
        qBound(0, AppSettings::textNewlineModeIndex(), m_hexControlsPanel->newlineModeComboBox()->count() - 1);
    const int byteLineModeIdx =
        qBound(0, AppSettings::textByteLineModeIndex(), m_hexControlsPanel->bytesPerLineComboBox()->count() - 1);
    const int gutterWidth = qMax(48, AppSettings::textGutterWidth());
    const int gutterFormatIdx = qBound(0, AppSettings::textGutterFormatIndex(), 6);
    const bool prefillOnMerge = AppSettings::prefillOnMergeEnabled();
    const int currentByteNumberSystemIdx = qBound(0, AppSettings::currentByteInfoNumberSystemIndex(), 2);
    const bool dataViewBigEndian = AppSettings::dataViewBigEndianEnabled();
    const int dataViewTextModeIdx =
        qBound(0, AppSettings::dataViewTextModeIndex(),
               m_rawDataViewShellPanel->textInterpretationComboBox()->count() - 1);
    const int dataViewBitmapModeIdx =
        qBound(0, AppSettings::dataViewBitmapModeIndex(),
               m_rawDataViewShellPanel->bitmapModeComboBox()->count() - 1);
    const int dataViewZoom = qBound(1, AppSettings::dataViewBitmapZoom(), 32);
    const EmbeddedImageFormats supportedImageFormats = supportedEmbeddedImageFormats();
    const EmbeddedImageFormats rememberedImageFormats =
        EmbeddedImageFormats::fromInt(AppSettings::dataViewImageFormatMask(allEmbeddedImageFormats().toInt()));
    const int dataViewImageScopeIdx =
        qBound(0, AppSettings::dataViewImageScopeIndex(),
               m_dataViewImagePanel->scopeComboBox()->count() - 1);
    const int dataViewImageMaxPixelsK =
        qBound(m_dataViewImagePanel->maxPixelsKSpinBox()->minimum(),
               AppSettings::dataViewImageMaxPixelsK(),
               m_dataViewImagePanel->maxPixelsKSpinBox()->maximum());
    const int dataViewImageMaxResults =
        qBound(m_dataViewImagePanel->maxResultsSpinBox()->minimum(),
               AppSettings::dataViewImageMaxResults(),
               m_dataViewImagePanel->maxResultsSpinBox()->maximum());
    const int dataViewImageJobs =
        qBound(m_dataViewImagePanel->jobsSpinBox()->minimum(),
               AppSettings::dataViewImageJobs(threadCount),
               m_dataViewImagePanel->jobsSpinBox()->maximum());
    m_hexControlsPanel->showAsComboBox()->setCurrentIndex(hexShowAsIdx);
    m_hexControlsPanel->littleEndianRadioButton()->setChecked(!hexBigEndian);
    m_hexControlsPanel->bigEndianRadioButton()->setChecked(hexBigEndian);
    m_hexControlsPanel->stringsOnlyCheckBox()->setChecked(stringsOnly);
    m_hexControlsPanel->highlightResultCheckBox()->setChecked(highlightResult);
    m_hexControlsPanel->wrapCheckBox()->setChecked(wrap);
    m_hexControlsPanel->collapseCheckBox()->setChecked(collapse);
    m_hexControlsPanel->breatheCheckBox()->setChecked(breathe);
    m_hexControlsPanel->newlineModeComboBox()->setCurrentIndex(newlineModeIdx);
    m_hexControlsPanel->monospaceCheckBox()->setChecked(monospace);
    m_hexControlsPanel->bytesPerLineComboBox()->setCurrentIndex(byteLineModeIdx);
    m_hexControlsPanel->shiftBitsSpinBox()->setValue(0);
    setDataViewBigEndianEnabled(dataViewBigEndian);
    m_rawDataViewShellPanel->textInterpretationComboBox()->setCurrentIndex(dataViewTextModeIdx);
    m_rawDataViewShellPanel->bitmapModeComboBox()->setCurrentIndex(dataViewBitmapModeIdx);
    m_dataViewImagePanel->setSupportedFormats(supportedImageFormats);
    m_dataViewImagePanel->setSelectedFormats(rememberedImageFormats);
    m_dataViewImagePanel->setSelectedScope(static_cast<EmbeddedImageScope>(dataViewImageScopeIdx));
    m_dataViewImagePanel->maxPixelsKSpinBox()->setValue(dataViewImageMaxPixelsK);
    m_dataViewImagePanel->maxResultsSpinBox()->setValue(dataViewImageMaxResults);
    m_dataViewImagePanel->jobsSpinBox()->setValue(dataViewImageJobs);
    m_scanControlsPanel->prefillOnMergeCheckBox()->setChecked(prefillOnMerge);
    m_textView->setDisplayMode(
        hexShowAsIdx == 0 ? TextDisplayMode::ByteMode
                          : (hexShowAsIdx == 4 ? TextDisplayMode::ClassicMode
                                               : TextDisplayMode::StringMode));
    m_textView->setMode(selectedTextMode());
    m_textView->setUtf16LittleEndian(!hexBigEndian);
    m_textView->setNewlineMode(static_cast<TextNewlineMode>(newlineModeIdx));
    m_textView->setWrapMode(wrap);
    m_textView->setCollapseRunsEnabled(collapse);
    m_textView->setBreatheEnabled(breathe);
    m_textView->setStringsOnlyEnabled(stringsOnly);
    m_textView->setMonospaceEnabled(monospace);
    m_textView->setResultHighlightEnabled(highlightResult);
    m_textView->setByteLineMode(static_cast<ByteLineMode>(byteLineModeIdx));
    m_textView->setGutterWidth(gutterWidth);
    m_textView->setGutterOffsetFormat(static_cast<TextViewWidget::GutterOffsetFormat>(gutterFormatIdx));
    m_bitmapView->setTextMode(selectedDataViewTextMode());
    m_bitmapView->setUtf16LittleEndian(!dataViewBigEndian);
    m_bitmapView->setResultOverlayEnabled(highlightResult);
    m_bitmapView->setZoom(dataViewZoom);
    m_rawDataViewShellPanel->zoomLabel()->setText(QStringLiteral("%1x").arg(dataViewZoom));
    m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(dataViewBigEndian);
    m_currentByteInfoPanel->decimalModeRadioButton()->setChecked(currentByteNumberSystemIdx == 0);
    m_currentByteInfoPanel->hexModeRadioButton()->setChecked(currentByteNumberSystemIdx == 1);
    m_currentByteInfoPanel->octalModeRadioButton()->setChecked(currentByteNumberSystemIdx == 2);
    connect(m_scanControlsPanel->prefillOnMergeCheckBox(), &QCheckBox::toggled, this,
            [](bool checked) { AppSettings::setPrefillOnMergeEnabled(checked); });
    connect(m_textView, &TextViewWidget::gutterOffsetFormatChanged, this,
            [](int formatIndex) { AppSettings::setTextGutterFormatIndex(formatIndex); });
    connect(m_textView, &TextViewWidget::gutterWidthChanged, this,
            [](int width) { AppSettings::setTextGutterWidth(width); });

    updateTextModeControlVisibility();
    updateHexInfoPanel();
    clearCurrentByteInfo();

    bool rememberedStructDefinitionLoaded = false;
    if (!rememberedStructDefinitionPath.isEmpty() &&
        QFileInfo(rememberedStructDefinitionPath).isFile()) {
        rememberedStructDefinitionLoaded =
            m_structModeLeftPanel->loadDeclarationFromFile(
                rememberedStructDefinitionPath);
    }
    if (!rememberedStructDefinitionLoaded) {
        if (!rememberedStructDeclaration.isEmpty()) {
            m_structModeLeftPanel->structDeclarationEdit()->setPlainText(
                rememberedStructDeclaration);
        }
    }
    if (m_structModeLeftPanel->structureGraph().defaultEntryName().isEmpty() &&
        !rememberedStructEntryName.isEmpty()) {
        const int entryIndex =
            m_structModeLeftPanel->entryComboBox()->findText(
                rememberedStructEntryName);
        if (entryIndex >= 0) {
            m_structModeLeftPanel->entryComboBox()->setCurrentIndex(entryIndex);
        }
    }
    m_structModeLeftPanel->entryCountSpinBox()->setValue(
        qBound(m_structModeLeftPanel->entryCountSpinBox()->minimum(),
               rememberedStructEntryCount,
               m_structModeLeftPanel->entryCountSpinBox()->maximum()));

    m_resultModel.setScanTargets(&m_scanTargets);
    refreshSourceSummary();
    updateBlockSizeLabel();
    bool rememberedSourceLoaded = false;
    quint64 restoredSourceOffset = 0;
    if (!rememberedSingleFile.isEmpty()) {
        syncSourcePathInputText(rememberedSingleFile);
        const QFileInfo rememberedInfo(rememberedSingleFile);
        const SourceTargetKind rememberedKind =
            classifySourceTarget(rememberedInfo);
        const QString rememberedAbsolutePath =
            rememberedInfo.exists() ? rememberedInfo.absoluteFilePath()
                                    : rememberedSingleFile;
        if ((rememberedKind == SourceTargetKind::File ||
             rememberedKind == SourceTargetKind::BlockDevice) &&
            canOpenSourceTarget(rememberedInfo.absoluteFilePath(),
                                rememberedKind)) {
            rememberedSourceLoaded = applySourcePath(rememberedSingleFile, true);
            if (rememberedSourceLoaded && !m_scanTargets.isEmpty() &&
                m_scanTargets.first().fileSize > 0) {
                restoredSourceOffset =
                    qMin(rememberedSingleFileOffset,
                         m_scanTargets.first().fileSize - 1);
                if (restoredSourceOffset != 0) {
                    jumpToAbsoluteOffset(restoredSourceOffset);
                } else {
                    rememberActiveSingleFileOffset(0);
                }
            }
        } else {
            clearSourceSelection(false);
            syncSourcePathInputText(rememberedSingleFile);
            updateSourcePathFeedback(SourcePathFeedback::NotFound,
                                     SourceTargetKind::None,
                                     rememberedAbsolutePath);
        }
    }
    if (rememberedSourceLoaded &&
        m_structModeLeftPanel->previewEnabled() &&
        m_structModeLeftPanel->canPreview()) {
        QMetaObject::invokeMethod(
            this,
            [this, restoredSourceOffset]() {
                if (isSingleFileModeActive() &&
                    !m_textHoverBuffer.data.isEmpty() &&
                    m_structModeLeftPanel->previewEnabled() &&
                    m_structModeLeftPanel->canPreview()) {
                    createStructPreview(restoredSourceOffset);
                }
            },
            Qt::QueuedConnection);
    }
}

MainWindow::~MainWindow() { m_destroying = true; }

void MainWindow::setProtectedSourceOpenerForTests(std::unique_ptr<ProtectedSourceOpener> opener) {
    m_protectedSourceOpener = std::move(opener);
}

void MainWindow::onSourcePathTextChanged(const QString&) {
    if (m_sourcePathValidationTimer != nullptr) {
        m_sourcePathValidationTimer->start();
    }
}

void MainWindow::validateSourcePathInput() {
    if (m_scanControlsPanel == nullptr || m_scanControlsPanel->sourcePathLineEdit() == nullptr) {
        return;
    }
    previewSourcePath(m_scanControlsPanel->sourcePathLineEdit()->text());
}

bool MainWindow::selectSourcePath(const QString& path) {
    return applySourcePath(path, true);
}

bool MainWindow::applySourcePath(const QString& path, bool syncInputText) {
    if (path.isEmpty()) {
        clearSourceSelection();
        updateSourcePathFeedback(SourcePathFeedback::None, SourceTargetKind::None, path);
        return false;
    }

    const QFileInfo info(path);
    const SourceTargetKind kind = classifySourceTarget(info);
    if (kind == SourceTargetKind::None) {
        clearSourceSelection(false);
        updateSourcePathFeedback(SourcePathFeedback::NotFound, SourceTargetKind::None, path);
        return false;
    }

    const QString absolutePath = info.absoluteFilePath();
    if (!canOpenSourceTarget(absolutePath, kind)) {
        clearSourceSelection(false);
        updateSourcePathFeedback(SourcePathFeedback::PermissionDenied, kind, absolutePath);
        if (tryOpenProtectedSource(absolutePath, kind)) {
            return true;
        }
        return false;
    }

    if (syncInputText) {
        syncSourcePathInputText(absolutePath);
    }
    return kind == SourceTargetKind::Directory ? selectDirectorySource(absolutePath)
                                               : selectSingleFileSource(absolutePath);
}

void MainWindow::previewSourcePath(const QString& path) {
    clearSourceSelection(false);
    if (path.isEmpty()) {
        updateSourcePathFeedback(SourcePathFeedback::None, SourceTargetKind::None, path);
        return;
    }

    const QFileInfo info(path);
    const SourceTargetKind kind = classifySourceTarget(info);
    if (kind == SourceTargetKind::None) {
        updateSourcePathFeedback(SourcePathFeedback::NotFound, SourceTargetKind::None, path);
        return;
    }

    updateSourcePathFeedback(SourcePathFeedback::Found, kind, info.absoluteFilePath());
}

MainWindow::SourceTargetKind MainWindow::classifySourceTarget(const QFileInfo& info) const {
    if (!info.exists()) {
        return SourceTargetKind::None;
    }
    if (info.isDir()) {
        return SourceTargetKind::Directory;
    }
    if (info.isFile()) {
        return SourceTargetKind::File;
    }

#ifdef Q_OS_LINUX
    struct stat st {};
    const QByteArray pathBytes = info.absoluteFilePath().toLocal8Bit();
    if (::stat(pathBytes.constData(), &st) == 0 && S_ISBLK(st.st_mode)) {
        return SourceTargetKind::BlockDevice;
    }
#endif
    return SourceTargetKind::None;
}

bool MainWindow::canOpenSourceTarget(const QString& path, SourceTargetKind kind) const {
    if (kind == SourceTargetKind::Directory) {
        const QFileInfo info(path);
        return info.isDir() && info.isReadable() && info.isExecutable();
    }
    if (kind == SourceTargetKind::File || kind == SourceTargetKind::BlockDevice) {
        if (m_filePool.hasExternalReadFd(path)) {
            return true;
        }
        QFile file(path);
        return file.open(QIODevice::ReadOnly);
    }
    return false;
}

bool MainWindow::tryOpenProtectedSource(const QString& path, SourceTargetKind kind) {
    if (m_protectedSourceOpener == nullptr) {
        return false;
    }

    std::optional<ProtectedSourceKind> protectedKind;
    if (kind == SourceTargetKind::File) {
        protectedKind = ProtectedSourceKind::RegularFile;
    } else if (kind == SourceTargetKind::BlockDevice) {
        protectedKind = ProtectedSourceKind::BlockDevice;
    }
    if (!protectedKind.has_value() ||
        !m_protectedSourceOpener->isAvailable(path, protectedKind.value())) {
        return false;
    }

    ProtectedOpenResult result = m_protectedSourceOpener->open(path, protectedKind.value());
    if (result.status != ProtectedOpenResult::Status::Opened || result.fd < 0 ||
        result.fileSize == 0) {
        QString reason = result.errorMessage.trimmed();
        const bool looksOpaque = reason.isEmpty() ||
            reason.startsWith(QStringLiteral("org.freedesktop."), Qt::CaseInsensitive) ||
            reason.startsWith(QStringLiteral("QDBusError"), Qt::CaseInsensitive) ||
            (!reason.contains(QLatin1Char(' ')) && reason.contains(QLatin1Char('.')));
        if (looksOpaque) {
            if (reason.contains(QStringLiteral("cancel"), Qt::CaseInsensitive)) {
                reason = QStringLiteral("authentication was cancelled");
            } else if (reason.contains(QStringLiteral("authoriz"), Qt::CaseInsensitive) ||
                       reason.contains(QStringLiteral("accessdenied"), Qt::CaseInsensitive)) {
                reason = QStringLiteral("authorization was denied");
            } else if (reason.contains(QStringLiteral("serviceunknown"), Qt::CaseInsensitive) ||
                       reason.contains(QStringLiteral("noreply"), Qt::CaseInsensitive) ||
                       reason.contains(QStringLiteral("disconnected"), Qt::CaseInsensitive)) {
                reason = QStringLiteral("the system authorization service is unavailable");
            } else {
                reason = QStringLiteral("the system could not grant elevated access");
            }
        }
        statusBar()->showMessage(QStringLiteral("Could not open %1 with elevated permissions: %2")
                                     .arg(path, reason));
        return false;
    }

    if (!m_filePool.registerExternalReadFd(path, result.fd, result.fileSize)) {
#ifdef Q_OS_LINUX
        ::close(result.fd);
#endif
        return false;
    }

    if (!selectSingleFileSource(path)) {
        m_filePool.forgetExternalReadFd(path);
        return false;
    }
    return true;
}

void MainWindow::clearSourceSelection(bool clearRememberedSource) {
    m_sourceFiles.clear();
    m_sourceMode = SourceMode::None;
    m_selectedSourceDisplay.clear();
    m_scanTargets.clear();
    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    clearCurrentByteInfo();
    m_filePool.clearExternalReadFds();
    if (clearRememberedSource) {
        AppSettings::clearRememberedSingleFilePath();
        AppSettings::clearRememberedSingleFileOffset();
    }
    refreshSourceSummary();
}

void MainWindow::rememberActiveSingleFileOffset(quint64 offset) {
    if (m_sourceMode != SourceMode::SingleFile || m_scanTargets.size() != 1) {
        return;
    }
    const ScanTarget& target = m_scanTargets.first();
    if (target.filePath.isEmpty() || m_filePool.hasExternalReadFd(target.filePath)) {
        return;
    }
    if (target.fileSize > 0) {
        offset = qMin(offset, target.fileSize - 1);
    } else {
        offset = 0;
    }
    AppSettings::setRememberedSingleFilePath(target.filePath);
    AppSettings::setRememberedSingleFileOffset(offset);
}

void MainWindow::syncSourcePathInputText(const QString& path) {
    if (m_scanControlsPanel == nullptr || m_scanControlsPanel->sourcePathLineEdit() == nullptr) {
        return;
    }
    QSignalBlocker blocker(m_scanControlsPanel->sourcePathLineEdit());
    m_scanControlsPanel->sourcePathLineEdit()->setText(path);
    if (m_sourcePathValidationTimer != nullptr) {
        m_sourcePathValidationTimer->stop();
    }
}

void MainWindow::updateSourcePathFeedback(SourcePathFeedback feedback, SourceTargetKind kind,
                                          const QString& path) {
    if (m_scanControlsPanel == nullptr || m_scanControlsPanel->sourcePathLineEdit() == nullptr ||
        m_scanControlsPanel->selectedSourceTypeIconLabel() == nullptr) {
        return;
    }

    QString iconPath = QStringLiteral(":/res/none.png");
    QString iconToolTip = QStringLiteral("No source selected");
    if (kind == SourceTargetKind::File) {
        iconPath = QStringLiteral(":/res/file.png");
        iconToolTip = QStringLiteral("File");
    } else if (kind == SourceTargetKind::BlockDevice) {
        iconPath = QStringLiteral(":/res/dev.png");
        iconToolTip = QStringLiteral("Block device");
    } else if (kind == SourceTargetKind::Directory) {
        iconPath = QStringLiteral(":/res/dir.png");
        iconToolTip = QStringLiteral("Directory");
    }
    m_scanControlsPanel->selectedSourceTypeIconLabel()->setPixmap(QPixmap(iconPath));
    m_scanControlsPanel->selectedSourceTypeIconLabel()->setToolTip(iconToolTip);

    QLineEdit* sourcePathEdit = m_scanControlsPanel->sourcePathLineEdit();
    switch (feedback) {
        case SourcePathFeedback::Open:
            sourcePathEdit->setStyleSheet(QStringLiteral("QLineEdit { background-color: #c8f7c5; }"));
            writeStatusLineToStdout(QStringLiteral("Open: %1").arg(path));
            break;
        case SourcePathFeedback::PermissionDenied:
            sourcePathEdit->setStyleSheet(QStringLiteral("QLineEdit { background-color: #ffc9c9; }"));
            writeStatusLineToStdout(QStringLiteral("Permission denied: %1").arg(path));
            break;
        case SourcePathFeedback::NotFound:
            sourcePathEdit->setStyleSheet(QStringLiteral("QLineEdit { background-color: #fff3a3; }"));
            writeStatusLineToStdout(QStringLiteral("Not found: %1").arg(path));
            break;
        case SourcePathFeedback::Found:
            sourcePathEdit->setStyleSheet(QStringLiteral("QLineEdit { background-color: white; }"));
            break;
        case SourcePathFeedback::None:
        default:
            sourcePathEdit->setStyleSheet({});
            break;
    }
}

bool MainWindow::selectSingleFileSource(const QString& filePath) {
    if (filePath.isEmpty()) {
        return false;
    }
    const QFileInfo info(filePath);
    const SourceTargetKind kind = classifySourceTarget(info);
    if (kind != SourceTargetKind::File && kind != SourceTargetKind::BlockDevice) {
        return false;
    }
    if (!canOpenSourceTarget(info.absoluteFilePath(), kind)) {
        return false;
    }
    const QString absolutePath = info.absoluteFilePath();
    const bool usesExternalReadFd = m_filePool.hasExternalReadFd(absolutePath);
    if (!usesExternalReadFd) {
        m_filePool.clearExternalReadFds();
    }

    m_sourceFiles = {absolutePath};
    m_sourceMode = SourceMode::SingleFile;
    m_selectedSourceDisplay = absolutePath;
    buildScanTargets(m_sourceFiles);
    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    clearCurrentByteInfo();

    AppSettings::setLastBrowseDialogDirectory(absolutePath);
    if (usesExternalReadFd) {
        AppSettings::clearRememberedSingleFilePath();
        AppSettings::clearRememberedSingleFileOffset();
    } else {
        AppSettings::setRememberedSingleFilePath(absolutePath);
        AppSettings::setRememberedSingleFileOffset(0);
    }
    refreshSourceSummary();
    loadNotEmptyPreview();
    syncStructPreviewToControls();
    updateBufferStatusLine();
    syncSourcePathInputText(absolutePath);
    updateSourcePathFeedback(SourcePathFeedback::Open, kind, absolutePath);
    return true;
}

bool MainWindow::selectDirectorySource(const QString& dirPath) {
    if (dirPath.isEmpty()) {
        return false;
    }
    const QFileInfo info(dirPath);
    if (classifySourceTarget(info) != SourceTargetKind::Directory ||
        !canOpenSourceTarget(info.absoluteFilePath(), SourceTargetKind::Directory)) {
        return false;
    }
    const QString absolutePath = info.absoluteFilePath();
    m_filePool.clearExternalReadFds();

    m_sourceFiles = FileEnumerator::enumerateRecursive(absolutePath);
    m_sourceMode = SourceMode::Directory;
    m_selectedSourceDisplay = absolutePath;
    buildScanTargets(m_sourceFiles);
    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    clearCurrentByteInfo();

    AppSettings::setLastBrowseDialogDirectory(absolutePath);
    AppSettings::clearRememberedSingleFilePath();
    AppSettings::clearRememberedSingleFileOffset();
    refreshSourceSummary();
    updateBufferStatusLine();
    syncSourcePathInputText(absolutePath);
    updateSourcePathFeedback(SourcePathFeedback::Open, SourceTargetKind::Directory, absolutePath);
    return true;
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (m_ui != nullptr && m_ui->mainSplitter != nullptr) {
        for (int i = 1; i < m_ui->mainSplitter->count(); ++i) {
            if (watched != m_ui->mainSplitter->handle(i)) {
                continue;
            }
            if (event->type() == QEvent::MouseButtonPress) {
                m_mainSplitterHandleDragInProgress = true;
            } else if (event->type() == QEvent::MouseButtonRelease) {
                m_mainSplitterHandleDragInProgress = false;
            }
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onOpenFile() {
    const QString filePath = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select file"), AppSettings::lastBrowseDialogDirectory());
    if (filePath.isEmpty()) {
        return;
    }
    applySourcePath(filePath, true);
}

void MainWindow::onOpenDirectory() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select directory"), AppSettings::lastBrowseDialogDirectory());
    if (dir.isEmpty()) {
        return;
    }
    applySourcePath(dir, true);
}

void MainWindow::onStartScan() {
    if (m_scanController.isRunning()) {
        onStopScan();
        return;
    }
    startScan(ScanKind::Text);
}

void MainWindow::onStartStructureScan() {
    m_mainTabsPanel->activateScanTab();
    if (m_scanController.isRunning()) {
        if (m_activeScanKind == ScanKind::Structure) {
            onStopScan();
        }
        return;
    }
    startScan(ScanKind::Structure);
}

void MainWindow::startScan(ScanKind kind) {
    if (m_scanTargets.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Breco"),
                                 QStringLiteral("Select file or directory first."));
        return;
    }

    const bool structureScan = kind == ScanKind::Structure;
    const QByteArray term = structureScan
                                ? QByteArray()
                                : m_scanControlsPanel->searchTermLineEdit()->text().toUtf8();
    if (!structureScan && term.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Breco"),
                                 QStringLiteral("Enter a search term."));
        return;
    }
    if (structureScan && !m_structModeLeftPanel->canPreview()) {
        QMessageBox::information(this, QStringLiteral("Breco"),
                                 QStringLiteral("Select a valid structure entry first."));
        return;
    }
    const auto scanButtonPressedAt = std::chrono::steady_clock::now();

    m_resultModel.clear();
    clearResultBufferCacheState();
    m_targetMatchIntervals.clear();
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    onHoverLeft();
    updateBufferStatusLine();

    m_scanControlsPanel->scanProgressBar()->setValue(0);
    std::shared_ptr<const StructureGraph> scanGraph;
    std::shared_ptr<const QHash<QString, VisualizationSource>> scanExternalSources;
    QString scanEntry;
    if (structureScan) {
        scanGraph = std::make_shared<StructureGraph>(
            m_structModeLeftPanel->structureGraph());
        scanEntry = m_structModeLeftPanel->entryComboBox()->currentText();
        auto externalSources = std::make_shared<QHash<QString, VisualizationSource>>();
        if (!loadExternalStructSources(externalSources.get())) {
            return;
        }
        scanExternalSources = std::move(externalSources);
    }
    m_activeScanKind = kind;
    m_savedIgnoreCaseEnabled =
        m_scanControlsPanel->ignoreCaseCheckBox()->isEnabled();
    m_scanControlsPanel->ignoreCaseCheckBox()->setEnabled(false);
    if (structureScan) {
        m_savedStructureSearchTerm =
            m_scanControlsPanel->searchTermLineEdit()->text();
        m_savedStructureTermEnabled =
            m_scanControlsPanel->searchTermLineEdit()->isEnabled();
        m_scanControlsPanel->searchTermLineEdit()->setText(
            QStringLiteral("Structure: %1").arg(scanEntry));
        m_scanControlsPanel->searchTermLineEdit()->setEnabled(false);
    }
    m_scanController.startScan(m_scanTargets, term, effectiveBlockSizeBytes(), selectedWorkerCount(),
                               selectedTextMode(),
                               m_scanControlsPanel->ignoreCaseCheckBox()->isChecked(),
                               m_scanControlsPanel->prefillOnMergeCheckBox()->isChecked(),
                               scanButtonPressedAt, scanGraph, scanEntry,
                               scanExternalSources);
}

void MainWindow::onStopScan() { m_scanController.requestStop(); }

void MainWindow::onResultActivated(const QModelIndex& index) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: indexValid=%1 row=%2")
                           .arg(index.isValid() ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(index.row()));
    }
    if (!index.isValid()) {
        restoreDirtyBufferForRow(m_activePreviewRow);
        m_activePreviewRow = -1;
        BRECO_SELTRACE("onResultActivated: invalid index, return");
        return;
    }

    const int row = index.row();
    const MatchRecord* match = m_resultModel.matchAt(row);
    if (match == nullptr) {
        restoreDirtyBufferForRow(m_activePreviewRow);
        m_activePreviewRow = -1;
        BRECO_SELTRACE(QStringLiteral("onResultActivated: no match for row=%1, return").arg(row));
        return;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match->scanTargetIdx)
                           .arg(match->offset));
    }

    if (match->scanTargetIdx != m_activeOverlapTargetIdx) {
        const auto it = m_targetMatchIntervals.constFind(match->scanTargetIdx);
        if (it != m_targetMatchIntervals.constEnd()) {
            if (debug::selectionTraceEnabled()) {
                BRECO_SELTRACE(QStringLiteral("onResultActivated: setOverlapIntervals targetIdx=%1 intervals=%2")
                                   .arg(match->scanTargetIdx)
                                   .arg(it->size()));
            }
            m_bitmapView->setOverlapIntervals(*it);
        } else {
            BRECO_SELTRACE(QStringLiteral("onResultActivated: setOverlapIntervals targetIdx=%1 intervals=0")
                               .arg(match->scanTargetIdx));
            m_bitmapView->setOverlapIntervals({});
        }
        m_activeOverlapTargetIdx = match->scanTargetIdx;
    } else if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultActivated: overlap target unchanged targetIdx=%1")
                           .arg(match->scanTargetIdx));
    }

    BRECO_SELTRACE("onResultActivated: showMatchPreview begin");
    showMatchPreview(row, *match);
    BRECO_SELTRACE("onResultActivated: showMatchPreview end");
}

void MainWindow::onResultsBatchReady(const QVector<MatchRecord>& matches, int mergedTotal) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultsBatchReady: start matches=%1 mergedTotal=%2")
                           .arg(matches.size())
                           .arg(mergedTotal));
    }
    m_resultBuffers = m_scanController.resultBuffers();
    m_matchBufferIndices = m_scanController.matchBufferIndices();
    m_resultModel.appendBatch(matches);
    BRECO_SELTRACE("onResultsBatchReady: enforceBufferCacheBudget begin");
    const int evictions = enforceBufferCacheBudget();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onResultsBatchReady: enforceBufferCacheBudget evictions=%1")
                           .arg(evictions));
    }
    BRECO_SELTRACE("onResultsBatchReady: enforceBufferCacheBudget end");
    rebuildTargetMatchIntervals();
    m_activeOverlapTargetIdx = -1;
    updateBufferStatusLine();
    BRECO_SELTRACE("onResultsBatchReady: done");
}

void MainWindow::onProgressUpdated(const ScanProgressSnapshot& progressSnapshot) {
    const quint64 scanned = progressSnapshot.scannedBytes;
    const quint64 total = progressSnapshot.totalBytes;
    if (total > 0) {
        const int progress = static_cast<int>((static_cast<long double>(scanned) /
                                               static_cast<long double>(total)) *
                                              1000.0L);
        m_scanControlsPanel->scanProgressBar()->setValue(qBound(0, progress, 1000));
    }
    m_scanControlsPanel->scanProgressBar()->setFormat(formatScanProgress(progressSnapshot));
    if (QLabel* scannedLabel = m_scanControlsPanel->scannedValueLabel(); scannedLabel != nullptr) {
        scannedLabel->setText(humanBytes(scanned));
    }
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(total));
}

void MainWindow::onScanStarted(int fileCount, quint64 totalBytes) {
    m_scanControlsPanel->filesCountValueLabel()->setText(QString::number(fileCount));
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(totalBytes));
    setScanButtonMode(true);
    m_scanControlsPanel->clearLifecycleLog();
    m_scanControlsPanel->showLifecycleCard();
    AppSettings::setViewScanLogVisible(true);
    m_ui->actionViewScanLog->setChecked(true);
    updateBufferStatusLine();
}

void MainWindow::onScanFinished(bool stoppedByUser, bool) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("onScanFinished: stoppedByUser=%1 rows=%2")
                           .arg(stoppedByUser ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(m_resultModel.rowCount()));
    }
    restoreTransientScanUi();
    if (isSingleFileModeActive()) {
        insertSyntheticPreviewResultAtTop();
    }
    updateBufferStatusLine();
    if (m_resultModel.rowCount() > 0) {
        BRECO_SELTRACE("onScanFinished: selecting first row");
        selectResultRow(0);
    }
}

void MainWindow::onTextModeChanged(int idx) {
    m_textView->setDisplayMode(
        idx == 0 ? TextDisplayMode::ByteMode
                 : (idx == 4 ? TextDisplayMode::ClassicMode : TextDisplayMode::StringMode));
    switch (idx) {
        case 1:
            m_lastTextInterpretationMode = TextInterpretationMode::Ascii;
            m_textView->setMode(TextInterpretationMode::Ascii);
            break;
        case 2:
            m_lastTextInterpretationMode = TextInterpretationMode::Utf8;
            m_textView->setMode(TextInterpretationMode::Utf8);
            break;
        case 3:
            m_lastTextInterpretationMode = TextInterpretationMode::Utf16;
            m_textView->setMode(TextInterpretationMode::Utf16);
            m_textView->setUtf16LittleEndian(m_hexControlsPanel->littleEndianRadioButton()->isChecked());
            break;
        case 0:
        case 4:
        default:
            m_textView->setMode(TextInterpretationMode::Ascii);
            break;
    }
    AppSettings::setHexShowAsIndex(idx);
    updateHexControlsVisibility();
    scheduleSharedPreviewUpdate();
}

void MainWindow::onBitmapModeChanged(int idx) {
    switch (idx) {
        case 0:
            m_bitmapView->setMode(BitmapMode::Rgb24);
            break;
        case 1:
            m_bitmapView->setMode(BitmapMode::Grey8);
            break;
        case 2:
            m_bitmapView->setMode(BitmapMode::Grey24);
            break;
        case 3:
            m_bitmapView->setMode(BitmapMode::Rgbi256);
            break;
        case 4:
            m_bitmapView->setMode(BitmapMode::Binary);
            break;
        case 5:
            m_bitmapView->setMode(BitmapMode::Text);
            break;
        default:
            break;
    }
    AppSettings::setDataViewBitmapModeIndex(idx);
    scheduleSharedPreviewUpdate();
}

void MainWindow::onTextBackingScrollRequested(int wheelSteps, int bytesPerStepHint,
                                              int visibleBytesHint) {
    Q_UNUSED(visibleBytesHint);
    const qint64 bytesPerWheelStep =
        static_cast<qint64>(qMax(1, bytesPerStepHint)) * 4LL;
    const qint64 delta = -static_cast<qint64>(wheelSteps) * bytesPerWheelStep;
    shiftSharedCenterBy(delta);
}

quint64 MainWindow::effectiveBlockSizeBytes() const {
    quint64 blockSize = static_cast<quint64>(qMax(1, m_scanControlsPanel->blockSizeSpin()->value()));
    switch (m_scanControlsPanel->blockSizeUnitCombo()->currentIndex()) {
        case 0:
            return blockSize;
        case 1:
            return blockSize * 1024ULL;
        case 2:
            return blockSize * 1024ULL * 1024ULL;
        default:
            return blockSize;
    }
}

ShiftSettings MainWindow::currentShiftSettings() const {
    ShiftSettings shift;
    shift.amount = (m_shiftValueSpin != nullptr) ? m_shiftValueSpin->value() : 0;
    shift.unit = ShiftUnit::Bits;
    return shift;
}

TextInterpretationMode MainWindow::selectedTextMode() const {
    const int idx = m_hexControlsPanel != nullptr ? m_hexControlsPanel->showAsComboBox()->currentIndex() : 1;
    switch (idx) {
        case 2:
            return TextInterpretationMode::Utf8;
        case 3:
            return TextInterpretationMode::Utf16;
        case 0:
        default:
            return TextInterpretationMode::Ascii;
    }
}

TextInterpretationMode MainWindow::selectedDataViewTextMode() const {
    const int idx = m_rawDataViewShellPanel != nullptr
                        ? m_rawDataViewShellPanel->textInterpretationComboBox()->currentIndex()
                        : 0;
    switch (idx) {
        case 1:
            return TextInterpretationMode::Utf8;
        case 2:
            return TextInterpretationMode::Utf16;
        case 0:
        default:
            return TextInterpretationMode::Ascii;
    }
}

bool MainWindow::dataViewBigEndianEnabled() const {
    return m_dataViewBigEndian;
}

void MainWindow::setDataViewBigEndianEnabled(bool enabled) {
    m_dataViewBigEndian = enabled;
    for (DataViewShellPanel* shell :
         {m_rawDataViewShellPanel, m_structDataViewShellPanel}) {
        if (shell == nullptr) {
            continue;
        }
        const QSignalBlocker littleBlocker(shell->littleEndianRadioButton());
        const QSignalBlocker bigBlocker(shell->bigEndianRadioButton());
        shell->littleEndianRadioButton()->setChecked(!enabled);
        shell->bigEndianRadioButton()->setChecked(enabled);
    }
    if (m_currentByteInfoPanel != nullptr) {
        m_currentByteInfoPanel->bigEndianCheckBox()->setChecked(enabled);
    }
    if (m_bitmapView != nullptr) {
        m_bitmapView->setUtf16LittleEndian(!enabled);
    }
    AppSettings::setDataViewBigEndianEnabled(enabled);
    refreshCurrentByteInfoFromLastHover();
    refreshDataViewFromNavigator();
    rebuildStructVisualization();
}

void MainWindow::updateTextModeControlVisibility() {
    updateHexControlsVisibility();
}

void MainWindow::updateHexControlsVisibility() {
    if (m_hexControlsPanel == nullptr) {
        return;
    }
    const int showAsIndex = m_hexControlsPanel->showAsComboBox()->currentIndex();
    const bool byteMode = showAsIndex == 0 || showAsIndex == 4;
    const bool textMode = showAsIndex >= 1 && showAsIndex <= 3;
    m_hexControlsPanel->newlineModeComboBox()->setVisible(textMode);
    m_hexControlsPanel->stringsOnlyCheckBox()->setVisible(textMode);
    m_hexControlsPanel->wrapCheckBox()->setVisible(textMode);
    m_hexControlsPanel->collapseCheckBox()->setVisible(textMode);
    m_hexControlsPanel->breatheCheckBox()->setVisible(textMode);
    m_hexControlsPanel->monospaceCheckBox()->setVisible(textMode);
    m_hexControlsPanel->bytesPerLineComboBox()->setVisible(byteMode);
}

void MainWindow::updateHexInfoPanel() {
    if (m_destroying || m_hexControlsPanel == nullptr) {
        return;
    }

    QString fileName = QStringLiteral("-");
    QString fileSize = QStringLiteral("-");
    if (m_activePreviewRow >= 0 && m_activePreviewRow < m_resultModel.rowCount()) {
        if (const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
            match != nullptr && match->scanTargetIdx >= 0 &&
            match->scanTargetIdx < m_scanTargets.size()) {
            const ScanTarget& target = m_scanTargets.at(match->scanTargetIdx);
            fileName = QFileInfo(target.filePath).fileName();
            if (fileName.isEmpty()) {
                fileName = target.filePath;
            }
            fileSize = humanBytes(target.fileSize);
        }
    }
    m_hexControlsPanel->fileNameValueLabel()->setText(fileName);
    m_hexControlsPanel->fileSizeValueLabel()->setText(fileSize);

    const std::optional<quint64> firstVisible =
        (m_textView != nullptr) ? m_textView->firstVisibleByteOffset() : std::nullopt;
    QLineEdit* offsetEdit = m_hexControlsPanel->offsetValueEdit();
    offsetEdit->setText(firstVisible.has_value() ? formatHex(firstVisible.value(), 1)
                                                : QStringLiteral("-"));
    offsetEdit->setEnabled(firstVisible.has_value());
    offsetEdit->setModified(false);

    const std::optional<QPair<quint64, quint64>> selection =
        m_activeTextSelectionRange.has_value()
            ? m_activeTextSelectionRange
            : ((m_textView != nullptr) ? m_textView->selectionRangeOffsets() : std::nullopt);
    if (!selection.has_value()) {
        m_hexControlsPanel->selectedValueEdit()->clear();
        m_hexControlsPanel->selectedValueEdit()->setEnabled(false);
        m_hexControlsPanel->selectedValueEdit()->setModified(false);
        m_hexControlsPanel->selectToValueEdit()->clear();
        m_hexControlsPanel->selectToValueEdit()->setEnabled(false);
        m_hexControlsPanel->selectToValueEdit()->setModified(false);
        return;
    }
    const quint64 count = selection->second > selection->first
                              ? selection->second - selection->first
                              : 1ULL;
    if (count <= 1ULL) {
        m_hexControlsPanel->selectedValueEdit()->setText(formatHex(selection->first, 1));
    } else {
        m_hexControlsPanel->selectedValueEdit()->setText(
            QStringLiteral("%1 (+%2 bytes)")
                .arg(formatHex(selection->first, 1), QString::number(count)));
    }
    m_hexControlsPanel->selectedValueEdit()->setEnabled(true);
    m_hexControlsPanel->selectedValueEdit()->setModified(false);
    m_hexControlsPanel->selectToValueEdit()->setText(
        formatHex(selection->second - 1ULL, 1));
    m_hexControlsPanel->selectToValueEdit()->setEnabled(true);
    m_hexControlsPanel->selectToValueEdit()->setModified(false);
}

bool MainWindow::parseHexNavigatorOffset(const QString& text, quint64* offset) {
    if (offset == nullptr) {
        return false;
    }
    QString value = text.trimmed();
    const int suffixStart = value.indexOf(QLatin1Char(' '));
    if (suffixStart >= 0) {
        value.truncate(suffixStart);
    }
    int base = 10;
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value.remove(0, 2);
        base = 16;
    }
    if (value.isEmpty()) {
        return false;
    }
    bool valid = false;
    const quint64 parsed = value.toULongLong(&valid, base);
    if (!valid) {
        return false;
    }
    *offset = parsed;
    return true;
}

void MainWindow::commitHexNavigatorEdit(HexNavigatorField field) {
    QLineEdit* edit = nullptr;
    switch (field) {
        case HexNavigatorField::Offset:
            edit = m_hexControlsPanel->offsetValueEdit();
            break;
        case HexNavigatorField::Selected:
            edit = m_hexControlsPanel->selectedValueEdit();
            break;
        case HexNavigatorField::SelectTo:
            edit = m_hexControlsPanel->selectToValueEdit();
            break;
    }

    quint64 editedValue = 0;
    const std::optional<int> targetIndex = activePreviewTargetIndex();
    const std::optional<quint64> viewportOffset =
        m_textView != nullptr ? m_textView->firstVisibleByteOffset() : std::nullopt;
    const std::optional<QPair<quint64, quint64>> selection =
        m_activeTextSelectionRange.has_value()
            ? m_activeTextSelectionRange
            : (m_textView != nullptr ? m_textView->selectionRangeOffsets() : std::nullopt);
    if (edit == nullptr || !parseHexNavigatorOffset(edit->text(), &editedValue) ||
        !targetIndex.has_value() || !viewportOffset.has_value()) {
        updateHexInfoPanel();
        return;
    }

    const quint64 fileSize = m_scanTargets.at(targetIndex.value()).fileSize;
    if (fileSize == 0 || editedValue >= fileSize) {
        updateHexInfoPanel();
        return;
    }

    if (!selection.has_value()) {
        if (field == HexNavigatorField::Offset) {
            navigateHexView(editedValue, std::nullopt);
        } else {
            updateHexInfoPanel();
        }
        return;
    }

    const quint64 selectionStart = selection->first;
    const quint64 selectionEnd = selection->second;
    if (selectionEnd <= selectionStart || selectionStart < viewportOffset.value()) {
        updateHexInfoPanel();
        return;
    }
    const quint64 delta = selectionStart - viewportOffset.value();
    const quint64 selectionLength = selectionEnd - selectionStart;
    quint64 nextViewport = viewportOffset.value();
    quint64 nextSelectionStart = selectionStart;
    quint64 nextSelectionEnd = selectionEnd;

    switch (field) {
        case HexNavigatorField::Offset:
            if (editedValue > std::numeric_limits<quint64>::max() - delta) {
                updateHexInfoPanel();
                return;
            }
            nextViewport = editedValue;
            nextSelectionStart = editedValue + delta;
            if (nextSelectionStart >= fileSize ||
                selectionLength > fileSize - nextSelectionStart) {
                updateHexInfoPanel();
                return;
            }
            nextSelectionEnd = nextSelectionStart + selectionLength;
            break;
        case HexNavigatorField::Selected:
            if (editedValue < delta || selectionLength > fileSize - editedValue) {
                updateHexInfoPanel();
                return;
            }
            nextSelectionStart = editedValue;
            nextSelectionEnd = editedValue + selectionLength;
            nextViewport = editedValue - delta;
            break;
        case HexNavigatorField::SelectTo:
            if (editedValue < selectionStart) {
                updateHexInfoPanel();
                return;
            }
            nextSelectionEnd = editedValue + 1ULL;
            break;
    }

    navigateHexView(nextViewport,
                    qMakePair(nextSelectionStart, nextSelectionEnd));
}

bool MainWindow::navigateHexView(
    quint64 viewportOffset,
    std::optional<QPair<quint64, quint64>> selectionRange) {
    const std::optional<int> targetIndex = activePreviewTargetIndex();
    if (!targetIndex.has_value()) {
        updateHexInfoPanel();
        return false;
    }
    const quint64 fileSize = m_scanTargets.at(targetIndex.value()).fileSize;
    if (viewportOffset >= fileSize ||
        (selectionRange.has_value() &&
         (selectionRange->second <= selectionRange->first ||
          selectionRange->second > fileSize))) {
        updateHexInfoPanel();
        return false;
    }

    m_pendingHexViewportOffset = viewportOffset;
    m_pendingHexSelectionRange = selectionRange;
    const quint64 anchor = selectionRange.has_value() ? selectionRange->first : viewportOffset;
    jumpToAbsoluteOffset(anchor);
    m_pendingHexViewportOffset.reset();
    m_pendingHexSelectionRange.reset();

    const bool viewportApplied =
        m_textView != nullptr && m_textView->firstVisibleByteOffset() == viewportOffset;
    const bool selectionApplied =
        !selectionRange.has_value() || m_activeTextSelectionRange == selectionRange;
    updateHexInfoPanel();
    return viewportApplied && selectionApplied;
}

void MainWindow::refreshDataViewFromNavigator() {
    if (m_textView == nullptr || m_currentByteInfoPanel == nullptr) {
        return;
    }
    std::optional<quint64> anchor;
    if (m_activeTextSelectionRange.has_value()) {
        anchor = m_activeTextSelectionRange->first;
    } else {
        anchor = m_textView->firstVisibleByteOffset();
    }

    if (anchor.has_value() && anchor.value() >= m_textHoverBuffer.baseOffset &&
        anchor.value() < m_textHoverBuffer.baseOffset +
                             static_cast<quint64>(m_textHoverBuffer.data.size())) {
        updateCurrentByteInfoFromHover(m_textHoverBuffer, anchor.value());
    } else if (!m_lastHoverAbsoluteOffset.has_value()) {
        clearCurrentByteInfo();
    }
}

void MainWindow::setScanButtonMode(bool running) {
    m_scanControlsPanel->startScanButton()->setText(running ? QStringLiteral("Stop")
                                                            : QStringLiteral("Scan"));
    m_structModeLeftPanel->setScanState(
        running, running && m_activeScanKind == ScanKind::Structure);
}

void MainWindow::restoreTransientScanUi() {
    if (m_activeScanKind == ScanKind::Structure) {
        m_scanControlsPanel->searchTermLineEdit()->setText(
            m_savedStructureSearchTerm);
        m_scanControlsPanel->searchTermLineEdit()->setEnabled(
            m_savedStructureTermEnabled);
    }
    if (m_activeScanKind != ScanKind::None) {
        m_scanControlsPanel->ignoreCaseCheckBox()->setEnabled(
            m_savedIgnoreCaseEnabled);
    }
    m_activeScanKind = ScanKind::None;
    setScanButtonMode(false);
}

void MainWindow::updateBlockSizeLabel() {
    if (m_sourceMode == SourceMode::SingleFile && m_scanTargets.size() == 1) {
        const quint64 bytes = m_scanTargets.first().fileSize;
        const quint64 block = effectiveBlockSizeBytes();
        const quint64 blockCount = (bytes + block - 1) / block;
        m_scanControlsPanel->blockSizeLabel()->setText(
            QStringLiteral("Block size (%1 blocks)").arg(QString::number(blockCount)));
        return;
    }
    m_scanControlsPanel->blockSizeLabel()->setText(QStringLiteral("Block size"));
}

int MainWindow::selectedWorkerCount() const {
    const QVariant workerData = m_scanControlsPanel->workerCountCombo()->currentData();
    if (workerData.isValid()) {
        return qMax(1, workerData.toInt());
    }
    return qMax(1, m_scanControlsPanel->workerCountCombo()->currentText().toInt());
}

QString MainWindow::humanBytes(quint64 bytes) const {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    long double value = static_cast<long double>(bytes);
    int unitIdx = 0;
    while (value >= 1024.0L && unitIdx < 4) {
        value /= 1024.0L;
        ++unitIdx;
    }
    return QStringLiteral("%1 %2").arg(QString::number(static_cast<double>(value), 'f', 2), units[unitIdx]);
}

void MainWindow::refreshSourceSummary() {
    m_scanControlsPanel->filesCountValueLabel()->setText(QString::number(m_scanTargets.size()));
    m_scanControlsPanel->searchSpaceValueLabel()->setText(humanBytes(currentSelectedSourceBytes()));
    updateBlockSizeLabel();
    updateHexInfoPanel();
}

void MainWindow::buildScanTargets(const QVector<QString>& filePaths) {
    m_scanTargets.clear();
    for (const QString& path : filePaths) {
        const QFileInfo info(path);
        const QString absolutePath = info.absoluteFilePath();
        const quint64 size =
            m_filePool.externalReadSize(absolutePath).value_or(fileSizeWithBlockDeviceSupport(info));
        if (size == 0) {
            continue;
        }
        ScanTarget target;
        target.filePath = absolutePath;
        target.fileSize = size;
        m_scanTargets.push_back(target);
    }
    m_resultModel.setScanTargets(&m_scanTargets);
}

quint64 MainWindow::currentSelectedSourceBytes() const {
    quint64 total = 0;
    for (const ScanTarget& target : m_scanTargets) {
        total += target.fileSize;
    }
    return total;
}

void MainWindow::selectResultRow(int row) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("selectResultRow: requestedRow=%1 rowCount=%2")
                           .arg(row)
                           .arg(m_resultModel.rowCount()));
    }
    if (row < 0 || row >= m_resultModel.rowCount()) {
        BRECO_SELTRACE("selectResultRow: out of range, return");
        return;
    }
    QTableView* resultsTable = m_resultsPanel->resultsTableView();
    const QModelIndex idx = m_resultModel.index(row, 0);
    const QModelIndex previous = resultsTable->currentIndex();
    resultsTable->setCurrentIndex(idx);
    resultsTable->selectRow(row);
    if (!previous.isValid() || previous.row() != row) {
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral("selectResultRow: selection model emitted change (previousRow=%1), return")
                               .arg(previous.row()));
        }
        return;
    }
    BRECO_SELTRACE("selectResultRow: current row unchanged, invoking onResultActivated directly");
    onResultActivated(idx);
}

QString MainWindow::filePathForTarget(int targetIdx) const {
    if (targetIdx < 0 || targetIdx >= m_scanTargets.size()) {
        return {};
    }
    return m_scanTargets.at(targetIdx).filePath;
}

QVector<int> MainWindow::bufferReferenceCounts() const {
    QVector<int> counts;
    counts.fill(0, m_resultBuffers.size());
    for (const int bufferIndex : m_matchBufferIndices) {
        if (bufferIndex >= 0 && bufferIndex < counts.size()) {
            ++counts[bufferIndex];
        }
    }
    return counts;
}

quint64 MainWindow::totalResidentBufferBytes(const QVector<int>& refCounts) const {
    quint64 total = 0;
    const int count = qMin(refCounts.size(), m_resultBuffers.size());
    for (int i = 0; i < count; ++i) {
        if (refCounts.at(i) <= 0) {
            continue;
        }
        total += static_cast<quint64>(qMax(0, m_resultBuffers.at(i).bytes.size()));
    }
    return total;
}

bool MainWindow::evictOneBufferLargestFirstLeastUsed(const QSet<int>& protectedBufferIndices) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: start buffers=%1 protected=%2")
                           .arg(m_resultBuffers.size())
                           .arg(protectedBufferIndices.size()));
    }
    if (m_resultBuffers.isEmpty() || m_matchBufferIndices.isEmpty()) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: no buffers or mapping, return false");
        return false;
    }

    const QVector<int> refCounts = bufferReferenceCounts();
    int candidate = -1;
    quint64 candidateSize = 0;
    int candidateRefs = std::numeric_limits<int>::max();
    for (int i = 0; i < m_resultBuffers.size() && i < refCounts.size(); ++i) {
        if (protectedBufferIndices.contains(i)) {
            continue;
        }
        if (refCounts.at(i) <= 0) {
            continue;
        }
        const quint64 size = static_cast<quint64>(qMax(0, m_resultBuffers.at(i).bytes.size()));
        if (size == 0) {
            continue;
        }
        const int refs = refCounts.at(i);
        if (candidate < 0 || size > candidateSize ||
            (size == candidateSize && refs < candidateRefs)) {
            candidate = i;
            candidateSize = size;
            candidateRefs = refs;
        }
    }
    if (candidate < 0) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: no eviction candidate");
        return false;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: candidate=%1 size=%2 refs=%3")
                           .arg(candidate)
                           .arg(candidateSize)
                           .arg(candidateRefs));
    }

    QVector<int> affectedRows;
    affectedRows.reserve(candidateRefs);
    for (int row = 0; row < m_matchBufferIndices.size(); ++row) {
        if (m_matchBufferIndices.at(row) == candidate) {
            affectedRows.push_back(row);
        }
    }
    if (affectedRows.isEmpty()) {
        std::cout << "[cache] evicting buffer#" << candidate
                  << " size=" << candidateSize
                  << " refs=" << candidateRefs
                  << " action=clear-unreferenced-by-rows" << std::endl;
        m_resultBuffers[candidate].bytes.clear();
        BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: cleared unreferenced candidate=%1")
                           .arg(candidate));
        return true;
    }

    const int firstRow = affectedRows.first();
    const MatchRecord* firstMatch = m_resultModel.matchAt(firstRow);
    if (firstMatch == nullptr) {
        BRECO_SELTRACE("evictOneBufferLargestFirstLeastUsed: firstMatch missing, return false");
        return false;
    }
    std::cout << "[cache] evicting buffer#" << candidate
              << " size=" << candidateSize
              << " refs=" << candidateRefs
              << " affectedRows=" << affectedRows.size()
              << " action=replace-with-zero-length-placeholders" << std::endl;
    m_resultBuffers[candidate] = makeEvictedPlaceholderBuffer(*firstMatch);
    m_matchBufferIndices[firstRow] = candidate;

    for (int i = 1; i < affectedRows.size(); ++i) {
        const int row = affectedRows.at(i);
        const MatchRecord* match = m_resultModel.matchAt(row);
        if (match == nullptr) {
            continue;
        }
        const int newIndex = m_resultBuffers.size();
        m_resultBuffers.push_back(makeEvictedPlaceholderBuffer(*match));
        m_matchBufferIndices[row] = newIndex;
    }
    BRECO_SELTRACE(QStringLiteral("evictOneBufferLargestFirstLeastUsed: replaced candidate=%1 with %2 placeholder rows")
                       .arg(candidate)
                       .arg(affectedRows.size()));
    return true;
}

int MainWindow::enforceBufferCacheBudget(const QSet<int>& protectedBufferIndices) {
    const bool traceEnabled = debug::selectionTraceEnabled();
    if (traceEnabled) {
        const QVector<int> refCounts = bufferReferenceCounts();
        BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: start resident=%1 budget=%2 protected=%3")
                           .arg(totalResidentBufferBytes(refCounts))
                           .arg(kResultBufferCacheBudgetBytes)
                           .arg(protectedBufferIndices.size()));
    }
    int evictions = 0;
    while (true) {
        const QVector<int> refCounts = bufferReferenceCounts();
        const quint64 resident = totalResidentBufferBytes(refCounts);
        if (resident <= kResultBufferCacheBudgetBytes) {
            if (traceEnabled) {
                BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: within budget resident=%1 evictions=%2")
                                   .arg(resident)
                                   .arg(evictions));
            }
            break;
        }
        if (traceEnabled) {
            BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: over budget resident=%1 evictions=%2")
                               .arg(resident)
                               .arg(evictions));
        }
        if (!evictOneBufferLargestFirstLeastUsed(protectedBufferIndices)) {
            if (traceEnabled) {
                BRECO_SELTRACE(QStringLiteral("enforceBufferCacheBudget: eviction unavailable at resident=%1")
                                   .arg(resident));
            }
            break;
        }
        ++evictions;
    }
    return evictions;
}

bool MainWindow::ensureRowBufferLoaded(int row, const MatchRecord& match,
                                       const QSet<int>& protectedBufferIndices) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    if (row < 0 || row >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("ensureRowBufferLoaded: row outside mapping, return false");
        return false;
    }
    const int bufferIndex = m_matchBufferIndices.at(row);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: invalid bufferIndex=%1, return false")
                           .arg(bufferIndex));
        return false;
    }
    if (!m_resultBuffers.at(bufferIndex).bytes.isEmpty()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: bufferIndex=%1 already resident").arg(bufferIndex));
        return true;
    }

    std::cout << "[cache] on-demand load start t+"
              << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
              << " buffer#" << bufferIndex
              << " targetIdx=" << match.scanTargetIdx
              << " matchOffset=" << match.offset << std::endl;

    const quint64 loadStartUs = debug::selectionTraceElapsedUs();
    const ResultBuffer loaded = loadEvictedWindowForMatch(match);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: loadEvictedWindowForMatch took=%1us size=%2")
                           .arg(debug::selectionTraceElapsedUs() - loadStartUs)
                           .arg(loaded.bytes.size()));
    }
    const quint64 loadElapsedUs = debug::selectionTraceElapsedUs() - loadStartUs;
    const quint64 loadElapsedMs = loadElapsedUs / 1000ULL;
    if (loaded.bytes.isEmpty()) {
        std::cout << "[cache] on-demand load failed t+"
                  << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
                  << " buffer#" << bufferIndex
                  << " elapsedMs=" << loadElapsedMs << std::endl;
        BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: on-demand load failed row=%1 bufferIndex=%2")
                           .arg(row)
                           .arg(bufferIndex));
        return false;
    }
    m_resultBuffers[bufferIndex] = loaded;
    std::cout << "[cache] on-demand load finished t+"
              << (debug::selectionTraceElapsedUs() / 1000ULL) << "ms: row=" << row
              << " buffer#" << bufferIndex
              << " start=" << loaded.fileOffset
              << " size=" << loaded.bytes.size()
              << " elapsedMs=" << loadElapsedMs << std::endl;

    QSet<int> protectedSet = protectedBufferIndices;
    protectedSet.insert(bufferIndex);
    BRECO_SELTRACE("ensureRowBufferLoaded: enforceBufferCacheBudget begin");
    enforceBufferCacheBudget(protectedSet);
    BRECO_SELTRACE("ensureRowBufferLoaded: enforceBufferCacheBudget end");
    BRECO_SELTRACE(QStringLiteral("ensureRowBufferLoaded: finished row=%1 bufferIndex=%2")
                       .arg(row)
                       .arg(bufferIndex));
    return !m_resultBuffers.at(bufferIndex).bytes.isEmpty();
}

ResultBuffer MainWindow::makeEvictedPlaceholderBuffer(const MatchRecord& match) const {
    ResultBuffer placeholder;
    placeholder.scanTargetIdx = match.scanTargetIdx;
    placeholder.fileOffset = match.offset;
    placeholder.bytes.clear();
    placeholder.dirty = false;
    return placeholder;
}

ResultBuffer MainWindow::loadEvictedWindowForMatch(const MatchRecord& match) const {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("loadEvictedWindowForMatch: start targetIdx=%1 offset=%2")
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    ResultBuffer out;
    if (match.scanTargetIdx < 0 || match.scanTargetIdx >= m_scanTargets.size()) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: invalid target index, return empty");
        return out;
    }

    const ScanTarget& target = m_scanTargets.at(match.scanTargetIdx);
    if (target.filePath.isEmpty() || target.fileSize == 0) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: empty target path or size, return empty");
        return out;
    }

    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const quint64 start =
        (match.offset > kEvictedWindowRadiusBytes) ? (match.offset - kEvictedWindowRadiusBytes) : 0;
    const quint64 end =
        qMin(target.fileSize, match.offset + termLen + kEvictedWindowRadiusBytes);
    if (end <= start) {
        BRECO_SELTRACE(QStringLiteral("loadEvictedWindowForMatch: invalid range start=%1 end=%2")
                           .arg(start)
                           .arg(end));
        return out;
    }
    const quint64 size = end - start;

    const ShiftReadPlan plan =
        ShiftTransform::makeReadPlan(start, size, target.fileSize, ShiftSettings{});
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "loadEvictedWindowForMatch: outputStart=%1 outputSize=%2 readStart=%3 readSize=%4")
                           .arg(start)
                           .arg(size)
                           .arg(plan.readStart)
                           .arg(plan.readSize));
    }
    const quint64 loadStartUs = debug::selectionTraceElapsedUs();
    const auto rawWindow =
        m_windowLoader.loadRawWindow(target.filePath, target.fileSize, start, size, ShiftSettings{});
    if (debug::selectionTraceEnabled()) {
        const quint64 elapsed = debug::selectionTraceElapsedUs() - loadStartUs;
        BRECO_SELTRACE(QStringLiteral(
                           "loadEvictedWindowForMatch: loadTransformedWindow done elapsed=%1us hasValue=%2 size=%3")
                           .arg(elapsed)
                           .arg(rawWindow.has_value() ? QStringLiteral("true")
                                                        : QStringLiteral("false"))
                           .arg(rawWindow.has_value() ? rawWindow->bytes.size() : -1));
    }
    if (!rawWindow.has_value()) {
        BRECO_SELTRACE("loadEvictedWindowForMatch: loadTransformedWindow failed, return empty");
        return out;
    }

    out.scanTargetIdx = match.scanTargetIdx;
    out.fileOffset = start;
    out.bytes = rawWindow->bytes;
    out.dirty = false;
    Q_UNUSED(plan);
    BRECO_SELTRACE("loadEvictedWindowForMatch: done");
    return out;
}

bool MainWindow::restoreBufferRawIfDirty(int bufferIndex) {
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return false;
    }
    ResultBuffer& buffer = m_resultBuffers[bufferIndex];
    if (!buffer.dirty) {
        return true;
    }
    if (buffer.scanTargetIdx < 0 || buffer.scanTargetIdx >= m_scanTargets.size()) {
        buffer.dirty = false;
        return false;
    }
    const ScanTarget& target = m_scanTargets.at(buffer.scanTargetIdx);
    if (target.filePath.isEmpty() || target.fileSize == 0 || buffer.bytes.isEmpty()) {
        buffer.dirty = false;
        return false;
    }
    const quint64 size = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const auto rawWindow = m_windowLoader.loadRawWindow(
        target.filePath, target.fileSize, buffer.fileOffset, size, ShiftSettings{});
    if (!rawWindow.has_value()) {
        return false;
    }
    buffer.bytes = rawWindow->bytes;
    buffer.dirty = false;
    return true;
}

void MainWindow::restoreDirtyBufferForRow(int row) {
    if (row < 0 || row >= m_matchBufferIndices.size()) {
        return;
    }
    const int bufferIndex = m_matchBufferIndices.at(row);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return;
    }
    restoreBufferRawIfDirty(bufferIndex);
}

void MainWindow::applyShiftToBufferIfEnabled(int bufferIndex) {
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return;
    }
    ResultBuffer& buffer = m_resultBuffers[bufferIndex];
    const ShiftSettings shift = currentShiftSettings();
    if (shift.amount == 0 || buffer.bytes.isEmpty()) {
        return;
    }
    if (buffer.scanTargetIdx < 0 || buffer.scanTargetIdx >= m_scanTargets.size()) {
        return;
    }
    const quint64 size = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    if (size == 0) {
        return;
    }
    const quint64 fileSize = m_scanTargets.at(buffer.scanTargetIdx).fileSize;
    buffer.bytes = ShiftTransform::transformWindow(buffer.bytes, buffer.fileOffset, buffer.fileOffset, size,
                                                   fileSize, shift);
    buffer.dirty = true;
}

bool MainWindow::expandActivePreviewBuffer(int direction) {
    if (direction == 0 || m_activePreviewRow < 0 || m_activePreviewRow >= m_resultModel.rowCount()) {
        return false;
    }
    const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
    if (match == nullptr || match->scanTargetIdx < 0 || match->scanTargetIdx >= m_scanTargets.size()) {
        return false;
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_matchBufferIndices.size()) {
        return false;
    }
    const int bufferIndex = m_matchBufferIndices.at(m_activePreviewRow);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return false;
    }

    if (m_resultBuffers.at(bufferIndex).bytes.isEmpty()) {
        if (!ensureRowBufferLoaded(m_activePreviewRow, *match)) {
            return false;
        }
    }
    ResultBuffer& buffer = m_resultBuffers[bufferIndex];
    if (buffer.bytes.isEmpty()) {
        return false;
    }

    const ScanTarget& target = m_scanTargets.at(match->scanTargetIdx);
    const quint64 currentStart = buffer.fileOffset;
    const quint64 currentEndExclusive =
        currentStart + static_cast<quint64>(qMax(0, buffer.bytes.size()));
    if (currentEndExclusive <= currentStart || target.fileSize == 0) {
        return false;
    }

    quint64 nextStart = currentStart;
    quint64 nextEndExclusive = qMin(currentEndExclusive, target.fileSize);
    if (direction < 0) {
        const quint64 delta = qMin(kTextChunkExpandStepBytes, nextStart);
        nextStart -= delta;
    } else {
        nextEndExclusive = qMin(target.fileSize, nextEndExclusive + kTextChunkExpandStepBytes);
    }

    if (nextStart == currentStart && nextEndExclusive == currentEndExclusive) {
        return false;
    }
    if (nextEndExclusive <= nextStart) {
        return false;
    }

    const auto rawWindow = m_windowLoader.loadRawWindow(
        target.filePath, target.fileSize, nextStart, nextEndExclusive - nextStart, ShiftSettings{});
    if (!rawWindow.has_value()) {
        return false;
    }

    buffer.scanTargetIdx = match->scanTargetIdx;
    buffer.fileOffset = nextStart;
    buffer.bytes = rawWindow->bytes;
    buffer.dirty = false;
    applyShiftToBufferIfEnabled(bufferIndex);
    return !buffer.bytes.isEmpty();
}

void MainWindow::clearResultBufferCacheState() {
    clearStructSourceHighlight();
    clearStructPreview();
    cancelImageScan();
    if (m_dataViewImagePanel != nullptr) {
        m_dataViewImagePanel->clearResults();
        m_dataViewImagePanel->resetProgress();
        m_dataViewImagePanel->setStatusText(QStringLiteral("No image scan has run."));
    }
    m_resultBuffers.clear();
    m_matchBufferIndices.clear();
    m_activePreviewRow = -1;
    m_activeOverlapTargetIdx = -1;
    m_sharedCenterOffset = 0;
    m_pendingCenterOffset.reset();
    m_pendingHexViewportOffset.reset();
    m_pendingHexSelectionRange.reset();
    m_previewUpdateScheduled = false;
    m_textExpandBeforeBytes = 0;
    m_textExpandAfterBytes = 0;
    m_pendingPageDirection = 0;
    m_pendingPageEdgeOffset.reset();
    m_pendingFileEdgeNavigation = 0;
    m_textScrollDragInProgress = false;
    m_pendingPreviewAfterTextScrollDrag = false;
    m_lastSyntheticBufferIndex = -1;
    m_textHoverBuffer = {};
    m_bitmapHoverBuffer = {};
    clearCurrentByteInfo();
    m_activeTextSelectionRange.reset();
    updateHexInfoPanel();
}

void MainWindow::rebuildTargetMatchIntervals() {
    m_targetMatchIntervals.clear();
    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const QVector<MatchRecord>& matches = m_resultModel.allMatches();
    for (const MatchRecord& match : matches) {
        const quint64 start = match.offset;
        const quint64 end = start + qMax<quint64>(1, termLen);
        m_targetMatchIntervals[match.scanTargetIdx].push_back(qMakePair(start, end));
    }
}

std::optional<unsigned char> MainWindow::previousByteBeforeViewport(const ResultBuffer& buffer,
                                                                    quint64 viewportStart) const {
    if (buffer.bytes.isEmpty() || viewportStart <= buffer.fileOffset) {
        return std::nullopt;
    }

    const quint64 rel = viewportStart - buffer.fileOffset;
    if (rel == 0 || rel > static_cast<quint64>(buffer.bytes.size())) {
        return std::nullopt;
    }
    return static_cast<unsigned char>(buffer.bytes.at(static_cast<int>(rel - 1)));
}

quint64 MainWindow::clampViewportStart(const ResultBuffer& buffer, quint64 desiredStart,
                                       quint64 windowBytes) const {
    if (buffer.bytes.isEmpty()) {
        return buffer.fileOffset;
    }

    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const quint64 clampedWindow = qMin(windowBytes, bufferSize);
    if (clampedWindow == 0) {
        return bufferStart;
    }

    const quint64 maxStart = bufferStart + (bufferSize - clampedWindow);
    return qBound(bufferStart, desiredStart, maxStart);
}

MainWindow::ViewportWindow MainWindow::viewportFromStart(const ResultBuffer& buffer,
                                                         quint64 startOffset,
                                                         quint64 windowBytes) const {
    ViewportWindow window;
    if (buffer.bytes.isEmpty() || windowBytes == 0) {
        return window;
    }

    const quint64 clampedStart = clampViewportStart(buffer, startOffset, windowBytes);
    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const quint64 clampedWindow = qMin(windowBytes, bufferSize);

    const int relStart = static_cast<int>(clampedStart - bufferStart);
    const int len = static_cast<int>(clampedWindow);
    window.start = clampedStart;
    window.data = buffer.bytes.mid(relStart, len);
    return window;
}

quint64 MainWindow::textViewportByteWindow() const {
    if (m_textView == nullptr) {
        return 1;
    }
    return static_cast<quint64>(qMax(1, m_textView->recommendedViewportByteCount()));
}

quint64 MainWindow::bitmapViewportByteWindow() const {
    if (m_bitmapView == nullptr) {
        return 1;
    }
    return qMax<quint64>(1, m_bitmapView->viewportByteCapacity());
}

MainWindow::ByteSpan MainWindow::centeredSpan(const ResultBuffer& buffer, quint64 centerOffset,
                                              quint64 desiredWindowBytes) const {
    ByteSpan span;
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    if (bufferSize == 0) {
        span.start = buffer.fileOffset;
        span.size = 0;
        return span;
    }

    const quint64 bufferStart = buffer.fileOffset;
    const quint64 bufferEnd = bufferStart + bufferSize;
    const quint64 clampedCenter = qBound(bufferStart, centerOffset, bufferEnd - 1);
    const quint64 windowSize = qMax<quint64>(1, qMin(desiredWindowBytes, bufferSize));
    const quint64 before = windowSize / 2;

    quint64 start = (clampedCenter > before) ? (clampedCenter - before) : 0;
    if (start < bufferStart) {
        start = bufferStart;
    }
    const quint64 maxStart = bufferStart + (bufferSize - windowSize);
    if (start > maxStart) {
        start = maxStart;
    }

    span.start = start;
    span.size = windowSize;
    return span;
}

void MainWindow::requestSharedCenter(quint64 absoluteOffset) {
    rememberActiveSingleFileOffset(absoluteOffset);
    m_pendingCenterOffset = absoluteOffset;
    scheduleSharedPreviewUpdate();
}

void MainWindow::jumpToAbsoluteOffset(quint64 absoluteOffset) {
    if (m_activePreviewRow < 0 && isSingleFileModeActive()) {
        loadNotEmptyPreview();
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_resultModel.rowCount() ||
        m_activePreviewRow >= m_matchBufferIndices.size()) {
        return;
    }
    const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
    if (match == nullptr || match->scanTargetIdx < 0 || match->scanTargetIdx >= m_scanTargets.size()) {
        return;
    }
    const ScanTarget& target = m_scanTargets.at(match->scanTargetIdx);
    if (absoluteOffset >= target.fileSize) {
        return;
    }
    const int bufferIndex = m_matchBufferIndices.at(m_activePreviewRow);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return;
    }
    if (!restoreBufferRawIfDirty(bufferIndex)) {
        return;
    }
    ResultBuffer& buffer = m_resultBuffers[bufferIndex];
    const quint64 bufferSize = static_cast<quint64>(qMax(0, buffer.bytes.size()));
    const bool inResidentBuffer =
        bufferSize > 0 && absoluteOffset >= buffer.fileOffset &&
        absoluteOffset < buffer.fileOffset + bufferSize;
    if (!inResidentBuffer) {
        const quint64 desiredWindow =
            qMin(target.fileSize, qMax(kNotEmptyInitialBytes,
                                       qMax(textViewportByteWindow(), bitmapViewportByteWindow())));
        const quint64 halfWindow = desiredWindow / 2ULL;
        quint64 loadStart = absoluteOffset > halfWindow ? absoluteOffset - halfWindow : 0ULL;
        if (loadStart + desiredWindow > target.fileSize) {
            loadStart = target.fileSize > desiredWindow ? target.fileSize - desiredWindow : 0ULL;
        }
        const auto rawWindow =
            m_windowLoader.loadRawWindow(target.filePath, target.fileSize,
                                         loadStart, desiredWindow, ShiftSettings{});
        if (!rawWindow.has_value() || rawWindow->bytes.isEmpty()) {
            return;
        }
        buffer.scanTargetIdx = match->scanTargetIdx;
        buffer.fileOffset = loadStart;
        buffer.bytes = rawWindow->bytes;
        buffer.dirty = false;
        applyShiftToBufferIfEnabled(bufferIndex);
    } else {
        applyShiftToBufferIfEnabled(bufferIndex);
    }

    m_textExpandBeforeBytes = 0;
    m_textExpandAfterBytes = 0;
    m_pendingPageDirection = 0;
    m_pendingPageEdgeOffset.reset();
    m_pendingFileEdgeNavigation = 0;
    m_sharedCenterOffset = absoluteOffset;
    m_pendingCenterOffset = absoluteOffset;
    rememberActiveSingleFileOffset(absoluteOffset);
    updateSharedPreviewNow();
}

void MainWindow::shiftSharedCenterBy(qint64 signedBytes) {
    if (m_activePreviewRow < 0) {
        return;
    }
    const quint64 currentCenter =
        m_pendingCenterOffset.has_value() ? m_pendingCenterOffset.value() : m_sharedCenterOffset;
    quint64 nextCenter = currentCenter;
    if (signedBytes < 0) {
        const quint64 delta = static_cast<quint64>(-signedBytes);
        nextCenter = (delta >= nextCenter) ? 0 : (nextCenter - delta);
    } else if (signedBytes > 0) {
        nextCenter += static_cast<quint64>(signedBytes);
    }
    requestSharedCenter(nextCenter);
}

void MainWindow::requestSharedCenterFromTextScrollPosition(int sliderValue, int sliderMaximum) {
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_resultModel.rowCount()) {
        return;
    }
    const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
    if (match == nullptr) {
        return;
    }
    if (!ensureRowBufferLoaded(m_activePreviewRow, *match)) {
        return;
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_matchBufferIndices.size()) {
        return;
    }
    const int bufferIndex = m_matchBufferIndices.at(m_activePreviewRow);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        return;
    }
    const ResultBuffer& backing = m_resultBuffers.at(bufferIndex);
    const quint64 backingSize = static_cast<quint64>(qMax(0, backing.bytes.size()));
    if (backingSize == 0) {
        return;
    }

    const quint64 leastCapacity =
        qMax<quint64>(1, qMin(textViewportByteWindow(), bitmapViewportByteWindow()));
    const quint64 effectiveWindow = qMin(leastCapacity, backingSize);
    quint64 minCenter = backing.fileOffset + (effectiveWindow / 2ULL);
    quint64 maxCenter = backing.fileOffset + backingSize - 1ULL - ((effectiveWindow - 1ULL) / 2ULL);
    if (minCenter > maxCenter) {
        minCenter = maxCenter = backing.fileOffset + (backingSize / 2ULL);
    }

    const long double ratio =
        (sliderMaximum > 0)
            ? (static_cast<long double>(qBound(0, sliderValue, sliderMaximum)) /
               static_cast<long double>(sliderMaximum))
            : 0.0L;
    const quint64 centerRange = (maxCenter >= minCenter) ? (maxCenter - minCenter) : 0ULL;
    const quint64 center =
        minCenter + static_cast<quint64>(ratio * static_cast<long double>(centerRange));
    requestSharedCenter(center);
}

void MainWindow::scheduleSharedPreviewUpdate() {
    if (m_textScrollDragInProgress) {
        m_pendingPreviewAfterTextScrollDrag = true;
        return;
    }
    if (m_previewUpdateScheduled) {
        return;
    }
    m_previewUpdateScheduled = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_previewUpdateScheduled = false;
            updateSharedPreviewNow();
        },
        Qt::QueuedConnection);
}

void MainWindow::updateSharedPreviewNow() {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: start activeRow=%1 rows=%2")
                           .arg(m_activePreviewRow)
                           .arg(m_resultModel.rowCount()));
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_resultModel.rowCount()) {
        BRECO_SELTRACE("updateSharedPreviewNow: active row invalid, return");
        return;
    }
    const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
    if (match == nullptr) {
        BRECO_SELTRACE("updateSharedPreviewNow: match not found, return");
        return;
    }
    const quint64 ensureStartUs = debug::selectionTraceElapsedUs();
    const bool hasBuffer = ensureRowBufferLoaded(m_activePreviewRow, *match);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: ensureRowBufferLoaded ok=%1 elapsed=%2us")
                           .arg(hasBuffer ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(debug::selectionTraceElapsedUs() - ensureStartUs));
    }
    if (!hasBuffer) {
        return;
    }
    if (m_activePreviewRow < 0 || m_activePreviewRow >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("updateSharedPreviewNow: active row no longer mapped, return");
        return;
    }
    const int bufferIndex = m_matchBufferIndices.at(m_activePreviewRow);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: invalid bufferIndex=%1, return").arg(bufferIndex));
        return;
    }

    ResultBuffer* backingPtr = &m_resultBuffers[bufferIndex];
    const int fileEdgeNavigation = m_pendingFileEdgeNavigation;
    m_pendingFileEdgeNavigation = 0;
    const int pageDirection = m_pendingPageDirection;
    const std::optional<quint64> pageEdgeOffset = m_pendingPageEdgeOffset;
    m_pendingPageDirection = 0;
    m_pendingPageEdgeOffset.reset();
    if (fileEdgeNavigation != 0 && match->scanTargetIdx >= 0 && match->scanTargetIdx < m_scanTargets.size()) {
        const ScanTarget& target = m_scanTargets.at(match->scanTargetIdx);
        if (target.fileSize > 0) {
            const quint64 desiredWindow =
                qMax<quint64>(textViewportByteWindow(), bitmapViewportByteWindow());
            const quint64 loadSize = qMin(target.fileSize, desiredWindow);
            const quint64 loadStart =
                (fileEdgeNavigation < 0 || loadSize >= target.fileSize) ? 0ULL
                                                                        : (target.fileSize - loadSize);
            const auto rawWindow = m_windowLoader.loadRawWindow(
                target.filePath, target.fileSize, loadStart, loadSize, ShiftSettings{});
            if (rawWindow.has_value() && !rawWindow->bytes.isEmpty()) {
                backingPtr->scanTargetIdx = match->scanTargetIdx;
                backingPtr->fileOffset = loadStart;
                backingPtr->bytes = rawWindow->bytes;
                backingPtr->dirty = false;
                applyShiftToBufferIfEnabled(bufferIndex);
            }
        }
    }
    if (pageDirection != 0 && pageEdgeOffset.has_value() &&
        match->scanTargetIdx >= 0 && match->scanTargetIdx < m_scanTargets.size()) {
        const ScanTarget& target = m_scanTargets.at(match->scanTargetIdx);
        const quint64 currentStart = backingPtr->fileOffset;
        const quint64 currentSize = static_cast<quint64>(qMax(0, backingPtr->bytes.size()));
        const quint64 currentEndExclusive = currentStart + currentSize;
        const quint64 requestedEdge = pageEdgeOffset.value();
        const bool outsideCurrent = (currentSize == 0 || requestedEdge < currentStart ||
                                     requestedEdge >= currentEndExclusive);
        if (outsideCurrent && target.fileSize > 0) {
            const quint64 desiredWindow =
                qMax<quint64>(textViewportByteWindow(), bitmapViewportByteWindow());
            const quint64 loadSize = qMin(target.fileSize, desiredWindow);
            quint64 loadStart = 0;
            if (pageDirection < 0) {
                const quint64 edge = qMin(requestedEdge, target.fileSize - 1ULL);
                loadStart = (edge + 1ULL > loadSize) ? (edge + 1ULL - loadSize) : 0ULL;
            } else {
                loadStart = qMin(requestedEdge, target.fileSize - loadSize);
            }
            const auto rawWindow = m_windowLoader.loadRawWindow(
                target.filePath, target.fileSize, loadStart, loadSize, ShiftSettings{});
            if (rawWindow.has_value() && !rawWindow->bytes.isEmpty()) {
                backingPtr->scanTargetIdx = match->scanTargetIdx;
                backingPtr->fileOffset = loadStart;
                backingPtr->bytes = rawWindow->bytes;
                backingPtr->dirty = false;
                applyShiftToBufferIfEnabled(bufferIndex);
            }
        }
    }

    const ResultBuffer& backing = *backingPtr;
    const quint64 backingSize = static_cast<quint64>(qMax(0, backing.bytes.size()));
    if (backingSize == 0) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: backing bufferIndex=%1 empty, return")
                           .arg(bufferIndex));
        return;
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: using bufferIndex=%1 fileOffset=%2 size=%3")
                           .arg(bufferIndex)
                           .arg(backing.fileOffset)
                           .arg(backingSize));
    }

    quint64 center = m_pendingCenterOffset.has_value() ? m_pendingCenterOffset.value() : m_sharedCenterOffset;
    m_pendingCenterOffset.reset();
    if (fileEdgeNavigation < 0) {
        center = backing.fileOffset;
    } else if (fileEdgeNavigation > 0) {
        center = backing.fileOffset + backingSize - 1ULL;
    }
    center = qBound(backing.fileOffset, center, backing.fileOffset + backingSize - 1);
    m_sharedCenterOffset = center;
    BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: center=%1").arg(center));

    const std::optional<quint64> forcedViewportOffset = m_pendingHexViewportOffset;
    const std::optional<QPair<quint64, quint64>> forcedSelectionRange =
        m_pendingHexSelectionRange;
    m_pendingHexViewportOffset.reset();
    m_pendingHexSelectionRange.reset();

    ByteSpan textSpan = centeredSpan(backing, center, textViewportByteWindow());
    if (forcedViewportOffset.has_value()) {
        const quint64 backingEndExclusive = backing.fileOffset + backingSize;
        textSpan.start = qBound(backing.fileOffset, forcedViewportOffset.value(),
                                backingEndExclusive - 1ULL);
        quint64 requestedSize = textViewportByteWindow();
        if (forcedSelectionRange.has_value() &&
            forcedSelectionRange->second > textSpan.start) {
            requestedSize = qMax(requestedSize,
                                 forcedSelectionRange->second - textSpan.start);
        }
        textSpan.size = qMin(requestedSize, backingEndExclusive - textSpan.start);
    } else if (pageDirection != 0 && pageEdgeOffset.has_value()) {
        const quint64 backingStart = backing.fileOffset;
        const quint64 windowSize = qMax<quint64>(1, qMin(textSpan.size, backingSize));
        const quint64 maxStart = backingStart + (backingSize - windowSize);
        if (pageDirection < 0) {
            const quint64 edge = qBound(backingStart, pageEdgeOffset.value(), backingStart + backingSize - 1);
            const quint64 desiredStart =
                (edge + 1ULL > windowSize) ? (edge + 1ULL - windowSize) : backingStart;
            textSpan.start = qBound(backingStart, desiredStart, maxStart);
        } else {
            const quint64 desiredStart = qBound(backingStart, pageEdgeOffset.value(), maxStart);
            textSpan.start = desiredStart;
        }
        textSpan.size = windowSize;
        center = textSpan.start + (textSpan.size / 2ULL);
        m_sharedCenterOffset = center;
    }
    const quint64 backingStart = backing.fileOffset;
    const quint64 backingEndExclusive = backing.fileOffset + backingSize;
    const quint64 currentTextEndExclusive = textSpan.start + textSpan.size;
    const quint64 maxBefore = textSpan.start - backingStart;
    const quint64 beforeExpand = qMin(m_textExpandBeforeBytes, maxBefore);
    const quint64 expandedStart = textSpan.start - beforeExpand;
    const quint64 maxAfter = backingEndExclusive - currentTextEndExclusive;
    const quint64 afterExpand = qMin(m_textExpandAfterBytes, maxAfter);
    const quint64 expandedEndExclusive = currentTextEndExclusive + afterExpand;
    textSpan.start = expandedStart;
    textSpan.size = expandedEndExclusive - expandedStart;
    ByteSpan bitmapSpan = centeredSpan(backing, center, bitmapViewportByteWindow());
    if (textSpan.start < bitmapSpan.start) {
        const quint64 grow = bitmapSpan.start - textSpan.start;
        bitmapSpan.start -= grow;
        bitmapSpan.size += grow;
    }
    const quint64 textSpanEndExclusive = textSpan.start + textSpan.size;
    const quint64 bitmapSpanEndExclusive = bitmapSpan.start + bitmapSpan.size;
    if (textSpanEndExclusive > bitmapSpanEndExclusive) {
        bitmapSpan.size += (textSpanEndExclusive - bitmapSpanEndExclusive);
    }
    const quint64 textEnd = textSpan.start + textSpan.size;
    const quint64 bitmapEnd = bitmapSpan.start + bitmapSpan.size;
    const quint64 unionStart = qMin(textSpan.start, bitmapSpan.start);
    const quint64 unionEnd = qMax(textEnd, bitmapEnd);
    const quint64 unionSize = unionEnd - unionStart;
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "updateSharedPreviewNow: spans text=[%1,+%2] bitmap=[%3,+%4] union=[%5,+%6]")
                           .arg(textSpan.start)
                           .arg(textSpan.size)
                           .arg(bitmapSpan.start)
                           .arg(bitmapSpan.size)
                           .arg(unionStart)
                           .arg(unionSize));
    }

    QByteArray textBytes;
    QByteArray bitmapBytes;
    textBytes.reserve(static_cast<int>(qMin<quint64>(
        textSpan.size, static_cast<quint64>(std::numeric_limits<int>::max()))));
    bitmapBytes.reserve(static_cast<int>(qMin<quint64>(
        bitmapSpan.size, static_cast<quint64>(std::numeric_limits<int>::max()))));

    const quint64 sliceStartUs = debug::selectionTraceElapsedUs();
    const int unionRelStart = static_cast<int>(unionStart - backing.fileOffset);
    for (quint64 i = 0; i < unionSize; ++i) {
        const quint64 absOffset = unionStart + i;
        const char byte = backing.bytes.at(unionRelStart + static_cast<int>(i));
        if (absOffset >= textSpan.start && absOffset < textEnd) {
            textBytes.push_back(byte);
        }
        if (absOffset >= bitmapSpan.start && absOffset < bitmapEnd) {
            bitmapBytes.push_back(byte);
        }
    }
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("updateSharedPreviewNow: sliced textBytes=%1 bitmapBytes=%2 elapsed=%3us")
                           .arg(textBytes.size())
                           .arg(bitmapBytes.size())
                           .arg(debug::selectionTraceElapsedUs() - sliceStartUs));
    }

    const quint64 termLen = static_cast<quint64>(m_scanController.searchTermLength());
    const QString filePath = filePathForTarget(match->scanTargetIdx);
    const std::optional<unsigned char> previousTextByte =
        previousByteBeforeViewport(backing, textSpan.start);

    BRECO_SELTRACE("updateSharedPreviewNow: begin widget updates");
    m_previewSyncInProgress = true;
    quint64 fileSizeBytes = 0;
    if (match->scanTargetIdx >= 0 && match->scanTargetIdx < m_scanTargets.size()) {
        fileSizeBytes = m_scanTargets.at(match->scanTargetIdx).fileSize;
    }
    m_textView->setData(textBytes, textSpan.start, previousTextByte, fileSizeBytes);
    m_textView->setMatchRange(match->offset, static_cast<quint32>(termLen));
    m_textView->setSelectedOffset(center, !forcedViewportOffset.has_value());
    if (forcedSelectionRange.has_value()) {
        m_textView->setSelectionRange(forcedSelectionRange->first,
                                      forcedSelectionRange->second);
    }

    m_bitmapView->setData(bitmapBytes);
    m_bitmapView->setCenterAnchorOffset(center);
    m_bitmapView->setResultHighlight(match->offset, 0, static_cast<quint32>(termLen), 0,
                                     bitmapSpan.start);
    m_previewSyncInProgress = false;
    BRECO_SELTRACE("updateSharedPreviewNow: widget updates done");

    m_textHoverBuffer.filePath = filePath;
    m_textHoverBuffer.baseOffset = textSpan.start;
    m_textHoverBuffer.data = textBytes;
    m_bitmapHoverBuffer.filePath = filePath;
    m_bitmapHoverBuffer.baseOffset = bitmapSpan.start;
    m_bitmapHoverBuffer.data = bitmapBytes;
    updateHexInfoPanel();
    refreshDataViewFromNavigator();
    updateBufferStatusLine();
    BRECO_SELTRACE("updateSharedPreviewNow: hover buffers updated");
    BRECO_SELTRACE("updateSharedPreviewNow: done");
}

void MainWindow::showMatchPreview(int row, const MatchRecord& match) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("showMatchPreview: start row=%1 targetIdx=%2 offset=%3")
                           .arg(row)
                           .arg(match.scanTargetIdx)
                           .arg(match.offset));
    }
    if (row < 0 || row >= m_matchBufferIndices.size()) {
        BRECO_SELTRACE("showMatchPreview: row outside mapping, return");
        return;
    }
    const int previousRow = m_activePreviewRow;
    if (previousRow != row) {
        restoreDirtyBufferForRow(previousRow);
    }

    const int bufferIndex = m_matchBufferIndices.at(row);
    if (bufferIndex < 0 || bufferIndex >= m_resultBuffers.size()) {
        BRECO_SELTRACE(QStringLiteral("showMatchPreview: invalid bufferIndex=%1, return").arg(bufferIndex));
        return;
    }
    if (!restoreBufferRawIfDirty(bufferIndex)) {
        BRECO_SELTRACE(QStringLiteral("showMatchPreview: failed restoring dirty bufferIndex=%1")
                           .arg(bufferIndex));
        return;
    }
    applyShiftToBufferIfEnabled(bufferIndex);
    if (m_activePreviewRow != row) {
        m_textExpandBeforeBytes = 0;
        m_textExpandAfterBytes = 0;
    }
    m_activePreviewRow = row;
    m_sharedCenterOffset = match.offset;
    rememberActiveSingleFileOffset(match.offset);
    m_pendingCenterOffset.reset();
    BRECO_SELTRACE(QStringLiteral("showMatchPreview: updateSharedPreviewNow begin bufferIndex=%1").arg(bufferIndex));
    updateSharedPreviewNow();
    BRECO_SELTRACE("showMatchPreview: updateSharedPreviewNow end");
}

void MainWindow::loadNotEmptyPreview() {
    if (m_sourceMode != SourceMode::SingleFile || m_scanTargets.size() != 1) {
        return;
    }

    const ScanTarget& target = m_scanTargets.first();
    if (target.fileSize == 0) {
        return;
    }

    const quint64 size = qMin<quint64>(kNotEmptyInitialBytes, target.fileSize);
    const auto rawWindow =
        m_windowLoader.loadRawWindow(target.filePath, target.fileSize, 0, size, ShiftSettings{});
    if (!rawWindow.has_value()) {
        return;
    }
    const QByteArray transformedBytes = rawWindow->bytes;
    if (transformedBytes.isEmpty()) {
        return;
    }

    MatchRecord synthetic;
    synthetic.scanTargetIdx = 0;
    synthetic.threadId = 0;
    synthetic.offset = 0;
    synthetic.searchTimeNs = 0;

    clearResultBufferCacheState();
    ResultBuffer rb;
    rb.scanTargetIdx = 0;
    rb.fileOffset = 0;
    rb.bytes = transformedBytes;
    rb.dirty = false;
    m_resultBuffers.push_back(rb);
    m_matchBufferIndices = {0};
    m_lastSyntheticBufferIndex = 0;

    m_resultModel.clear();
    m_resultModel.appendBatch({synthetic});
    rebuildTargetMatchIntervals();
    selectResultRow(0);
    updateBufferStatusLine();
}

void MainWindow::startImageScan() {
    if (m_dataViewImagePanel == nullptr || m_imageScanController == nullptr) {
        return;
    }
    if (m_imageScanController->isRunning()) {
        m_imageScanController->requestStop();
        m_dataViewImagePanel->setStatusText(QStringLiteral("Stopping image scan..."));
        return;
    }

    EmbeddedImageScanSource source;
    EmbeddedImageScanOptions options = m_dataViewImagePanel->scanOptions();
    QString errorMessage;
    if (!buildImageScanRequest(source, options, errorMessage)) {
        m_dataViewImagePanel->setStatusText(errorMessage);
        return;
    }

    m_dataViewImagePanel->clearResults();
    m_dataViewImagePanel->resetProgress();
    m_dataViewImagePanel->setScanRunning(true);
    m_dataViewImagePanel->setStatusText(QStringLiteral("Scanning for images..."));

    EmbeddedImageScanRequest request;
    request.source = std::move(source);
    request.options = options;
    m_activeImageScanId = m_imageScanController->startScan(request);
}

void MainWindow::cancelImageScan() {
    m_activeImageScanId = 0;
    if (m_imageScanController != nullptr) {
        m_imageScanController->requestStop();
    }
    if (m_dataViewImagePanel != nullptr) {
        m_dataViewImagePanel->setScanRunning(false);
    }
}

std::optional<int> MainWindow::activePreviewTargetIndex() const {
    if (m_activePreviewRow >= 0 && m_activePreviewRow < m_resultModel.rowCount()) {
        if (const MatchRecord* match = m_resultModel.matchAt(m_activePreviewRow);
            match != nullptr && match->scanTargetIdx >= 0 &&
            match->scanTargetIdx < m_scanTargets.size()) {
            return match->scanTargetIdx;
        }
    }
    if (isSingleFileModeActive()) {
        return 0;
    }
    return std::nullopt;
}

quint64 MainWindow::binaryLengthFromInput(double value, int unitIndex,
                                          quint64 remainingBytes) {
    static constexpr quint64 multipliers[] = {
        1ULL,
        1024ULL,
        1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    if (!std::isfinite(value) || value <= 0.0) {
        return 0;
    }

    const int clampedUnit = qBound(0, unitIndex, 4);
    const long double multiplier = static_cast<long double>(multipliers[clampedUnit]);
    const long double maxBytes =
        static_cast<long double>(std::numeric_limits<quint64>::max());
    const long double input = static_cast<long double>(value);
    const quint64 requestedBytes =
        input >= maxBytes / multiplier
            ? std::numeric_limits<quint64>::max()
            : static_cast<quint64>(input * multiplier);
    return qMin(remainingBytes, requestedBytes);
}

QString MainWindow::binaryAmountText(quint64 bytes, int unitIndex) {
    static constexpr quint64 multipliers[] = {
        1ULL,
        1024ULL,
        1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    static const char* units[] = {"Bytes", "KiB", "MiB", "GiB", "TiB"};

    if (unitIndex < 0) {
        unitIndex = 0;
        while (unitIndex < 4 && bytes >= multipliers[unitIndex + 1]) {
            ++unitIndex;
        }
    }
    unitIndex = qBound(0, unitIndex, 4);
    if (unitIndex == 0) {
        return QStringLiteral("%1 Bytes").arg(bytes);
    }

    const long double value = static_cast<long double>(bytes) /
                              static_cast<long double>(multipliers[unitIndex]);
    return QStringLiteral("%1 %2")
        .arg(QString::number(static_cast<double>(value), 'f', 2), units[unitIndex]);
}

QString MainWindow::binaryProgressText(quint64 written, quint64 total) {
    int unitIndex = 0;
    static constexpr quint64 multipliers[] = {
        1ULL,
        1024ULL,
        1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    while (unitIndex < 4 && total >= multipliers[unitIndex + 1]) {
        ++unitIndex;
    }
    return QStringLiteral("%1 / %2")
        .arg(binaryAmountText(written, unitIndex), binaryAmountText(total, unitIndex));
}

std::optional<quint64> MainWindow::promptBinaryRangeLength(quint64 remainingBytes) {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("saveRangeAsBinaryDialog"));
    dialog.setWindowTitle(QStringLiteral("Save range as binary"));
    dialog.setMinimumWidth(460);

    auto* layout = new QVBoxLayout(&dialog);
    auto* untilEndRadio = new QRadioButton(
        QStringLiteral("Until end of file (%1)").arg(binaryAmountText(remainingBytes)), &dialog);
    untilEndRadio->setObjectName(QStringLiteral("saveRangeUntilEndRadio"));
    untilEndRadio->setChecked(true);
    layout->addWidget(untilEndRadio);

    auto* numericRow = new QHBoxLayout();
    auto* numericRadio = new QRadioButton(&dialog);
    numericRadio->setObjectName(QStringLiteral("saveRangeNumericRadio"));
    numericRadio->setAccessibleName(QStringLiteral("Specific length"));
    numericRow->addWidget(numericRadio);

    auto* valueInput = new QLineEdit(&dialog);
    valueInput->setObjectName(QStringLiteral("saveRangeValueInput"));
    auto* validator = new QDoubleValidator(
        0.0, std::numeric_limits<double>::max(), 12, valueInput);
    validator->setNotation(QDoubleValidator::ScientificNotation);
    validator->setLocale(QLocale::c());
    valueInput->setValidator(validator);
    valueInput->setAlignment(Qt::AlignRight);
    numericRow->addWidget(valueInput, 1);

    auto* unitCombo = new QComboBox(&dialog);
    unitCombo->setObjectName(QStringLiteral("saveRangeUnitCombo"));
    unitCombo->addItems({QStringLiteral("Bytes"), QStringLiteral("KiB"),
                         QStringLiteral("MiB"), QStringLiteral("GiB"),
                         QStringLiteral("TiB")});
    numericRow->addWidget(unitCombo);
    layout->addLayout(numericRow);

    static constexpr quint64 multipliers[] = {
        1ULL,
        1024ULL,
        1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL,
        1024ULL * 1024ULL * 1024ULL * 1024ULL,
    };
    int initialUnit = 0;
    while (initialUnit < 4 && remainingBytes >= multipliers[initialUnit + 1]) {
        ++initialUnit;
    }
    unitCombo->setCurrentIndex(initialUnit);
    const long double initialValue =
        static_cast<long double>(remainingBytes) /
        static_cast<long double>(multipliers[initialUnit]);
    valueInput->setText(QString::number(static_cast<double>(initialValue), 'g', 12));

    valueInput->setEnabled(false);
    unitCombo->setEnabled(false);
    connect(numericRadio, &QRadioButton::toggled, valueInput, &QWidget::setEnabled);
    connect(numericRadio, &QRadioButton::toggled, unitCombo, &QWidget::setEnabled);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->setObjectName(QStringLiteral("saveRangeButtons"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::optional<quint64> selectedLength;
    auto updateOkEnabled = [&]() {
        const bool acceptable = untilEndRadio->isChecked() || valueInput->hasAcceptableInput();
        buttons->button(QDialogButtonBox::Ok)->setEnabled(acceptable);
    };
    connect(untilEndRadio, &QRadioButton::toggled, &dialog,
            [&](bool) { updateOkEnabled(); });
    connect(valueInput, &QLineEdit::textChanged, &dialog,
            [&](const QString&) { updateOkEnabled(); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (untilEndRadio->isChecked()) {
            selectedLength = remainingBytes;
            dialog.accept();
            return;
        }
        bool valid = false;
        const double value = QLocale::c().toDouble(valueInput->text(), &valid);
        if (!valid || value < 0.0) {
            valueInput->setFocus();
            return;
        }
        selectedLength = binaryLengthFromInput(value, unitCombo->currentIndex(),
                                               remainingBytes);
        dialog.accept();
    });
    updateOkEnabled();

    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return selectedLength;
}

void MainWindow::saveSelectedBinaryRange(quint64 startOffset,
                                         quint64 endOffsetExclusive) {
    const std::optional<int> targetIndex = activePreviewTargetIndex();
    if (!targetIndex.has_value()) {
        QMessageBox::warning(this, QStringLiteral("Save File"),
                             QStringLiteral("There is no active source to save."));
        return;
    }
    const ScanTarget target = m_scanTargets.at(targetIndex.value());
    const quint64 end = qMin(endOffsetExclusive, target.fileSize);
    if (startOffset >= target.fileSize || end <= startOffset) {
        QMessageBox::warning(this, QStringLiteral("Save File"),
                             QStringLiteral("The selected byte range is outside the active source."));
        return;
    }
    saveBinaryRangeWithDialogs(target, startOffset, end - startOffset);
}

void MainWindow::saveBinaryFromHere(quint64 startOffset) {
    const std::optional<int> targetIndex = activePreviewTargetIndex();
    if (!targetIndex.has_value()) {
        QMessageBox::warning(this, QStringLiteral("Save File"),
                             QStringLiteral("There is no active source to save."));
        return;
    }
    const ScanTarget target = m_scanTargets.at(targetIndex.value());
    if (startOffset >= target.fileSize) {
        QMessageBox::warning(this, QStringLiteral("Save File"),
                             QStringLiteral("The selected byte offset is outside the active source."));
        return;
    }

    const std::optional<quint64> length =
        promptBinaryRangeLength(target.fileSize - startOffset);
    if (!length.has_value()) {
        return;
    }
    saveBinaryRangeWithDialogs(target, startOffset, length.value());
}

void MainWindow::saveBinaryRangeWithDialogs(const ScanTarget& target,
                                            quint64 startOffset, quint64 length) {
    const QString outputPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save File"), QString(), QStringLiteral("*.*"));
    if (outputPath.isEmpty()) {
        return;
    }
    saveBinaryRangeWithProgress(outputPath, target, startOffset, length);
}

void MainWindow::saveBinaryRangeWithProgress(const QString& outputPath,
                                             const ScanTarget& target,
                                             quint64 startOffset, quint64 length) {
    QDialog progressDialog(this);
    progressDialog.setObjectName(QStringLiteral("binarySaveProgressDialog"));
    progressDialog.setWindowTitle(QStringLiteral("Save binary"));
    progressDialog.setWindowModality(Qt::WindowModal);
    progressDialog.setWindowFlag(Qt::WindowCloseButtonHint, false);
    progressDialog.setMinimumWidth(420);

    auto* layout = new QVBoxLayout(&progressDialog);
    auto* statusLabel = new QLabel(QStringLiteral("Saving file..."), &progressDialog);
    statusLabel->setObjectName(QStringLiteral("binarySaveStatusLabel"));
    layout->addWidget(statusLabel);

    auto* progressBar = new QProgressBar(&progressDialog);
    progressBar->setObjectName(QStringLiteral("binarySaveProgressBar"));
    progressBar->setRange(0, 1000);
    progressBar->setValue(length == 0 ? 1000 : 0);
    progressBar->setFormat(binaryProgressText(0, length));
    layout->addWidget(progressBar);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &progressDialog);
    buttons->setObjectName(QStringLiteral("binarySaveButtons"));
    buttons->hide();
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &progressDialog, &QDialog::accept);

    bool saved = false;
    QString errorMessage;
    QTimer::singleShot(0, &progressDialog, [&]() {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const auto updateProgress = [&](quint64 written) {
            const int progress =
                length == 0
                    ? 1000
                    : static_cast<int>((static_cast<long double>(written) * 1000.0L) /
                                       static_cast<long double>(length));
            progressBar->setValue(qBound(0, progress, 1000));
            progressBar->setFormat(binaryProgressText(written, length));
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        };

        saved = writeBinaryRangeToFile(outputPath, target, startOffset, length,
                                       currentShiftSettings(), &errorMessage,
                                       updateProgress);
        if (!saved) {
            progressDialog.reject();
            return;
        }
        statusLabel->setText(QStringLiteral("File saved"));
        progressBar->hide();
        buttons->show();
    });

    progressDialog.exec();
    if (!saved) {
        QMessageBox::warning(
            this, QStringLiteral("Save File"),
            errorMessage.isEmpty()
                ? QStringLiteral("Could not save the binary file to %1.").arg(outputPath)
                : errorMessage);
    }
}

bool MainWindow::writeBinaryRangeToFile(
    const QString& outputPath, const ScanTarget& target, quint64 startOffset,
    quint64 length, const ShiftSettings& shift, QString* errorMessage,
    const std::function<void(quint64)>& progressCallback) {
    auto fail = [&](const QString& message) {
        if (errorMessage != nullptr) {
            *errorMessage = message;
        }
        return false;
    };

    if (outputPath.isEmpty() || target.filePath.isEmpty() ||
        startOffset > target.fileSize || length > target.fileSize - startOffset) {
        return fail(QStringLiteral("The requested byte range is invalid."));
    }

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("Could not open %1 for writing: %2")
                        .arg(outputPath, output.errorString()));
    }

    quint64 written = 0;
    while (written < length) {
        const quint64 chunkSize = qMin(kBinarySaveChunkBytes, length - written);
        const quint64 chunkOffset = startOffset + written;
        const std::optional<QByteArray> chunk = m_windowLoader.loadTransformedWindow(
            target.filePath, target.fileSize, chunkOffset, chunkSize, shift);
        if (!chunk.has_value() || static_cast<quint64>(chunk->size()) != chunkSize) {
            output.cancelWriting();
            return fail(QStringLiteral("Could not read %1 bytes from the source at offset %2.")
                            .arg(chunkSize)
                            .arg(chunkOffset));
        }

        qint64 chunkWritten = 0;
        while (chunkWritten < chunk->size()) {
            const qint64 count = output.write(
                chunk->constData() + chunkWritten, chunk->size() - chunkWritten);
            if (count <= 0) {
                const QString outputError = output.errorString();
                output.cancelWriting();
                return fail(QStringLiteral("Could not write %1: %2")
                                .arg(outputPath, outputError));
            }
            chunkWritten += count;
        }

        written += chunkSize;
        if (progressCallback) {
            progressCallback(written);
        }
    }

    if (!output.commit()) {
        return fail(QStringLiteral("Could not finish writing %1: %2")
                        .arg(outputPath, output.errorString()));
    }
    return true;
}

quint64 MainWindow::imageScanStartOffset() const {
    if (m_textView != nullptr && m_textView->selectedOffset().has_value()) {
        return m_textView->selectedOffset().value();
    }
    if (m_textView != nullptr && m_textView->firstVisibleByteOffset().has_value()) {
        return m_textView->firstVisibleByteOffset().value();
    }
    return m_textHoverBuffer.baseOffset;
}

bool MainWindow::buildImageScanRequest(EmbeddedImageScanSource& source,
                                       EmbeddedImageScanOptions& options,
                                       QString& errorMessage) const {
    const EmbeddedImageScope scope =
        m_dataViewImagePanel != nullptr ? m_dataViewImagePanel->selectedScope()
                                        : EmbeddedImageScope::FromStart;
    if (!options.formats) {
        errorMessage = QStringLiteral("Select at least one supported image format.");
        return false;
    }

    if (scope == EmbeddedImageScope::VisibleBuffer) {
        if (m_textHoverBuffer.data.isEmpty()) {
            errorMessage = QStringLiteral("There is no visible hex buffer to scan.");
            return false;
        }
        const QByteArray visibleBytes = m_textHoverBuffer.data;
        const quint64 baseOffset = m_textHoverBuffer.baseOffset;
        source.filePath = m_textHoverBuffer.filePath;
        source.fileSize = baseOffset + static_cast<quint64>(visibleBytes.size());
        source.read = [visibleBytes, baseOffset](quint64 offset,
                                                 quint64 size) -> std::optional<QByteArray> {
            if (offset < baseOffset) {
                return std::nullopt;
            }
            const quint64 rel = offset - baseOffset;
            if (rel >= static_cast<quint64>(visibleBytes.size())) {
                return QByteArray();
            }
            const quint64 available = static_cast<quint64>(visibleBytes.size()) - rel;
            const quint64 readSize = qMin(size, available);
            return visibleBytes.mid(static_cast<qsizetype>(rel),
                                    static_cast<qsizetype>(readSize));
        };
        options.startOffset = baseOffset;
        options.endOffsetExclusive = source.fileSize;
        return true;
    }

    const std::optional<int> targetIndex = activePreviewTargetIndex();
    if (!targetIndex.has_value()) {
        errorMessage = QStringLiteral("Select or preview a file before scanning for images.");
        return false;
    }
    const ScanTarget& target = m_scanTargets.at(targetIndex.value());
    if (target.filePath.isEmpty() || target.fileSize == 0) {
        errorMessage = QStringLiteral("The active source has no readable bytes.");
        return false;
    }

    const ShiftSettings shift = currentShiftSettings();
    const ShiftedWindowLoader* loader = &m_windowLoader;
    source.filePath = target.filePath;
    source.fileSize = target.fileSize;
    source.read = [loader, filePath = target.filePath, fileSize = target.fileSize,
                   shift](quint64 offset, quint64 size) -> std::optional<QByteArray> {
        return loader->loadTransformedWindow(filePath, fileSize, offset, size, shift);
    };
    options.startOffset = (scope == EmbeddedImageScope::FromStart) ? 0 : imageScanStartOffset();
    options.endOffsetExclusive = target.fileSize;
    if (options.startOffset >= options.endOffsetExclusive) {
        errorMessage = QStringLiteral("The image scan start offset is outside the active source.");
        return false;
    }
    return true;
}

void MainWindow::finishImageScan(quint64 scanId, const EmbeddedImageScanSummary& summary,
                                 const QVector<EmbeddedImageResult>& results) {
    if (scanId != m_activeImageScanId || m_dataViewImagePanel == nullptr) {
        return;
    }
    m_activeImageScanId = 0;
    m_dataViewImagePanel->setScanRunning(false);
    if (m_dataViewImagePanel->resultCount() == 0) {
        for (const EmbeddedImageResult& result : results) {
            m_dataViewImagePanel->addResult(result);
        }
    }
    const QString suffix = summary.bytesScanned > 0
                               ? QStringLiteral(" Scanned %1 bytes.").arg(summary.bytesScanned)
                               : QString();
    m_dataViewImagePanel->setStatusText(summary.message + suffix);
}

void MainWindow::writeStatusLineToStdout(const QString& line) {
    if (line == m_lastStatusLineText) {
        return;
    }
    m_lastStatusLineText = line;
    if (QStatusBar* sb = statusBar(); sb != nullptr) {
        sb->showMessage(line);
    }
}

QString MainWindow::formatBinarySizeFixed2(quint64 bytes) const {
    static const char* units[] = {"B", "KiB", "MiB", "GiB"};
    long double value = static_cast<long double>(bytes);
    int unitIdx = 0;
    while (value >= 1024.0L && unitIdx < 3) {
        value /= 1024.0L;
        ++unitIdx;
    }
    return QStringLiteral("%1 %2")
        .arg(QString::number(static_cast<double>(value), 'f', 2), units[unitIdx]);
}

void MainWindow::updateBufferStatusLine() {
    quint64 currentBytes = 0;
    if (m_activePreviewRow >= 0 && m_activePreviewRow < m_matchBufferIndices.size()) {
        const int idx = m_matchBufferIndices.at(m_activePreviewRow);
        if (idx >= 0 && idx < m_resultBuffers.size()) {
            currentBytes = static_cast<quint64>(qMax(0, m_resultBuffers.at(idx).bytes.size()));
        }
    }
    const QVector<int> refCounts = bufferReferenceCounts();
    const quint64 allBytes = totalResidentBufferBytes(refCounts);
    writeStatusLineToStdout(QStringLiteral("Current buffer: %1  --  All buffers: %2")
                                .arg(formatBinarySizeFixed2(currentBytes))
                                .arg(formatBinarySizeFixed2(allBytes)));
}

bool MainWindow::isSingleFileModeActive() const {
    return m_sourceMode == SourceMode::SingleFile && m_scanTargets.size() == 1;
}

bool MainWindow::isSyntheticPreviewMatch(const MatchRecord& match) const {
    return match.scanTargetIdx == 0 && match.threadId == 0 && match.offset == 0 &&
           match.searchTimeNs == 0;
}

bool MainWindow::insertSyntheticPreviewResultAtTop() {
    if (!isSingleFileModeActive()) {
        return false;
    }
    const ScanTarget& target = m_scanTargets.first();
    if (target.fileSize == 0) {
        return false;
    }
    const quint64 size = qMin<quint64>(kNotEmptyInitialBytes, target.fileSize);
    const auto rawWindow =
        m_windowLoader.loadRawWindow(target.filePath, target.fileSize, 0, size, ShiftSettings{});
    if (!rawWindow.has_value() || rawWindow->bytes.isEmpty()) {
        return false;
    }

    MatchRecord synthetic;
    synthetic.scanTargetIdx = 0;
    synthetic.threadId = 0;
    synthetic.offset = 0;
    synthetic.searchTimeNs = 0;

    QVector<MatchRecord> rebuiltMatches;
    rebuiltMatches.reserve(m_resultModel.rowCount() + 1);
    rebuiltMatches.push_back(synthetic);
    const QVector<MatchRecord>& existingMatches = m_resultModel.allMatches();
    int oldStartRow = 0;
    if (!existingMatches.isEmpty() && isSyntheticPreviewMatch(existingMatches.first())) {
        oldStartRow = 1;
    }
    for (int i = oldStartRow; i < existingMatches.size(); ++i) {
        rebuiltMatches.push_back(existingMatches.at(i));
    }

    QVector<ResultBuffer> oldBuffers = m_resultBuffers;
    QVector<int> oldIndices = m_matchBufferIndices;
    if (oldStartRow == 1 && !oldIndices.isEmpty()) {
        oldIndices.removeFirst();
    }
    if (oldStartRow == 1 && m_lastSyntheticBufferIndex >= 0 && m_lastSyntheticBufferIndex < oldBuffers.size()) {
        oldBuffers.removeAt(m_lastSyntheticBufferIndex);
        for (int& idx : oldIndices) {
            if (idx > m_lastSyntheticBufferIndex) {
                --idx;
            } else if (idx == m_lastSyntheticBufferIndex) {
                idx = -1;
            }
        }
    }

    ResultBuffer syntheticBuffer;
    syntheticBuffer.scanTargetIdx = 0;
    syntheticBuffer.fileOffset = 0;
    syntheticBuffer.bytes = rawWindow->bytes;
    syntheticBuffer.dirty = false;

    QVector<ResultBuffer> rebuiltBuffers;
    rebuiltBuffers.reserve(oldBuffers.size() + 1);
    rebuiltBuffers.push_back(syntheticBuffer);
    for (const ResultBuffer& b : oldBuffers) {
        rebuiltBuffers.push_back(b);
    }
    QVector<int> rebuiltIndices;
    rebuiltIndices.reserve(oldIndices.size() + 1);
    rebuiltIndices.push_back(0);
    for (const int idx : oldIndices) {
        rebuiltIndices.push_back(idx < 0 ? -1 : idx + 1);
    }

    m_resultBuffers = rebuiltBuffers;
    m_matchBufferIndices = rebuiltIndices;
    m_lastSyntheticBufferIndex = 0;
    m_resultModel.clear();
    m_resultModel.appendBatch(rebuiltMatches);
    rebuildTargetMatchIntervals();
    m_activeOverlapTargetIdx = -1;
    m_activePreviewRow = -1;
    return true;
}

void MainWindow::refreshCurrentByteInfoFromLastHover() {
    if (!m_lastHoverAbsoluteOffset.has_value()) {
        clearCurrentByteInfo();
        return;
    }
    switch (m_lastHoverSource) {
        case HoverSource::Text:
            updateCurrentByteInfoFromHover(m_textHoverBuffer, m_lastHoverAbsoluteOffset.value());
            break;
        case HoverSource::Bitmap:
            updateCurrentByteInfoFromHover(m_bitmapHoverBuffer, m_lastHoverAbsoluteOffset.value());
            break;
        case HoverSource::None:
        default:
            clearCurrentByteInfo();
            break;
    }
}

void MainWindow::updateCurrentByteInfoFromHover(const HoverBuffer& buffer, quint64 absoluteOffset) {
    if (m_currentByteInfoPanel == nullptr || buffer.data.isEmpty()) {
        clearCurrentByteInfo();
        return;
    }
    if (absoluteOffset < buffer.baseOffset ||
        absoluteOffset >= buffer.baseOffset + static_cast<quint64>(buffer.data.size())) {
        clearCurrentByteInfo();
        return;
    }

    const int relativeIndex = static_cast<int>(absoluteOffset - buffer.baseOffset);
    if (relativeIndex < 0 || relativeIndex >= buffer.data.size()) {
        clearCurrentByteInfo();
        return;
    }

    const int availableBytes = qMax(0, buffer.data.size() - relativeIndex);
    const unsigned char b0 = static_cast<unsigned char>(buffer.data.at(relativeIndex));
    const QString ascii = printableAsciiChar(b0);
    const QString utf8 = utf8Glyph(buffer.data, relativeIndex);
    const bool useBigEndian = m_currentByteInfoPanel->bigEndianCheckBox()->isChecked();
    const QString utf16 = utf16Glyph(buffer.data, relativeIndex, !useBigEndian,
                                     QStringLiteral("n/a"));

    const NumberSystem numberSystem = currentNumberSystem(m_currentByteInfoPanel);
    bool ok8 = false;
    bool ok16 = false;
    bool ok32 = false;
    bool ok64 = false;
    const quint64 v8 = useBigEndian ? readUnsignedBig(buffer.data, relativeIndex, 1, &ok8)
                                    : readUnsignedLittle(buffer.data, relativeIndex, 1, &ok8);
    const quint64 v16 = useBigEndian ? readUnsignedBig(buffer.data, relativeIndex, 2, &ok16)
                                     : readUnsignedLittle(buffer.data, relativeIndex, 2, &ok16);
    const quint64 v32 = useBigEndian ? readUnsignedBig(buffer.data, relativeIndex, 4, &ok32)
                                     : readUnsignedLittle(buffer.data, relativeIndex, 4, &ok32);
    const quint64 v64 = useBigEndian ? readUnsignedBig(buffer.data, relativeIndex, 8, &ok64)
                                     : readUnsignedLittle(buffer.data, relativeIndex, 8, &ok64);

    const QString na = QStringLiteral("n/a");
    m_currentByteInfoPanel->asciiValueLabel()->setText(ascii);
    m_currentByteInfoPanel->utf8ValueLabel()->setText(utf8);
    m_currentByteInfoPanel->utf16ValueLabel()->setText(utf16);
    m_currentByteInfoPanel->hexStr8BytesValueLabel()->setText(
        formatHexWindow8(buffer.data, relativeIndex, useBigEndian));
    m_currentByteInfoPanel->s8ValueLabel()->setText(
        ok8 ? formatSignedByNumberSystem(v8, 1, numberSystem) : na);
    m_currentByteInfoPanel->u8ValueLabel()->setText(
        ok8 ? formatUnsignedByNumberSystem(v8, 1, numberSystem) : na);
    m_currentByteInfoPanel->s16ValueLabel()->setText(
        ok16 ? formatSignedByNumberSystem(v16, 2, numberSystem) : na);
    m_currentByteInfoPanel->u16ValueLabel()->setText(
        ok16 ? formatUnsignedByNumberSystem(v16, 2, numberSystem) : na);
    m_currentByteInfoPanel->s32ValueLabel()->setText(
        ok32 ? formatSignedByNumberSystem(v32, 4, numberSystem) : na);
    m_currentByteInfoPanel->u32ValueLabel()->setText(
        ok32 ? formatUnsignedByNumberSystem(v32, 4, numberSystem) : na);
    m_currentByteInfoPanel->s64ValueLabel()->setText(
        ok64 ? formatSignedByNumberSystem(v64, 8, numberSystem) : na);
    m_currentByteInfoPanel->u64ValueLabel()->setText(
        ok64 ? formatUnsignedByNumberSystem(v64, 8, numberSystem) : na);

    m_currentByteInfoPanel->byteInterpretationLargeLabel()->setText(
        utf16DisplayGlyph(buffer.data, relativeIndex, !useBigEndian));
    setCurrentByteCaptionHighlights(availableBytes);
}

void MainWindow::setCurrentByteCaptionHighlights(int availableBytes) {
    if (m_currentByteInfoPanel == nullptr) {
        return;
    }
    resetCurrentByteCaptionHighlights();
    const QColor c8(173, 216, 230);
    const QColor c16(130, 190, 220);
    const QColor c32(178, 235, 179);
    const QColor c64(120, 200, 130);
    auto styleCaption = [](QLabel* label, const QColor& color) {
        if (label != nullptr) {
            label->setStyleSheet(QStringLiteral("QLabel { background-color: %1; }").arg(color.name()));
        }
    };
    if (availableBytes >= 1) {
        styleCaption(m_currentByteInfoPanel->s8CaptionLabel(), c8);
        styleCaption(m_currentByteInfoPanel->u8CaptionLabel(), c8);
    }
    if (availableBytes >= 2) {
        styleCaption(m_currentByteInfoPanel->s16CaptionLabel(), c16);
        styleCaption(m_currentByteInfoPanel->u16CaptionLabel(), c16);
    }
    if (availableBytes >= 4) {
        styleCaption(m_currentByteInfoPanel->s32CaptionLabel(), c32);
        styleCaption(m_currentByteInfoPanel->u32CaptionLabel(), c32);
    }
    if (availableBytes >= 8) {
        styleCaption(m_currentByteInfoPanel->s64CaptionLabel(), c64);
        styleCaption(m_currentByteInfoPanel->u64CaptionLabel(), c64);
    }
}

void MainWindow::resetCurrentByteCaptionHighlights() {
    if (m_currentByteInfoPanel == nullptr) {
        return;
    }
    auto clearCaption = [](QLabel* label) {
        if (label != nullptr) {
            label->setStyleSheet(QString());
        }
    };
    clearCaption(m_currentByteInfoPanel->s8CaptionLabel());
    clearCaption(m_currentByteInfoPanel->u8CaptionLabel());
    clearCaption(m_currentByteInfoPanel->s16CaptionLabel());
    clearCaption(m_currentByteInfoPanel->u16CaptionLabel());
    clearCaption(m_currentByteInfoPanel->s32CaptionLabel());
    clearCaption(m_currentByteInfoPanel->u32CaptionLabel());
    clearCaption(m_currentByteInfoPanel->s64CaptionLabel());
    clearCaption(m_currentByteInfoPanel->u64CaptionLabel());
}

void MainWindow::clearCurrentByteInfo() {
    if (m_currentByteInfoPanel == nullptr) {
        return;
    }
    if (m_textView != nullptr) {
        m_textView->setHoverAnchorOffset(std::nullopt);
    }
    const QString empty = QStringLiteral("-");
    m_currentByteInfoPanel->byteInterpretationLargeLabel()->setText(empty);
    m_currentByteInfoPanel->asciiValueLabel()->setText(empty);
    m_currentByteInfoPanel->utf8ValueLabel()->setText(empty);
    m_currentByteInfoPanel->utf16ValueLabel()->setText(empty);
    m_currentByteInfoPanel->hexStr8BytesValueLabel()->setText(empty);
    m_currentByteInfoPanel->s8ValueLabel()->setText(empty);
    m_currentByteInfoPanel->u8ValueLabel()->setText(empty);
    m_currentByteInfoPanel->s16ValueLabel()->setText(empty);
    m_currentByteInfoPanel->u16ValueLabel()->setText(empty);
    m_currentByteInfoPanel->s32ValueLabel()->setText(empty);
    m_currentByteInfoPanel->u32ValueLabel()->setText(empty);
    m_currentByteInfoPanel->s64ValueLabel()->setText(empty);
    m_currentByteInfoPanel->u64ValueLabel()->setText(empty);
    resetCurrentByteCaptionHighlights();
    m_lastHoverAbsoluteOffset.reset();
    m_lastHoverSource = HoverSource::None;
}

void MainWindow::onTextHoverOffsetChanged(quint64 absoluteOffset) {
    m_bitmapView->setExternalHoverOffset(absoluteOffset);
    m_lastHoverSource = HoverSource::Text;
    m_lastHoverAbsoluteOffset = absoluteOffset;
    m_textView->setHoverAnchorOffset(absoluteOffset);
    updateCurrentByteInfoFromHover(m_textHoverBuffer, absoluteOffset);
}

void MainWindow::onTextCenterAnchorRequested(quint64 absoluteOffset) {
    if (m_previewSyncInProgress || m_textScrollDragInProgress) {
        return;
    }
    requestSharedCenter(absoluteOffset);
}

void MainWindow::onBitmapHoverOffsetChanged(quint64 absoluteOffset) {
    m_lastHoverSource = HoverSource::Bitmap;
    m_lastHoverAbsoluteOffset = absoluteOffset;
    m_textView->setHoverAnchorOffset(absoluteOffset);
    updateCurrentByteInfoFromHover(m_bitmapHoverBuffer, absoluteOffset);
}

void MainWindow::onBitmapByteClicked(quint64 absoluteOffset) {
    clearStructSourceHighlight();
    clearStructPreview();
    requestSharedCenter(absoluteOffset);
}

void MainWindow::onTextByteClicked(quint64 absoluteOffset) {
    if (m_structModeLeftPanel->previewEnabled() &&
        m_structModeLeftPanel->canPreview()) {
        createStructPreview(absoluteOffset);
    }
}

void MainWindow::setStructSourceHighlight(quint64 absoluteOffset,
                                          quint64 byteLength) {
    if (byteLength == 0) {
        clearStructSourceHighlight();
        return;
    }
    const quint64 rangeEnd =
        byteLength > std::numeric_limits<quint64>::max() - absoluteOffset
            ? std::numeric_limits<quint64>::max()
            : absoluteOffset + byteLength;
    m_structSourceHighlightRange = qMakePair(absoluteOffset, rangeEnd);
    if (m_textView != nullptr) {
        m_textView->setExternalSelectionRange(m_structSourceHighlightRange);
    }
    if (m_bitmapView != nullptr) {
        m_bitmapView->setExternalSelectionRange(m_structSourceHighlightRange);
    }
}

void MainWindow::clearStructSourceHighlight() {
    m_structSourceHighlightRange.reset();
    if (m_textView != nullptr) {
        m_textView->setExternalSelectionRange(std::nullopt);
    }
    if (m_bitmapView != nullptr) {
        m_bitmapView->setExternalSelectionRange(std::nullopt);
    }
}

void MainWindow::navigateToStructSource(const QString& filePath,
                                        quint64 absoluteOffset,
                                        quint64 byteLength) {
    const bool wasNavigating = m_structNavigationInProgress;
    m_structNavigationInProgress = true;
    clearStructSourceHighlight();

    bool sourceReady = true;
    if (!filePath.isEmpty()) {
        const std::optional<int> activeTarget = activePreviewTargetIndex();
        const bool sameSource =
            activeTarget.has_value() &&
            filePathForTarget(activeTarget.value()) == filePath;
        if (!sameSource) {
            sourceReady = applySourcePath(filePath, true);
        }
    }
    if (sourceReady) {
        jumpToAbsoluteOffset(absoluteOffset);
        setStructSourceHighlight(absoluteOffset, byteLength);
    }

    m_structNavigationInProgress = wasNavigating;
}

quint64 MainWindow::structVisualizationStartOffset() const {
    if (m_textView->selectionStartOffset().has_value()) {
        return m_textView->selectionStartOffset().value();
    }
    if (m_textView->selectedOffset().has_value()) {
        return m_textView->selectedOffset().value();
    }
    if (m_textView->firstVisibleByteOffset().has_value()) {
        return m_textView->firstVisibleByteOffset().value();
    }
    return m_textHoverBuffer.baseOffset;
}

bool MainWindow::loadExternalStructSources(
    QHash<QString, VisualizationSource>* sources) {
    if (sources == nullptr) {
        return false;
    }
    sources->clear();
    for (const QString& role : m_structModeLeftPanel->structureGraph().externalRoles()) {
        QString path = m_externalStructSourcePaths.value(role);
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            path = QFileDialog::getOpenFileName(
                this, QStringLiteral("Select external structure source: %1").arg(role),
                AppSettings::lastBrowseDialogDirectory(),
                QStringLiteral("All files (*)"));
            if (path.isEmpty()) {
                return false;
            }
            m_externalStructSourcePaths.insert(role, path);
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(this, QStringLiteral("External source error"),
                                 QStringLiteral("Could not read '%1': %2")
                                     .arg(path, file.errorString()));
            return false;
        }
        VisualizationSource source;
        source.bytes = file.readAll();
        source.filePath = QFileInfo(path).absoluteFilePath();
        sources->insert(role, std::move(source));
    }
    return true;
}

bool MainWindow::decodeStructView(StructViewState& view, bool allowSourceReload) {
    if (!m_structModeLeftPanel->isParseValid() ||
        !m_structModeLeftPanel->structureGraph().isVisualizableEntryName(view.type)) {
        view.reloadError = QStringLiteral("Struct type '%1' is not available").arg(view.type);
        return false;
    }

    QByteArray data;
    size_t dataStartOffset = 0;
    quint64 dataBaseOffset = 0;
    const auto loadSourceWindow = [&]() -> bool {
        if (!allowSourceReload || view.filePath.isEmpty() || view.fileSize <= view.offset) {
            return false;
        }
        const quint64 readSize =
            qMin(kNotEmptyInitialBytes, view.fileSize - view.offset);
        const std::optional<QByteArray> loaded =
            m_windowLoader.loadTransformedWindow(view.filePath, view.fileSize,
                                                  view.offset, readSize,
                                                  currentShiftSettings());
        if (!loaded.has_value()) {
            return false;
        }
        data = *loaded;
        dataStartOffset = 0;
        dataBaseOffset = view.offset;
        return true;
    };
    const quint64 hoverEnd =
        m_textHoverBuffer.baseOffset +
        static_cast<quint64>(m_textHoverBuffer.data.size());
    if (view.filePath == m_textHoverBuffer.filePath &&
        view.offset >= m_textHoverBuffer.baseOffset && view.offset < hoverEnd) {
        const quint64 hoverSuffixBytes = hoverEnd - view.offset;
        const quint64 desiredBytes =
            view.fileSize > view.offset
                ? qMin(kNotEmptyInitialBytes, view.fileSize - view.offset)
                : hoverSuffixBytes;
        if (!allowSourceReload || hoverSuffixBytes >= desiredBytes ||
            !loadSourceWindow()) {
            data = m_textHoverBuffer.data;
            dataBaseOffset = m_textHoverBuffer.baseOffset;
            dataStartOffset =
                static_cast<size_t>(view.offset - m_textHoverBuffer.baseOffset);
        }
    } else {
        loadSourceWindow();
    }
    if (data.isEmpty() || dataStartOffset >= static_cast<size_t>(data.size())) {
        view.reloadError =
            QStringLiteral("Could not load bytes at offset 0x%1")
                .arg(view.offset, 0, 16);
        return false;
    }

    QHash<QString, VisualizationSource> externalSources;
    if (!loadExternalStructSources(&externalSources)) {
        view.reloadError = QStringLiteral("External source selection was cancelled");
        return false;
    }
    VisualizationSource primary{data, view.filePath, dataBaseOffset};
    view.decodedRoot = visualize(
        m_structModeLeftPanel->structureGraph(), view.type, primary,
        dataStartOffset, view.repeat, externalSources,
        dataViewBigEndianEnabled() ? Endianness::Big : Endianness::Little);
    view.reloadError.clear();
    return true;
}

void MainWindow::syncStructPreviewToControls() {
    const quint64 absoluteOffset =
        m_structPreview.has_value() ? m_structPreview->offset
                                    : structVisualizationStartOffset();
    clearStructPreview();
    if (m_structModeLeftPanel->previewEnabled() &&
        m_structModeLeftPanel->canPreview()) {
        createStructPreview(absoluteOffset);
    }
}

void MainWindow::createStructPreview(quint64 absoluteOffset) {
    if (!m_structModeLeftPanel->previewEnabled() ||
        !m_structModeLeftPanel->canPreview()) {
        return;
    }
    StructViewState preview;
    preview.type = m_structModeLeftPanel->entryComboBox()->currentText();
    preview.repeat = m_structModeLeftPanel->entryCountSpinBox()->value();
    preview.offset = absoluteOffset;
    preview.filePath = m_textHoverBuffer.filePath;
    for (const ScanTarget& target : m_scanTargets) {
        if (target.filePath == preview.filePath) {
            preview.fileSize = target.fileSize;
            break;
        }
    }
    if (preview.fileSize == 0) {
        preview.fileSize =
            m_filePool.externalReadSize(preview.filePath)
                .value_or(m_textHoverBuffer.baseOffset +
                          static_cast<quint64>(m_textHoverBuffer.data.size()));
    }
    if (!decodeStructView(preview, false)) {
        return;
    }
    m_structPreview = preview;
    m_structModeLeftPanel->setPreviewActive(true);
    rebuildStructVisualization();
    AppSettings::setStructDeclarationText(m_structModeLeftPanel->declarationText());
    AppSettings::setStructEntryName(preview.type);
    AppSettings::setStructEntryCount(preview.repeat);
}

void MainWindow::clearStructPreview() {
    if (!m_structPreview.has_value() &&
        (m_structModeLeftPanel == nullptr ||
         !m_structModeLeftPanel->previewActive())) {
        return;
    }
    clearStructSourceHighlight();
    m_structPreview.reset();
    if (m_structModeLeftPanel != nullptr) {
        m_structModeLeftPanel->setPreviewActive(false);
    }
    rebuildStructVisualization();
}

void MainWindow::addCurrentStructView() {
    if (!m_structPreview.has_value()) {
        return;
    }
    StructViewState view = *m_structPreview;
    view.id = m_nextStructViewId++;
    view.name =
        QStringLiteral("%1@0x%2")
            .arg(view.type, QString::number(view.offset, 16).toUpper());
    m_currentStructViews.push_back(view);
    m_structModeLeftPanel->addCurrentView(
        CurrentStructView{view.id, view.name, view.type, view.repeat,
                          view.offset, view.decodedRoot.sourceLength,
                          view.filePath});
    rebuildStructVisualization();
}

void MainWindow::removeCurrentStructViews(const QVector<quint64>& ids) {
    m_currentStructViews.erase(
        std::remove_if(m_currentStructViews.begin(), m_currentStructViews.end(),
                       [&ids](const StructViewState& view) {
                           return ids.contains(view.id);
                       }),
        m_currentStructViews.end());
    rebuildStructVisualization();
}

void MainWindow::updateCurrentStructView(quint64 id, const QString& name,
                                         int repeat, quint64 offset) {
    auto found =
        std::find_if(m_currentStructViews.begin(), m_currentStructViews.end(),
                     [id](const StructViewState& view) { return view.id == id; });
    if (found == m_currentStructViews.end()) {
        return;
    }
    const bool requiresDecode = found->repeat != repeat || found->offset != offset;
    found->name = name;
    if (requiresDecode) {
        StructViewState updated = *found;
        updated.repeat = repeat;
        updated.offset = offset;
        decodeStructView(updated, true);
        *found = updated;
        m_structModeLeftPanel->setCurrentViewByteLength(
            found->id, found->decodedRoot.sourceLength);
    }
    rebuildStructVisualization();
}

void MainWindow::rebuildStructVisualization() {
    if (m_structDataViewPanel == nullptr) {
        return;
    }
    VisualizedNode root;
    root.name = QStringLiteral("root");
    m_structDataViewPanel->setSourceEndianness(
        dataViewBigEndianEnabled() ? Endianness::Big : Endianness::Little);
    const auto applyViewSource = [](VisualizedNode* node,
                                    const StructViewState& view) {
        node->sourceFilePath = view.filePath;
        node->sourceOffset = view.offset;
        node->sourceLength = view.decodedRoot.sourceLength;
        node->hasSourceOffset = true;
    };
    const auto buildViewNode = [this, &applyViewSource](
                                   const StructViewState& view,
                                   const QString& viewName) {
        VisualizedNode node;
        node.name = viewName;
        node.typeName = view.type;
        node.declarationRange =
            m_structModeLeftPanel->structureGraph().nameRangeForEntry(view.type);
        node.valueKind = view.repeat == 1 ? VisualizedValueKind::Object
                                         : VisualizedValueKind::Array;
        applyViewSource(&node, view);
        if (view.repeat == 1 && view.decodedRoot.children.size() == 1 &&
            view.decodedRoot.children.first().valueKind ==
                VisualizedValueKind::Object) {
            node.children = view.decodedRoot.children.first().children;
        } else {
            node.children = view.decodedRoot.children;
        }
        if (!view.reloadError.isEmpty()) {
            node.valid = false;
            node.errorMessage = view.reloadError;
        }
        return node;
    };
    const auto appendView = [&root, &buildViewNode](const StructViewState& view,
                                                    const QString& viewName) {
        root.children.push_back(buildViewNode(view, viewName));
    };
    if (m_structPreview.has_value()) {
        appendView(*m_structPreview, QStringLiteral("Preview"));
    }
    for (const StructViewState& view : m_currentStructViews) {
        appendView(view, view.name);
    }
    if (root.children.isEmpty()) {
        m_structDataViewPanel->setOutforms(
            m_structModeLeftPanel->structureGraph().outforms());
        m_structDataViewPanel->clearVisualization();
    } else {
        m_structDataViewPanel->setOutforms(
            m_structModeLeftPanel->structureGraph().outforms());
        m_structDataViewPanel->setVisualization(root);
    }
}

void MainWindow::onHoverLeft() {
    m_bitmapView->setExternalHoverOffset(std::nullopt);
    m_textView->setHoverAnchorOffset(std::nullopt);
    clearCurrentByteInfo();
}

}  // namespace breco
