#include "panel/VisualizePanel.h"

#include <cmath>
#include <QDir>
#include <QDockWidget>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "brecolang/gui/BrecoLangLibrary.h"
#include "ui_VisualizePanel.h"
#include "view/Cartesian2DView.h"
#include "view/Cartesian3DView.h"
#include "view/VisualizeBitmapCanvas.h"

namespace breco {

namespace {

void installView(QWidget* page, QWidget* view) {
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(view);
}

QString rangeText(quint64 start, qsizetype size) {
    if (size <= 0) {
        return QStringLiteral("No data");
    }
    const quint64 end = start + static_cast<quint64>(size) - 1;
    return QStringLiteral("0x%1–0x%2 (%3 bytes)")
        .arg(start, 0, 16)
        .arg(end, 0, 16)
        .arg(size);
}

}  // namespace

VisualizePanel::VisualizePanel(QWidget* parent)
    : QWidget(parent), m_ui(std::make_unique<Ui::VisualizePanel>()) {
    m_ui->setupUi(this);

    m_workspaceWindow = new QMainWindow(this);
    m_workspaceWindow->setObjectName(QStringLiteral("visualizeWorkspace"));
    m_workspaceWindow->setWindowFlag(Qt::Window, false);
    m_workspaceWindow->setDockNestingEnabled(true);
    auto* centralPlaceholder = new QWidget(m_workspaceWindow);
    centralPlaceholder->setObjectName(
        QStringLiteral("visualizeWorkspaceCentral"));
    centralPlaceholder->setMaximumSize(0, 0);
    m_workspaceWindow->setCentralWidget(centralPlaceholder);

    m_schemaDock =
        new QDockWidget(QStringLiteral("Visualize.breco"), m_workspaceWindow);
    m_schemaDock->setObjectName(QStringLiteral("visualizeSchemaDock"));
    m_schemaDock->setMinimumSize(120, 80);
    updateSchemaDockFeatures(false);
    m_schemaEditor = new QPlainTextEdit(m_schemaDock);
    m_schemaEditor->setObjectName(QStringLiteral("visualizeSchemaEditor"));
    m_schemaEditor->setPlaceholderText(
        QStringLiteral("Empty uses the built-in Visualize program"));
    m_schemaDock->setWidget(m_schemaEditor);

    m_resultDock = new QDockWidget(QStringLiteral("Result"), m_workspaceWindow);
    m_resultDock->setObjectName(QStringLiteral("visualizeResultDock"));
    m_resultDock->setMinimumSize(120, 80);
    updateResultDockFeatures(false);
    m_ui->verticalLayout->removeWidget(m_ui->canvasStack);
    m_resultDock->setWidget(m_ui->canvasStack);
    m_workspaceWindow->addDockWidget(Qt::LeftDockWidgetArea, m_schemaDock);
    m_workspaceWindow->addDockWidget(Qt::LeftDockWidgetArea, m_resultDock);
    m_workspaceWindow->splitDockWidget(m_schemaDock, m_resultDock,
                                       Qt::Horizontal);
    m_ui->verticalLayout->addWidget(m_workspaceWindow, 1);
    m_workspaceWindow->installEventFilter(this);
    m_schemaDock->installEventFilter(this);
    m_resultDock->installEventFilter(this);
    connect(m_schemaDock, &QDockWidget::topLevelChanged, this,
            [this](bool floating) { updateSchemaDockFeatures(floating); });
    connect(m_resultDock, &QDockWidget::topLevelChanged, this,
            [this](bool floating) { updateResultDockFeatures(floating); });

    m_programTimer = new QTimer(this);
    m_programTimer->setSingleShot(true);
    m_programTimer->setInterval(300);
    connect(m_programTimer, &QTimer::timeout, this, [this]() {
        saveProgram();
        rebuildVisualization();
    });
    connect(m_schemaEditor, &QPlainTextEdit::textChanged, this, [this]() {
        m_schemaSource = m_schemaEditor->toPlainText();
        m_programDirty = true;
        m_programTimer->start();
    });

    m_cartesian2DView = new Cartesian2DView(m_ui->cartesian2DPage);
    m_bitmapCanvas = new VisualizeBitmapCanvas(m_ui->bitmapPage);
    installView(m_ui->cartesian2DPage, m_cartesian2DView);
    installView(m_ui->bitmapPage, m_bitmapCanvas);

    const auto changeMode = [this]() {
        updateControlVisibility();
        rebuildVisualization();
        emit configurationChanged();
    };
    connect(m_ui->cartesian2DRadioButton, &QRadioButton::toggled, this,
            [changeMode](bool checked) {
                if (checked) {
                    changeMode();
                }
            });
    connect(m_ui->cartesian3DRadioButton, &QRadioButton::toggled, this,
            [changeMode](bool checked) {
                if (checked) {
                    changeMode();
                }
            });
    connect(m_ui->bitmapRadioButton, &QRadioButton::toggled, this,
            [changeMode](bool checked) {
                if (checked) {
                    changeMode();
                }
            });
    connect(m_bitmapCanvas, &VisualizeBitmapCanvas::inputExtentResized, this,
            &VisualizePanel::handleInputExtentResized);

    updateControlVisibility();
    setProgramDirectory({});
}

VisualizePanel::~VisualizePanel() { flushProgram(); }

bool VisualizePanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_workspaceWindow &&
        (event->type() == QEvent::Show || event->type() == QEvent::Resize)) {
        ensureWorkspaceLayout();
    }
    if ((watched == m_schemaDock || watched == m_resultDock) &&
        event->type() == QEvent::Close) {
        event->ignore();
        auto* dock = static_cast<QDockWidget*>(watched);
        if (dock->isFloating()) {
            dock->setFloating(false);
        }
        dock->show();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void VisualizePanel::ensureWorkspaceLayout() {
    if (m_workspaceLayoutApplied || m_workspaceWindow == nullptr ||
        m_schemaDock == nullptr || m_resultDock == nullptr) {
        return;
    }
    const int width = m_workspaceWindow->width();
    if (width < 50) {
        return;
    }
    m_workspaceLayoutApplied = true;
    m_workspaceWindow->resizeDocks({m_schemaDock, m_resultDock},
                                   {width / 3, width * 2 / 3},
                                   Qt::Horizontal);
}

void VisualizePanel::updateSchemaDockFeatures(bool floating) {
    QDockWidget::DockWidgetFeatures features =
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    if (floating) {
        features |= QDockWidget::DockWidgetClosable;
    }
    m_schemaDock->setFeatures(features);
}

void VisualizePanel::updateResultDockFeatures(bool floating) {
    QDockWidget::DockWidgetFeatures features =
        QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable;
    if (floating) {
        features |= QDockWidget::DockWidgetClosable;
    }
    m_resultDock->setFeatures(features);
}

void VisualizePanel::setProgramDirectory(const QString& directory) {
    flushProgram();
    const QString root =
        directory.isEmpty() ? lang::BrecoLangLibrary::defaultDirectory()
                            : QDir::cleanPath(directory);
    m_programPath = QDir(root).filePath(QStringLiteral("Visualize.breco"));
    loadProgram();
}

void VisualizePanel::flushProgram() {
    if (m_programPath.isEmpty()) {
        return;
    }
    if (m_schemaSource.trimmed().isEmpty() ||
        m_schemaSource == builtinVisualizeProgramSource()) {
        if (m_programDirty) {
            QFile::remove(m_programPath);
            m_programDirty = false;
        }
        return;
    }
    saveProgram();
}

void VisualizePanel::loadProgram() {
    if (m_programTimer != nullptr) {
        m_programTimer->stop();
    }
    QFile file(m_programPath);
    if (file.open(QIODevice::ReadOnly)) {
        m_schemaSource = QString::fromUtf8(file.readAll());
        m_programDirty = false;
    } else {
        m_schemaSource = builtinVisualizeProgramSource();
        m_programDirty = false;
    }
    const QSignalBlocker blocker(m_schemaEditor);
    m_schemaEditor->setPlainText(m_schemaSource);
    rebuildVisualization();
}

void VisualizePanel::saveProgram() {
    if (!m_programDirty || m_programPath.isEmpty()) {
        return;
    }
    const QString source = m_schemaSource;
    if (source.trimmed().isEmpty() ||
        source == builtinVisualizeProgramSource()) {
        return;
    }
    QString error;
    if (!lang::BrecoLangLibrary::ensureDirectory(
            QFileInfo(m_programPath).absolutePath(), &error)) {
        m_ui->statusLabel->setText(error);
        return;
    }
    QSaveFile file(m_programPath);
    if (!file.open(QIODevice::WriteOnly)) {
        m_ui->statusLabel->setText(file.errorString());
        return;
    }
    file.write(source.toUtf8());
    if (!file.commit()) {
        m_ui->statusLabel->setText(file.errorString());
        return;
    }
    m_programDirty = false;
}

void VisualizePanel::setData(QByteArray bytes, quint64 baseOffset,
                             bool truncated, quint64 fileSize) {
    m_bytes = std::move(bytes);
    m_baseOffset = baseOffset;
    m_truncated = truncated;
    m_fileSize = fileSize;
    rebuildVisualization();
}

void VisualizePanel::clearData() {
    m_bytes.clear();
    m_baseOffset = 0;
    m_fileSize = 0;
    m_truncated = false;
    m_cartesian2DView->setPoints({});
    if (m_cartesian3DView != nullptr) {
        m_cartesian3DView->setPoints({});
    }
    m_bitmapCanvas->clear();
    m_ui->statusLabel->setText(QStringLiteral("No source selected."));
}

VisualizationMode VisualizePanel::visualizationMode() const {
    if (m_ui->cartesian3DRadioButton->isChecked()) {
        return VisualizationMode::Cartesian3D;
    }
    if (m_ui->bitmapRadioButton->isChecked()) {
        return VisualizationMode::Bitmap;
    }
    return VisualizationMode::Cartesian2D;
}

void VisualizePanel::setVisualizationMode(VisualizationMode mode) {
    QRadioButton* button = m_ui->cartesian2DRadioButton;
    if (mode == VisualizationMode::Cartesian3D) {
        button = m_ui->cartesian3DRadioButton;
    } else if (mode == VisualizationMode::Bitmap) {
        button = m_ui->bitmapRadioButton;
    }
    button->setChecked(true);
    updateControlVisibility();
}

QString VisualizePanel::schemaText() const {
    return m_schemaEditor != nullptr ? m_schemaEditor->toPlainText()
                                     : m_schemaSource;
}

void VisualizePanel::setSchemaText(const QString& text) {
    m_schemaSource = text;
    const QSignalBlocker blocker(m_schemaEditor);
    m_schemaEditor->setPlainText(text);
    m_programDirty = true;
    saveProgram();
    rebuildVisualization();
}

QRadioButton* VisualizePanel::cartesian2DRadioButton() const {
    return m_ui->cartesian2DRadioButton;
}

QRadioButton* VisualizePanel::cartesian3DRadioButton() const {
    return m_ui->cartesian3DRadioButton;
}

QRadioButton* VisualizePanel::bitmapRadioButton() const {
    return m_ui->bitmapRadioButton;
}

QLabel* VisualizePanel::statusLabel() const { return m_ui->statusLabel; }

Cartesian3DView* VisualizePanel::ensureCartesian3DView() {
    if (m_cartesian3DView == nullptr) {
        m_cartesian3DView =
            new Cartesian3DView(m_ui->cartesian3DPage);
        installView(m_ui->cartesian3DPage, m_cartesian3DView);
    }
    return m_cartesian3DView;
}

void VisualizePanel::updateControlVisibility() {
    const VisualizationMode mode = visualizationMode();
    if (mode == VisualizationMode::Cartesian3D) {
        ensureCartesian3DView();
    }
    m_ui->canvasStack->setCurrentWidget(
        mode == VisualizationMode::Cartesian2D
            ? m_ui->cartesian2DPage
            : (mode == VisualizationMode::Cartesian3D
                   ? m_ui->cartesian3DPage
                   : m_ui->bitmapPage));
}

quint64 VisualizePanel::availableStart() const {
    return m_fileSize > 0 ? 0 : m_baseOffset;
}

quint64 VisualizePanel::availableEnd() const {
    if (m_fileSize > 0) {
        return m_fileSize;
    }
    return m_baseOffset + static_cast<quint64>(m_bytes.size());
}

void VisualizePanel::handleInputExtentResized(double areaScale, bool keepEnd) {
    if (m_bytes.isEmpty() || !std::isfinite(areaScale) || areaScale <= 0.0) {
        return;
    }
    const quint64 currentLength = static_cast<quint64>(m_bytes.size());
    const quint64 currentEnd = m_baseOffset + currentLength;
    quint64 newLength = qBound<quint64>(
        1,
        static_cast<quint64>(
            std::llround(static_cast<double>(currentLength) * areaScale)),
        kMaximumVisualizationBytes);
    const quint64 startBound = availableStart();
    const quint64 endBound = availableEnd();
    quint64 newStart = m_baseOffset;
    if (keepEnd) {
        if (currentEnd > newLength) {
            newStart = currentEnd - newLength;
        } else {
            newStart = startBound;
            newLength = qMin(newLength, currentEnd - startBound);
        }
    }
    if (newStart < startBound) {
        newStart = startBound;
    }
    if (endBound > newStart) {
        newLength = qMin(newLength, endBound - newStart);
    } else {
        newLength = 1;
    }
    newLength = qMax<quint64>(1, newLength);
    if (newStart == m_baseOffset && newLength == currentLength) {
        return;
    }

    const bool canSlice =
        newStart >= m_baseOffset &&
        newStart - m_baseOffset + newLength <= currentLength;
    if (canSlice) {
        const qsizetype localOffset =
            static_cast<qsizetype>(newStart - m_baseOffset);
        setData(m_bytes.mid(localOffset, static_cast<qsizetype>(newLength)),
                newStart, m_truncated, m_fileSize);
        return;
    }
    emit inputWindowRequested(newStart, newLength);
}

void VisualizePanel::rebuildVisualization() {
    const QString range = rangeText(m_baseOffset, m_bytes.size());
    if (m_bytes.isEmpty()) {
        const VisualizationConfigurationResult configuration =
            readVisualizationConfiguration(schemaText());
        if (configuration.success()) {
            updateDefaultWindowBytes(
                configuration.config.numBytesOnNoSelection);
            m_ui->statusLabel->setText(QStringLiteral("No source selected."));
        } else {
            m_ui->statusLabel->setText(configuration.error);
        }
        return;
    }

    const VisualizationMode mode = visualizationMode();
    const VisualizationDecodeResult result =
        decodeVisualization(schemaText(), m_bytes, m_baseOffset, mode);
    if (!result.success()) {
        m_ui->statusLabel->setText(
            QStringLiteral("%1 — %2").arg(range, result.error));
        return;
    }

    updateDefaultWindowBytes(result.config.numBytesOnNoSelection);
    m_cartesian2DView->setStyle(result.config.style);
    m_cartesian2DView->setTickDistance(result.config.tickDistance);
    if (m_cartesian3DView != nullptr) {
        m_cartesian3DView->setStyle(result.config.style);
        m_cartesian3DView->setTickDistance(result.config.tickDistance);
    }

    const QString fallback =
        result.usedBuiltinRecord
            ? QStringLiteral(" — using built-in %1")
                  .arg(mode == VisualizationMode::Cartesian2D
                           ? QStringLiteral("Cart2D")
                           : (mode == VisualizationMode::Cartesian3D
                                  ? QStringLiteral("Cart3D")
                                  : QStringLiteral("Bitmap")))
            : QString();
    const QString truncated =
        m_truncated ? QStringLiteral(" — selection truncated to 8 MiB")
                    : QString();

    if (mode == VisualizationMode::Bitmap) {
        m_bitmapCanvas->setVisualization(
            result.bitmapPixels, result.bitmapPackedBits,
            result.bitmapBitsPerPixel,
            result.bitmapHasPlot);
        const qsizetype pixelCount =
            result.bitmapPackedBits.isEmpty()
                ? result.bitmapPixels.size()
                : result.bitmapPackedBits.size() * 8;
        m_ui->statusLabel->setText(
            QStringLiteral("%1 — %2 pixels%3%4")
                .arg(range)
                .arg(pixelCount)
                .arg(fallback, truncated));
        return;
    }

    if (mode == VisualizationMode::Cartesian3D) {
        ensureCartesian3DView()->setPoints(result.points);
    } else {
        m_cartesian2DView->setPoints(result.points);
    }
    m_ui->statusLabel->setText(
        QStringLiteral("%1 — %2 points%3%4")
            .arg(range)
            .arg(result.points.size())
            .arg(fallback, truncated));
}

void VisualizePanel::updateDefaultWindowBytes(quint64 bytes) {
    bytes = qMin(bytes, kMaximumVisualizationBytes);
    if (m_defaultWindowBytes == bytes) {
        return;
    }
    m_defaultWindowBytes = bytes;
    emit defaultWindowBytesChanged();
}

}  // namespace breco
