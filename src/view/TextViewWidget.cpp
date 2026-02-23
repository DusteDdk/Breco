#include "view/TextViewWidget.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>

#include "debug/SelectionTrace.h"
#include "text/StringModeRules.h"

namespace breco {

namespace {
bool isPrintableAscii(unsigned char byte) { return byte >= 0x20 && byte <= 0x7E; }

bool decodeUtf8At(const QByteArray& bytes, int index, quint32* codepointOut, int* lengthOut) {
    if (index < 0 || index >= bytes.size()) {
        return false;
    }

    const unsigned char b0 = static_cast<unsigned char>(bytes.at(index));
    if ((b0 & 0x80U) == 0U) {
        if (codepointOut != nullptr) {
            *codepointOut = static_cast<quint32>(b0);
        }
        if (lengthOut != nullptr) {
            *lengthOut = 1;
        }
        return true;
    }

    int length = 0;
    quint32 cp = 0;
    quint32 minCp = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        length = 2;
        cp = static_cast<quint32>(b0 & 0x1FU);
        minCp = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        length = 3;
        cp = static_cast<quint32>(b0 & 0x0FU);
        minCp = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        length = 4;
        cp = static_cast<quint32>(b0 & 0x07U);
        minCp = 0x10000U;
    } else {
        return false;
    }

    if (index + length > bytes.size()) {
        return false;
    }
    for (int i = 1; i < length; ++i) {
        const unsigned char bx = static_cast<unsigned char>(bytes.at(index + i));
        if ((bx & 0xC0U) != 0x80U) {
            return false;
        }
        cp = (cp << 6U) | static_cast<quint32>(bx & 0x3FU);
    }
    if (cp < minCp || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return false;
    }

    if (codepointOut != nullptr) {
        *codepointOut = cp;
    }
    if (lengthOut != nullptr) {
        *lengthOut = length;
    }
    return true;
}

quint16 readUtf16Unit(const QByteArray& bytes, int index, bool littleEndian) {
    if (index + 1 >= bytes.size()) {
        return 0;
    }
    const unsigned char b0 = static_cast<unsigned char>(bytes.at(index));
    const unsigned char b1 = static_cast<unsigned char>(bytes.at(index + 1));
    if (littleEndian) {
        return static_cast<quint16>(b0 | (static_cast<quint16>(b1) << 8U));
    }
    return static_cast<quint16>((static_cast<quint16>(b0) << 8U) | b1);
}

bool decodeUtf16At(const QByteArray& bytes, int index, bool littleEndian, quint32* codepointOut,
                  int* lengthOut) {
    if (index < 0 || index + 1 >= bytes.size()) {
        return false;
    }

    const quint16 u0 = readUtf16Unit(bytes, index, littleEndian);
    if (u0 >= 0xD800U && u0 <= 0xDBFFU) {
        if (index + 3 >= bytes.size()) {
            return false;
        }
        const quint16 u1 = readUtf16Unit(bytes, index + 2, littleEndian);
        if (u1 < 0xDC00U || u1 > 0xDFFFU) {
            return false;
        }
        const quint32 cp = 0x10000U +
                           ((static_cast<quint32>(u0 - 0xD800U) << 10U) |
                            static_cast<quint32>(u1 - 0xDC00U));
        if (codepointOut != nullptr) {
            *codepointOut = cp;
        }
        if (lengthOut != nullptr) {
            *lengthOut = 4;
        }
        return true;
    }
    if (u0 >= 0xDC00U && u0 <= 0xDFFFU) {
        return false;
    }

    if (codepointOut != nullptr) {
        *codepointOut = static_cast<quint32>(u0);
    }
    if (lengthOut != nullptr) {
        *lengthOut = 2;
    }
    return true;
}

QString glyphFromCodepoint(quint32 cp) {
    if (cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
        return {};
    }
    const char32_t u = static_cast<char32_t>(cp);
    return QString::fromUcs4(&u, 1);
}
}

TextViewWidget::TextViewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(480);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);

    m_gutterWidget = new QWidget(this);
    m_contentWidget = new QWidget(this);
    m_vScrollBar = new QScrollBar(Qt::Vertical, this);
    m_hScrollBar = new QScrollBar(Qt::Horizontal, this);
    m_gutterWidget->installEventFilter(this);
    m_contentWidget->installEventFilter(this);
    m_contentWidget->setMouseTracking(true);
    m_gutterWidget->setMouseTracking(true);

    connect(m_vScrollBar, &QScrollBar::valueChanged, this, [this]() {
        m_contentWidget->update();
        m_gutterWidget->update();
        emitCenterAnchorOffset();
    });
    connect(m_hScrollBar, &QScrollBar::valueChanged, this,
            [this]() { m_contentWidget->update(); });

    layoutChildren();
}

void TextViewWidget::setMode(TextInterpretationMode mode) {
    m_mode = mode;
    rebuildLines();
    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
}

void TextViewWidget::setDisplayMode(TextDisplayMode mode) {
    if (m_displayMode == mode) {
        return;
    }
    m_displayMode = mode;
    if (m_displayMode == TextDisplayMode::ByteMode) {
        m_wrapMode = true;
    }
    rebuildLines();
    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
}

void TextViewWidget::setData(const QByteArray& bytes, quint64 baseOffset,
                             std::optional<unsigned char> previousByteBeforeBase) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setData: start bytes=%1 baseOffset=%2")
                           .arg(bytes.size())
                           .arg(baseOffset));
    }
    m_backingBytes = bytes;
    m_backingBaseOffset = baseOffset;
    m_previousByteBeforeBackingBase = previousByteBeforeBase;
    m_previousByteBeforeBase = previousByteBeforeBase;
    m_baseOffset = baseOffset;
    m_selectedOffset = baseOffset;
    m_matchStartOffset = baseOffset;
    m_matchLength = 0;
    m_hasSelectedOffset = false;
    m_hasSelection = false;
    m_selectionStartVisibleIndex = -1;
    m_selectionEndVisibleIndex = -1;
    m_selecting = false;
    m_lastHoveredAbsoluteOffset = -1;
    const quint64 viewportStartUs = debug::selectionTraceElapsedUs();
    setViewportWindow(baseOffset, true);
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setData: setViewportWindow elapsed=%1us")
                           .arg(debug::selectionTraceElapsedUs() - viewportStartUs));
    }
    m_vScrollBar->setValue(0);
    m_hScrollBar->setValue(0);
    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
    emitSelectionRangeChanged();
    BRECO_SELTRACE("TextViewWidget::setData: done");
}

int TextViewWidget::viewportByteCapacity() const {
    const int visibleLines = qMax(1, visibleLineCount());
    const QFontMetrics fm(font());
    const int contentWidth = qMax(32, m_contentWidget->width() - 16);

    int bytesPerLine = 0;
    if (m_displayMode == TextDisplayMode::ByteMode) {
        bytesPerLine = fixedBytesPerLine();
        if (bytesPerLine <= 0) {
            const int byteCellWidth = qMax(
                                          fm.horizontalAdvance(QStringLiteral("00")),
                                          fm.horizontalAdvance(QStringLiteral("FF"))) +
                                      12;
            bytesPerLine = qMax(1, contentWidth / qMax(1, byteCellWidth));
        }
    } else {
        const int avgCharWidth = qMax(1, fm.horizontalAdvance(QStringLiteral("M")));
        bytesPerLine = qMax(8, contentWidth / avgCharWidth);
    }

    const int visibleEstimate = qMax(1, visibleLines * bytesPerLine);
    // Keep 4 screens of context around the anchor; deterministic by geometry only.
    return qBound(1024, visibleEstimate * 4, 1024 * 1024);
}

bool TextViewWidget::setViewportWindow(quint64 desiredStartOffset, bool forceRebuild) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setViewportWindow: start desiredStart=%1 force=%2 backing=%3")
                           .arg(desiredStartOffset)
                           .arg(forceRebuild ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(m_backingBytes.size()));
    }
    if (m_backingBytes.isEmpty()) {
        m_bytes.clear();
        m_byteClasses.clear();
        m_visibleOffsets.clear();
        m_lines.clear();
        m_stringVisibilityMask.clear();
        m_baseOffset = m_backingBaseOffset;
        m_previousByteBeforeBase = m_previousByteBeforeBackingBase;
        updateScrollRange();
        BRECO_SELTRACE("TextViewWidget::setViewportWindow: empty backing, cleared and return false");
        return false;
    }

    const quint64 backingStart = m_backingBaseOffset;
    const quint64 backingSize = static_cast<quint64>(qMax(0, m_backingBytes.size()));
    const quint64 viewportSize = qMin<quint64>(viewportByteCapacity(), backingSize);
    if (viewportSize == 0) {
        m_bytes.clear();
        const quint64 rebuildStartUs = debug::selectionTraceElapsedUs();
        rebuildLines();
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral("TextViewWidget::setViewportWindow: viewportSize=0 rebuild elapsed=%1us")
                               .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
        }
        return false;
    }

    const quint64 maxStart = backingStart + (backingSize - viewportSize);
    const quint64 clampedStart = qBound(backingStart, desiredStartOffset, maxStart);
    const int relStart = static_cast<int>(clampedStart - backingStart);
    const int len = static_cast<int>(viewportSize);
    const bool changedWindow = (m_baseOffset != clampedStart || m_bytes.size() != len);
    if (!forceRebuild && !changedWindow) {
        BRECO_SELTRACE("TextViewWidget::setViewportWindow: unchanged window, return false");
        return false;
    }

    m_baseOffset = clampedStart;
    m_bytes = m_backingBytes.mid(relStart, len);
    if (relStart > 0) {
        m_previousByteBeforeBase = static_cast<unsigned char>(m_backingBytes.at(relStart - 1));
    } else {
        m_previousByteBeforeBase = m_previousByteBeforeBackingBase;
    }
    const quint64 rebuildStartUs = debug::selectionTraceElapsedUs();
    rebuildLines();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral(
                           "TextViewWidget::setViewportWindow: rebuilt base=%1 len=%2 changed=%3 elapsed=%4us")
                           .arg(m_baseOffset)
                           .arg(m_bytes.size())
                           .arg(changedWindow ? QStringLiteral("true") : QStringLiteral("false"))
                           .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
    }
    return changedWindow;
}

bool TextViewWidget::ensureOffsetInViewport(quint64 absoluteOffset, bool centerInView) {
    if (m_backingBytes.isEmpty()) {
        return false;
    }

    const quint64 backingStart = m_backingBaseOffset;
    const quint64 backingEnd =
        backingStart + static_cast<quint64>(qMax(0, m_backingBytes.size()));
    if (backingEnd <= backingStart) {
        return false;
    }
    if (absoluteOffset < backingStart) {
        absoluteOffset = backingStart;
    } else if (absoluteOffset >= backingEnd) {
        absoluteOffset = backingEnd - 1;
    }

    const quint64 currentStart = m_baseOffset;
    const quint64 currentSize = static_cast<quint64>(qMax(0, m_bytes.size()));
    const bool insideCurrent =
        currentSize > 0 && absoluteOffset >= currentStart && absoluteOffset < currentStart + currentSize;
    if (insideCurrent && !centerInView) {
        return false;
    }

    const quint64 viewportSize = qMin<quint64>(
        (currentSize > 0) ? currentSize : static_cast<quint64>(viewportByteCapacity()),
        static_cast<quint64>(qMax(0, m_backingBytes.size())));
    quint64 desiredStart = backingStart;
    if (absoluteOffset > (viewportSize / 2)) {
        desiredStart = absoluteOffset - (viewportSize / 2);
    }
    return setViewportWindow(desiredStart);
}

bool TextViewWidget::shiftViewportByBytes(qint64 signedBytes) {
    if (signedBytes == 0 || m_backingBytes.isEmpty()) {
        return false;
    }

    const quint64 backingStart = m_backingBaseOffset;
    const quint64 backingSize = static_cast<quint64>(qMax(0, m_backingBytes.size()));
    const quint64 viewportSize =
        qMin<quint64>(static_cast<quint64>(qMax(1, m_bytes.size())), backingSize);
    if (viewportSize == 0 || backingSize <= viewportSize) {
        return false;
    }

    const quint64 currentStart = m_baseOffset;
    const quint64 maxStart = backingStart + (backingSize - viewportSize);
    quint64 desiredStart = currentStart;
    if (signedBytes < 0) {
        const quint64 delta = static_cast<quint64>(-signedBytes);
        const quint64 distanceToStart = currentStart - backingStart;
        desiredStart = (delta >= distanceToStart) ? backingStart : (currentStart - delta);
    } else {
        const quint64 delta = static_cast<quint64>(signedBytes);
        const quint64 distanceToEnd = maxStart - currentStart;
        desiredStart = (delta >= distanceToEnd) ? maxStart : (currentStart + delta);
    }
    if (desiredStart == currentStart) {
        return false;
    }

    if (!setViewportWindow(desiredStart)) {
        return false;
    }

    if (signedBytes < 0) {
        m_vScrollBar->setValue(qMax(0, m_lines.size() - visibleLineCount()));
    } else {
        m_vScrollBar->setValue(0);
    }
    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
    return true;
}

void TextViewWidget::setSelectedOffset(quint64 absoluteOffset, bool centerInView) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setSelectedOffset: start offset=%1 center=%2")
                           .arg(absoluteOffset)
                           .arg(centerInView ? QStringLiteral("true") : QStringLiteral("false")));
    }
    const quint64 startUs = debug::selectionTraceElapsedUs();
    m_selectedOffset = absoluteOffset;
    m_hasSelectedOffset = true;
    ensureOffsetInViewport(absoluteOffset, centerInView);

    const int lineIdx = lineIndexForOffset(absoluteOffset);
    if (centerInView && lineIdx >= 0 && lineIdx < m_lines.size()) {
        const int firstLineTarget = qMax(0, lineIdx - (visibleLineCount() / 2));
        m_vScrollBar->setValue(firstLineTarget);

        const bool allowHScroll =
            !(m_displayMode == TextDisplayMode::ByteMode || m_wrapMode);
        if (allowHScroll) {
            const int x = xOffsetForAbsoluteOffset(m_lines.at(lineIdx), absoluteOffset);
            const int target = qMax(0, x - (m_contentWidget->width() / 2));
            m_hScrollBar->setValue(target);
        }
    }

    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setSelectedOffset: done elapsed=%1us")
                           .arg(debug::selectionTraceElapsedUs() - startUs));
    }
}

void TextViewWidget::setMatchRange(quint64 startOffset, quint32 length) {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::setMatchRange: start=%1 length=%2")
                           .arg(startOffset)
                           .arg(length));
    }
    m_matchStartOffset = startOffset;
    m_matchLength = length;
    m_contentWidget->update();
}

void TextViewWidget::setGutterVisible(bool visible) {
    m_gutterVisible = visible;
    layoutChildren();
    m_gutterWidget->setVisible(visible);
    updateScrollRange();
    m_contentWidget->update();
}

void TextViewWidget::setNewlineMode(TextNewlineMode mode) {
    m_newlineMode = mode;
    if (m_displayMode == TextDisplayMode::StringMode) {
        rebuildLines();
        m_contentWidget->update();
        m_gutterWidget->update();
        emitCenterAnchorOffset();
    }
}

void TextViewWidget::setWrapMode(bool enabled) {
    const bool effective = (m_displayMode == TextDisplayMode::ByteMode) ? true : enabled;
    if (m_wrapMode == effective) {
        return;
    }
    m_wrapMode = effective;
    rebuildLines();
    m_contentWidget->update();
    m_gutterWidget->update();
    emitCenterAnchorOffset();
}

void TextViewWidget::setByteLineMode(ByteLineMode mode) {
    m_byteLineMode = mode;
    if (m_displayMode == TextDisplayMode::ByteMode) {
        rebuildLines();
        m_contentWidget->update();
        m_gutterWidget->update();
        emitCenterAnchorOffset();
    }
}

void TextViewWidget::setMonospaceEnabled(bool enabled) {
    if (m_monospaceEnabled == enabled) {
        return;
    }
    m_monospaceEnabled = enabled;
    if (enabled) {
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    } else {
        setFont(QFont());
    }
    rebuildLines();
    m_contentWidget->update();
    m_gutterWidget->update();
}

int TextViewWidget::visibleByteCount() const {
    if (m_lines.isEmpty()) {
        return qMax(0, m_visibleOffsets.size());
    }

    const int first = firstVisibleLine();
    const int visible = visibleLineCount();
    int sum = 0;
    for (int i = 0; i < visible && first + i < m_lines.size(); ++i) {
        sum += qMax(0, m_lines.at(first + i).byteLength);
    }
    if (sum <= 0) {
        return qMax(0, m_visibleOffsets.size());
    }
    return sum;
}

int TextViewWidget::scrollBytesPerWheelStepHint() const {
    if (m_lines.isEmpty()) {
        return 1;
    }

    const int first = firstVisibleLine();
    const int visible = visibleLineCount();
    int visibleLines = 0;
    int visibleBytes = 0;
    for (int i = 0; i < visible && first + i < m_lines.size(); ++i) {
        ++visibleLines;
        visibleBytes += qMax(0, m_lines.at(first + i).byteLength);
    }
    if (visibleLines <= 0) {
        return 1;
    }
    return qMax(1, (visibleBytes + visibleLines - 1) / visibleLines);
}

int TextViewWidget::recommendedViewportByteCount() const {
    return viewportByteCapacity();
}

void TextViewWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    const quint64 anchorBeforeResize = currentCenterAnchorOffset();
    layoutChildren();
    if (!m_backingBytes.isEmpty()) {
        ensureOffsetInViewport(anchorBeforeResize, true);
    } else {
        rebuildLines();
    }
    emitCenterAnchorOffset();
}

bool TextViewWidget::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Paint) {
        if (watched == m_contentWidget) {
            paintContent();
            return true;
        }
        if (watched == m_gutterWidget) {
            paintGutter();
            return true;
        }
    }

    if (watched == m_contentWidget || watched == m_gutterWidget) {
        if (event->type() == QEvent::Wheel) {
            auto* wheelEvent = static_cast<QWheelEvent*>(event);
            if (handleWheelEvent(wheelEvent)) {
                return true;
            }
        }
    }

    if (watched == m_contentWidget) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                setFocus(Qt::MouseFocusReason);
                const std::optional<int> visibleIdx = visibleIndexForPoint(mouseEvent->pos());
                if (visibleIdx.has_value()) {
                    m_selectionStartVisibleIndex = visibleIdx.value();
                    m_selectionEndVisibleIndex = visibleIdx.value();
                    m_hasSelection = true;
                    m_selecting = true;
                    m_selectedOffset = m_visibleOffsets.at(visibleIdx.value());
                    m_hasSelectedOffset = true;
                } else {
                    m_hasSelection = false;
                    m_selecting = false;
                    m_selectionStartVisibleIndex = -1;
                    m_selectionEndVisibleIndex = -1;
                }
                m_contentWidget->update();
                m_gutterWidget->update();
                emitSelectionRangeChanged();
                mouseEvent->accept();
                return true;
            }
            if (mouseEvent->button() == Qt::RightButton && hasSelectionRange()) {
                showSelectionContextMenu(mouseEvent->pos());
                mouseEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            updateHoverFromPoint(mouseEvent->pos());
            if (m_selecting && (mouseEvent->buttons() & Qt::LeftButton)) {
                const std::optional<int> visibleIdx = visibleIndexForPoint(mouseEvent->pos());
                if (visibleIdx.has_value()) {
                    m_selectionEndVisibleIndex = visibleIdx.value();
                    m_selectedOffset = m_visibleOffsets.at(visibleIdx.value());
                    m_hasSelectedOffset = true;
                }
                m_contentWidget->update();
                m_gutterWidget->update();
                emitSelectionRangeChanged();
            }
            return false;
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::LeftButton && m_selecting) {
                m_selecting = false;
                emitSelectionRangeChanged();
                mouseEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::ContextMenu) {
            if (hasSelectionRange()) {
                auto* contextEvent = static_cast<QContextMenuEvent*>(event);
                showSelectionContextMenu(contextEvent->pos());
                contextEvent->accept();
                return true;
            }
        } else if (event->type() == QEvent::Leave) {
            if (m_lastHoveredAbsoluteOffset >= 0) {
                m_lastHoveredAbsoluteOffset = -1;
                emit hoverLeft();
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void TextViewWidget::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        if (!hasSelectionRange()) {
            event->accept();
            return;
        }
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (clipboard != nullptr) {
            clipboard->setText(selectedText(true));
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TextViewWidget::wheelEvent(QWheelEvent* event) {
    if (handleWheelEvent(event)) {
        return;
    }
    QWidget::wheelEvent(event);
}

bool TextViewWidget::handleWheelEvent(QWheelEvent* event) {
    const QPoint delta = event->angleDelta();
    if (delta.y() == 0) {
        return false;
    }

    const int steps = delta.y() / 120;
    if (steps == 0) {
        return false;
    }
    emit backingScrollRequested(steps, scrollBytesPerWheelStepHint(), visibleByteCount());

    event->accept();
    return true;
}

QVector<TextViewWidget::Token> TextViewWidget::decodeTokens(const QByteArray& rawLine,
                                                             quint64 absoluteOffset) const {
    QVector<Token> tokens;
    if (rawLine.isEmpty()) {
        return tokens;
    }

    const QFontMetrics fm(font());
    const int byteCellWidth =
        qMax(fm.horizontalAdvance(QStringLiteral("00")), fm.horizontalAdvance(QStringLiteral("FF"))) +
        10;

    for (int i = 0; i < rawLine.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(rawLine.at(i));
        const quint64 tokenAbsoluteOffset = absoluteOffset + static_cast<quint64>(i);
        const qint64 relSigned =
            static_cast<qint64>(tokenAbsoluteOffset) - static_cast<qint64>(m_baseOffset);
        const int rel = static_cast<int>(relSigned);
        const TextByteClass cls = (rel >= 0 && rel < m_byteClasses.size())
                                      ? m_byteClasses.at(rel)
                                      : TextByteClass::Invalid;

        Token token;
        token.absoluteOffset = tokenAbsoluteOffset;
        token.byteLen = 1;
        token.cls = cls;
        token.byteValue = byte;

        if (m_displayMode == TextDisplayMode::ByteMode) {
            token.kind = TokenKind::ByteBox;
            token.text = QStringLiteral("%1").arg(static_cast<int>(byte), 2, 16, QChar('0')).toUpper();
            token.pixelWidth = byteCellWidth;
            tokens.push_back(token);
            continue;
        }

        if (m_mode != TextInterpretationMode::Ascii) {
            quint32 cp = 0;
            int cpLen = 0;
            bool decoded = false;
            if (m_mode == TextInterpretationMode::Utf8) {
                decoded = decodeUtf8At(rawLine, i, &cp, &cpLen);
            } else if (m_mode == TextInterpretationMode::Utf16) {
                decoded = decodeUtf16At(rawLine, i, m_utf16LittleEndian, &cp, &cpLen);
            }
            if (decoded && cpLen > 1 && cp != 0x0AU && cp != 0x0DU && i + cpLen <= rawLine.size() &&
                cls != TextByteClass::Invalid) {
                const QString glyph = glyphFromCodepoint(cp);
                if (!glyph.isEmpty()) {
                    token.kind = TokenKind::Text;
                    token.text = glyph;
                    token.pixelWidth = fm.horizontalAdvance(token.text);
                    tokens.push_back(token);

                    for (int j = 1; j < cpLen; ++j) {
                        Token cont;
                        cont.absoluteOffset = tokenAbsoluteOffset + static_cast<quint64>(j);
                        cont.byteLen = 1;
                        const int relNext = rel + j;
                        cont.cls = (relNext >= 0 && relNext < m_byteClasses.size())
                                       ? m_byteClasses.at(relNext)
                                       : token.cls;
                        cont.byteValue =
                            static_cast<unsigned char>(rawLine.at(i + j));
                        cont.kind = TokenKind::Text;
                        cont.text.clear();
                        cont.pixelWidth = 0;
                        tokens.push_back(cont);
                    }

                    i += (cpLen - 1);
                    continue;
                }
            }
        }

        if (isPrintableAscii(byte)) {
            token.kind = TokenKind::Text;
            token.text = QString(QChar::fromLatin1(static_cast<char>(byte)));
            token.pixelWidth = fm.horizontalAdvance(token.text);
            tokens.push_back(token);
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            token.kind = TokenKind::ByteBox;
            token.controlByteBox = true;
            token.text = QStringLiteral("%1").arg(static_cast<int>(byte), 2, 16, QChar('0')).toUpper();
            token.pixelWidth = byteCellWidth;
            tokens.push_back(token);
            continue;
        }

        token.kind = TokenKind::ByteBox;
        const bool specialNull = (byte == 0x00U);
        token.specialNullBox = specialNull;
        token.text = specialNull
                         ? QStringLiteral("0")
                         : QStringLiteral("%1").arg(static_cast<int>(byte), 2, 16, QChar('0')).toUpper();
        token.pixelWidth = byteCellWidth;
        tokens.push_back(token);
    }

    if (m_displayMode != TextDisplayMode::StringMode || tokens.size() < 3) {
        return tokens;
    }

    QVector<Token> collapsed;
    collapsed.reserve(tokens.size());
    int i = 0;
    while (i < tokens.size()) {
        const Token& first = tokens.at(i);
        if (first.kind != TokenKind::ByteBox) {
            collapsed.push_back(first);
            ++i;
            continue;
        }

        int runEnd = i + 1;
        while (runEnd < tokens.size()) {
            const Token& prev = tokens.at(runEnd - 1);
            const Token& candidate = tokens.at(runEnd);
            const quint64 expectedOffset =
                prev.absoluteOffset + static_cast<quint64>(qMax(1, prev.byteLen));
            if (candidate.kind != TokenKind::ByteBox || candidate.byteValue != first.byteValue ||
                candidate.absoluteOffset != expectedOffset) {
                break;
            }
            ++runEnd;
        }

        const int runLen = runEnd - i;
        if (runLen >= 3) {
            Token merged = first;
            merged.byteLen = runLen;
            merged.collapsedUnprintableRun = true;
            collapsed.push_back(std::move(merged));
        } else {
            for (int j = i; j < runEnd; ++j) {
                collapsed.push_back(tokens.at(j));
            }
        }
        i = runEnd;
    }

    return collapsed;
}

void TextViewWidget::rebuildLines() {
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::rebuildLines: start bytes=%1 mode=%2 displayMode=%3")
                           .arg(m_bytes.size())
                           .arg(static_cast<int>(m_mode))
                           .arg(static_cast<int>(m_displayMode)));
    }
    const quint64 rebuildStartUs = debug::selectionTraceElapsedUs();
    m_lines.clear();
    m_visibleOffsets.clear();
    m_stringVisibilityMask.clear();
    m_byteClasses.clear();
    if (m_bytes.isEmpty()) {
        m_hasSelection = false;
        m_selectionStartVisibleIndex = -1;
        m_selectionEndVisibleIndex = -1;
        updateScrollRange();
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral("TextViewWidget::rebuildLines: empty bytes, elapsed=%1us")
                               .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
        }
        return;
    }

    const quint64 analyzeStartUs = debug::selectionTraceElapsedUs();
    const TextAnalysisResult analysis = TextSequenceAnalyzer::analyze(m_bytes, m_mode);
    m_byteClasses = analysis.classes;
    m_utf16LittleEndian = analysis.utf16LittleEndian;
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::rebuildLines: analyze elapsed=%1us")
                           .arg(debug::selectionTraceElapsedUs() - analyzeStartUs));
    }
    if (m_byteClasses.size() != m_bytes.size()) {
        m_byteClasses.fill(TextByteClass::Invalid, m_bytes.size());
    }

    // Visibility is byte-complete in both modes: no byte hiding/skipping in viewport.
    m_stringVisibilityMask.fill(true, m_bytes.size());

    int nextVisibleIndex = 0;
    auto finalizeLineFromTokens = [&](QVector<Token>&& rawTokens) {
        if (rawTokens.isEmpty()) {
            return;
        }

        DisplayLine line;
        line.absoluteOffset = rawTokens.first().absoluteOffset;
        line.firstVisibleIndex = nextVisibleIndex;
        line.tokens.reserve(rawTokens.size());
        for (Token& token : rawTokens) {
            token.visibleIndex = nextVisibleIndex++;
            line.byteLength += qMax(1, token.byteLen);
            line.pixelWidth += tokenVisualWidth(token);
            m_visibleOffsets.push_back(token.absoluteOffset);
            line.tokens.push_back(std::move(token));
        }
        m_lines.push_back(std::move(line));
    };

    auto finalizeLine = [&](quint64 absoluteOffset, const QByteArray& lineBytes) {
        QVector<Token> lineTokens = decodeTokens(lineBytes, absoluteOffset);
        finalizeLineFromTokens(std::move(lineTokens));
    };

    auto finalizeRebuildState = [&]() {
        if (m_visibleOffsets.isEmpty()) {
            m_hasSelection = false;
            m_selectionStartVisibleIndex = -1;
            m_selectionEndVisibleIndex = -1;
            updateScrollRange();
            return;
        }

        if (m_hasSelection) {
            m_selectionStartVisibleIndex =
                qBound(0, m_selectionStartVisibleIndex, m_visibleOffsets.size() - 1);
            m_selectionEndVisibleIndex =
                qBound(0, m_selectionEndVisibleIndex, m_visibleOffsets.size() - 1);
        } else {
            m_selectionStartVisibleIndex = -1;
            m_selectionEndVisibleIndex = -1;
        }
        updateScrollRange();
    };

    auto finalizeWithWrap = [&](quint64 absoluteOffset, const QByteArray& rawLine) {
        const bool wrapEnabled =
            (m_displayMode == TextDisplayMode::ByteMode) ? true : m_wrapMode;
        if (!wrapEnabled) {
            finalizeLine(absoluteOffset, rawLine);
            return;
        }

        if (rawLine.isEmpty()) {
            finalizeLine(absoluteOffset, rawLine);
            return;
        }

        const int bytesPerLine = fixedBytesPerLine();
        if (bytesPerLine > 0) {
            quint64 currentOffset = absoluteOffset;
            for (int i = 0; i < rawLine.size(); i += bytesPerLine) {
                const int len = qMin(bytesPerLine, rawLine.size() - i);
                finalizeLine(currentOffset, rawLine.mid(i, len));
                currentOffset += static_cast<quint64>(len);
            }
            return;
        }

        QVector<Token> tokens = decodeTokens(rawLine, absoluteOffset);
        if (tokens.isEmpty()) {
            return;
        }

        const int wrapWidth = qMax(48, m_contentWidget->width() - 16);
        QVector<Token> currentTokens;
        currentTokens.reserve(tokens.size());
        int currentWidth = 0;
        for (Token token : tokens) {
            const int nextWidth = tokenVisualWidth(token);

            if (!currentTokens.isEmpty() && currentWidth + nextWidth > wrapWidth) {
                finalizeLineFromTokens(std::move(currentTokens));
                currentTokens.clear();
                currentWidth = 0;
            }
            currentTokens.push_back(std::move(token));
            currentWidth += nextWidth;
        }

        if (!currentTokens.isEmpty()) {
            finalizeLineFromTokens(std::move(currentTokens));
        }
    };

    if (m_displayMode == TextDisplayMode::ByteMode) {
        finalizeWithWrap(m_baseOffset, m_bytes);
        finalizeRebuildState();
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral(
                               "TextViewWidget::rebuildLines: done(byte mode) lines=%1 visibleOffsets=%2 elapsed=%3us")
                               .arg(m_lines.size())
                               .arg(m_visibleOffsets.size())
                               .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
        }
        return;
    }

    if (m_newlineMode == TextNewlineMode::None) {
        finalizeWithWrap(m_baseOffset, m_bytes);
        finalizeRebuildState();
        if (debug::selectionTraceEnabled()) {
            BRECO_SELTRACE(QStringLiteral(
                               "TextViewWidget::rebuildLines: done(no newline split) lines=%1 visibleOffsets=%2 elapsed=%3us")
                               .arg(m_lines.size())
                               .arg(m_visibleOffsets.size())
                               .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
        }
        return;
    }

    QByteArray currentRaw;
    quint64 currentOffset = m_baseOffset;
    for (int i = 0; i < m_bytes.size(); ++i) {
        currentRaw.append(m_bytes.at(i));
        const bool byteVisible = (i >= 0 && i < m_stringVisibilityMask.size())
                                     ? m_stringVisibilityMask.at(i)
                                     : true;
        if (shouldBreakAfterByte(i, m_bytes, byteVisible)) {
            finalizeWithWrap(currentOffset, currentRaw);
            currentRaw.clear();
            currentOffset = m_baseOffset + static_cast<quint64>(i + 1);
        }
    }
    if (!currentRaw.isEmpty()) {
        finalizeWithWrap(currentOffset, currentRaw);
    }

    finalizeRebuildState();
    if (debug::selectionTraceEnabled()) {
        BRECO_SELTRACE(QStringLiteral("TextViewWidget::rebuildLines: done lines=%1 visibleOffsets=%2 elapsed=%3us")
                           .arg(m_lines.size())
                           .arg(m_visibleOffsets.size())
                           .arg(debug::selectionTraceElapsedUs() - rebuildStartUs));
    }
}

void TextViewWidget::updateScrollRange() {
    const int visible = visibleLineCount();
    m_vScrollBar->setPageStep(qMax(1, visible));
    m_vScrollBar->setSingleStep(1);
    m_vScrollBar->setRange(0, qMax(0, m_lines.size() - visible));

    const bool disableH = (m_displayMode == TextDisplayMode::ByteMode) || m_wrapMode;
    if (disableH) {
        m_hScrollBar->setPageStep(qMax(1, m_contentWidget->width()));
        m_hScrollBar->setSingleStep(0);
        m_hScrollBar->setRange(0, 0);
        return;
    }

    int maxWidth = 0;
    for (const DisplayLine& line : m_lines) {
        maxWidth = qMax(maxWidth, line.pixelWidth + 16);
    }
    m_hScrollBar->setPageStep(qMax(1, m_contentWidget->width()));
    m_hScrollBar->setSingleStep(12);
    m_hScrollBar->setRange(0, qMax(0, maxWidth - m_contentWidget->width()));
}

int TextViewWidget::lineHeight() const {
    const QFontMetrics fm(font());
    return fm.height() + 4;
}

int TextViewWidget::visibleLineCount() const {
    return qMax(1, m_contentWidget->height() / qMax(1, lineHeight()));
}

int TextViewWidget::firstVisibleLine() const { return m_vScrollBar->value(); }

int TextViewWidget::lineIndexForOffset(quint64 absoluteOffset) const {
    if (m_lines.isEmpty()) {
        return 0;
    }
    for (int i = 0; i < m_lines.size(); ++i) {
        const DisplayLine& line = m_lines.at(i);
        if (line.tokens.isEmpty()) {
            continue;
        }
        const quint64 first = line.tokens.first().absoluteOffset;
        const Token& lastToken = line.tokens.last();
        const quint64 last = lastToken.absoluteOffset +
                             static_cast<quint64>(qMax(1, lastToken.byteLen)) - 1ULL;
        if (absoluteOffset <= first) {
            return i;
        }
        if (absoluteOffset >= first && absoluteOffset <= last) {
            return i;
        }
    }
    return m_lines.size() - 1;
}

int TextViewWidget::xOffsetForAbsoluteOffset(const DisplayLine& line, quint64 absoluteOffset) const {
    int x = 8;
    for (const Token& token : line.tokens) {
        const quint64 tokenEndExclusive =
            token.absoluteOffset + static_cast<quint64>(qMax(1, token.byteLen));
        if (absoluteOffset < tokenEndExclusive) {
            return x;
        }
        x += tokenVisualWidth(token);
    }
    return x;
}

quint64 TextViewWidget::absoluteOffsetForPoint(const QPoint& point) const {
    const std::optional<int> visibleIdx = visibleIndexForPoint(point);
    if (!visibleIdx.has_value()) {
        return m_baseOffset;
    }
    const int idx = visibleIdx.value();
    if (idx < 0 || idx >= m_visibleOffsets.size()) {
        return m_baseOffset;
    }
    return m_visibleOffsets.at(idx);
}

std::optional<int> TextViewWidget::visibleIndexForPoint(const QPoint& point) const {
    if (m_lines.isEmpty() || m_visibleOffsets.isEmpty()) {
        return std::nullopt;
    }

    const int lineH = lineHeight();
    const int lineIndex = qBound(0, firstVisibleLine() + (point.y() / qMax(1, lineH)),
                                 m_lines.size() - 1);
    const DisplayLine& line = m_lines.at(lineIndex);
    if (line.tokens.isEmpty()) {
        return std::nullopt;
    }
    const int xTarget = point.x() + m_hScrollBar->value();
    const QFontMetrics fm(font());

    int x = 8;
    for (const Token& token : line.tokens) {
        const int tokenWidth = tokenVisualWidth(token);
        const int tokenStartX = x;
        const int tokenEndX = x + tokenWidth;
        if (xTarget <= tokenStartX) {
            return token.visibleIndex;
        }
        if (xTarget < tokenEndX) {
            if (token.kind == TokenKind::Text && !token.text.isEmpty()) {
                if (xTarget >= tokenStartX + (fm.horizontalAdvance(token.text) / 2)) {
                    if (token.visibleIndex + 1 < m_visibleOffsets.size()) {
                        return token.visibleIndex + 1;
                    }
                }
                return token.visibleIndex;
            }
            return token.visibleIndex;
        }
        x = tokenEndX;
    }

    return line.tokens.last().visibleIndex;
}

void TextViewWidget::updateHoverFromPoint(const QPoint& point) {
    if (m_lines.isEmpty() || m_visibleOffsets.isEmpty()) {
        return;
    }
    const quint64 absoluteOffset = absoluteOffsetForPoint(point);
    if (m_lastHoveredAbsoluteOffset != static_cast<qint64>(absoluteOffset)) {
        m_lastHoveredAbsoluteOffset = static_cast<qint64>(absoluteOffset);
        emit hoverAbsoluteOffsetChanged(absoluteOffset);
    }
}

bool TextViewWidget::hasSelectionRange() const {
    return m_hasSelection && m_selectionStartVisibleIndex >= 0 &&
           m_selectionEndVisibleIndex >= 0 &&
           m_selectionStartVisibleIndex != m_selectionEndVisibleIndex;
}

QPair<quint64, quint64> TextViewWidget::normalizedSelection() const {
    const QVector<quint64> offsets = selectedVisibleOffsets();
    if (offsets.isEmpty()) {
        return qMakePair<quint64, quint64>(0, 0);
    }
    return qMakePair(offsets.first(), offsets.last() + 1ULL);
}

QPair<int, int> TextViewWidget::normalizedSelectionVisibleIndices() const {
    if (!hasSelectionRange()) {
        return qMakePair(0, 0);
    }
    if (m_selectionStartVisibleIndex <= m_selectionEndVisibleIndex) {
        return qMakePair(m_selectionStartVisibleIndex, m_selectionEndVisibleIndex);
    }
    return qMakePair(m_selectionEndVisibleIndex, m_selectionStartVisibleIndex);
}

QVector<const TextViewWidget::Token*> TextViewWidget::selectedTokens() const {
    QVector<const Token*> selected;
    if (!hasSelectionRange() || m_lines.isEmpty()) {
        return selected;
    }

    const auto range = normalizedSelectionVisibleIndices();
    const int selectionStart = qMax(0, range.first);
    const int selectionEnd = qMax(selectionStart, range.second);
    selected.reserve(qMax(0, selectionEnd - selectionStart));

    for (const DisplayLine& line : m_lines) {
        for (const Token& token : line.tokens) {
            if (token.visibleIndex < selectionStart) {
                continue;
            }
            if (token.visibleIndex >= selectionEnd) {
                return selected;
            }
            selected.push_back(&token);
        }
    }
    return selected;
}

QVector<quint64> TextViewWidget::selectedVisibleOffsets() const {
    QVector<quint64> offsets;
    const QVector<const Token*> selected = selectedTokens();
    if (selected.isEmpty()) {
        return offsets;
    }

    offsets.reserve(selected.size());
    for (const Token* token : selected) {
        if (token == nullptr) {
            continue;
        }
        offsets.push_back(token->absoluteOffset);
    }
    return offsets;
}

unsigned char TextViewWidget::byteAtAbsoluteOffset(quint64 absoluteOffset) const {
    const qint64 relSigned =
        static_cast<qint64>(absoluteOffset) - static_cast<qint64>(m_baseOffset);
    if (relSigned < 0 || relSigned >= m_bytes.size()) {
        return 0x00U;
    }
    return static_cast<unsigned char>(m_bytes.at(static_cast<int>(relSigned)));
}

QByteArray TextViewWidget::selectedBytes() const {
    const QVector<const Token*> selected = selectedTokens();
    if (selected.isEmpty()) {
        return {};
    }

    QByteArray out;
    out.reserve(selected.size());
    for (const Token* token : selected) {
        if (token == nullptr) {
            continue;
        }
        out.push_back(static_cast<char>(byteAtAbsoluteOffset(token->absoluteOffset)));
    }
    return out;
}

QString TextViewWidget::selectedText(bool replaceNullMarkers) const {
    const QVector<const Token*> selected = selectedTokens();
    if (selected.isEmpty()) {
        return {};
    }

    QString out;
    out.reserve(selected.size() * 3);
    for (const Token* token : selected) {
        if (token == nullptr) {
            continue;
        }

        if (token->kind == TokenKind::Text) {
            if (!token->text.isEmpty()) {
                out.append(token->text);
            }
            continue;
        }

        if (token->kind == TokenKind::ByteBox) {
            if (replaceNullMarkers && token->specialNullBox) {
                out.append(QStringLiteral(" {null} "));
            } else {
                out.append(token->text);
            }
        }

        // Copy uses visible token text, but line breaks should still follow the active newline mode.
        if (m_displayMode != TextDisplayMode::StringMode || m_newlineMode == TextNewlineMode::None) {
            continue;
        }

        const qint64 relSigned =
            static_cast<qint64>(token->absoluteOffset) - static_cast<qint64>(m_baseOffset);
        if (relSigned < 0 || relSigned >= m_bytes.size()) {
            continue;
        }

        const int runStart = static_cast<int>(relSigned);
        const int runLen = qMax(1, token->byteLen);
        const int runEndExclusive = qMin(m_bytes.size(), runStart + runLen);
        for (int idx = runStart; idx < runEndExclusive; ++idx) {
            const bool byteVisible =
                (idx >= 0 && idx < m_stringVisibilityMask.size()) ? m_stringVisibilityMask.at(idx) : true;
            if (shouldBreakAfterByte(idx, m_bytes, byteVisible)) {
                out.append('\n');
            }
        }
    }
    return out;
}

QString TextViewWidget::selectedOffsetHexText() const {
    const QByteArray slice = selectedBytes();
    const QVector<quint64> offsets = selectedVisibleOffsets();
    if (slice.isEmpty() || offsets.isEmpty() || slice.size() != offsets.size()) {
        return {};
    }

    QString out;
    constexpr int kBytesPerLine = 16;
    for (int i = 0; i < slice.size(); i += kBytesPerLine) {
        out.append(QStringLiteral("%1: ")
                       .arg(offsets.at(i), 12, 16, QChar('0'))
                       .toUpper());
        const int lineLen = qMin(kBytesPerLine, slice.size() - i);
        for (int j = 0; j < lineLen; ++j) {
            const unsigned char b = static_cast<unsigned char>(slice.at(i + j));
            out.append(QStringLiteral("%1").arg(static_cast<int>(b), 2, 16, QChar('0')).toUpper());
            if (j + 1 < lineLen) {
                out.append(' ');
            }
        }
        if (i + lineLen < slice.size()) {
            out.append('\n');
        }
    }
    return out;
}

QString TextViewWidget::selectedHexText() const {
    const QByteArray slice = selectedBytes();
    if (slice.isEmpty()) {
        return {};
    }

    QString out;
    out.reserve(slice.size() * 3);
    for (int i = 0; i < slice.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(slice.at(i));
        out.append(QStringLiteral("%1").arg(static_cast<int>(b), 2, 16, QChar('0')).toUpper());
        if (i + 1 < slice.size()) {
            out.append(' ');
        }
    }
    return out;
}

QString TextViewWidget::selectedCHeaderText() const {
    const QByteArray slice = selectedBytes();
    if (slice.isEmpty()) {
        return {};
    }

    QString out;
    out.append(QStringLiteral("static const unsigned char selected_bytes[] = {\n    "));
    for (int i = 0; i < slice.size(); ++i) {
        const unsigned char b = static_cast<unsigned char>(slice.at(i));
        out.append(QStringLiteral("0x%1").arg(static_cast<int>(b), 2, 16, QChar('0')).toUpper());
        if (i + 1 < slice.size()) {
            out.append(QStringLiteral(", "));
        }
        if ((i + 1) % 12 == 0 && i + 1 < slice.size()) {
            out.append(QStringLiteral("\n    "));
        }
    }
    out.append(QStringLiteral("\n};\n"));
    out.append(QStringLiteral("static const unsigned int selected_bytes_len = %1;\n")
                   .arg(slice.size()));
    return out;
}

void TextViewWidget::copySelectionToClipboard(CopyFormat format) const {
    if (!hasSelectionRange()) {
        return;
    }

    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }

    switch (format) {
        case CopyFormat::TextOnly:
            clipboard->setText(selectedText(false));
            break;
        case CopyFormat::OffsetHex:
            clipboard->setText(selectedOffsetHexText());
            break;
        case CopyFormat::Hex:
            clipboard->setText(selectedHexText());
            break;
        case CopyFormat::CHeader:
            clipboard->setText(selectedCHeaderText());
            break;
        case CopyFormat::Binary: {
            const QByteArray slice = selectedBytes();
            if (slice.isEmpty()) {
                return;
            }
            auto* mime = new QMimeData();
            mime->setData(QStringLiteral("application/octet-stream"), slice);
            mime->setText(selectedHexText());
            clipboard->setMimeData(mime);
            break;
        }
    }
}

int TextViewWidget::fixedBytesPerLine() const {
    if (m_displayMode != TextDisplayMode::ByteMode) {
        return 0;
    }
    switch (m_byteLineMode) {
        case ByteLineMode::B8:
            return 8;
        case ByteLineMode::B16:
            return 16;
        case ByteLineMode::B32:
            return 32;
        case ByteLineMode::B64:
            return 64;
        case ByteLineMode::Auto:
        default:
            return 0;
    }
}

bool TextViewWidget::shouldBreakAfterByte(int index, const QByteArray& data,
                                          bool byteIsVisible) const {
    Q_UNUSED(byteIsVisible);
    if (index < 0 || index >= data.size()) {
        return false;
    }

    const unsigned char byte = static_cast<unsigned char>(data.at(index));
    bool nullBreakAllowed = false;
    if (byte == 0x00U) {
        std::optional<unsigned char> previousByte;
        if (index > 0) {
            previousByte = static_cast<unsigned char>(data.at(index - 1));
        } else {
            previousByte = m_previousByteBeforeBase;
        }
        nullBreakAllowed = shouldRenderStringModeNull(previousByte);
    }

    switch (m_newlineMode) {
        case TextNewlineMode::None:
            return false;
        case TextNewlineMode::Nl:
            return byte == '\n';
        case TextNewlineMode::Crlf:
            return (byte == '\n' && index > 0 && data.at(index - 1) == '\r');
        case TextNewlineMode::Null:
            return byte == 0x00U && nullBreakAllowed;
        case TextNewlineMode::NlCrNull:
            if (byte == '\n') {
                return true;
            }
            if (byte == '\r') {
                if (index + 1 < data.size() && data.at(index + 1) == '\n') {
                    return false;
                }
                return true;
            }
            if (byte == 0x00U) {
                return nullBreakAllowed;
            }
            return false;
        default:
            return false;
    }
}

void TextViewWidget::emitSelectionRangeChanged() {
    if (!hasSelectionRange()) {
        emit selectionRangeChanged(false, 0, 0);
        return;
    }
    const QVector<quint64> offsets = selectedVisibleOffsets();
    if (offsets.isEmpty()) {
        emit selectionRangeChanged(false, 0, 0);
        return;
    }
    for (int i = 1; i < offsets.size(); ++i) {
        if (offsets.at(i) != offsets.at(i - 1) + 1ULL) {
            emit selectionRangeChanged(false, 0, 0);
            return;
        }
    }
    emit selectionRangeChanged(true, offsets.first(), offsets.last() + 1ULL);
}

void TextViewWidget::showSelectionContextMenu(const QPoint& localPos) {
    if (!hasSelectionRange()) {
        return;
    }

    QMenu menu(this);
    QMenu* copyMenu = menu.addMenu(QStringLiteral("Copy"));
    QAction* copyText = copyMenu->addAction(QStringLiteral("Text only"));
    QAction* copyOffsetHex = copyMenu->addAction(QStringLiteral("Offset + Hex"));
    QAction* copyHex = copyMenu->addAction(QStringLiteral("Hex"));
    QAction* copyCHeader = copyMenu->addAction(QStringLiteral("C Header"));
    QAction* copyBinary = copyMenu->addAction(QStringLiteral("Binary"));

    QAction* selected = menu.exec(m_contentWidget->mapToGlobal(localPos));
    if (selected == copyText) {
        copySelectionToClipboard(CopyFormat::TextOnly);
    } else if (selected == copyOffsetHex) {
        copySelectionToClipboard(CopyFormat::OffsetHex);
    } else if (selected == copyHex) {
        copySelectionToClipboard(CopyFormat::Hex);
    } else if (selected == copyCHeader) {
        copySelectionToClipboard(CopyFormat::CHeader);
    } else if (selected == copyBinary) {
        copySelectionToClipboard(CopyFormat::Binary);
    }
}

int TextViewWidget::tokenVisualWidth(const Token& token) const {
    return token.pixelWidth + (token.kind == TokenKind::ByteBox ? 2 : 0);
}

QColor TextViewWidget::colorForClass(TextByteClass cls) const {
    switch (cls) {
        case TextByteClass::Printable:
            return QColor(0x00, 0x00, 0x00);
        case TextByteClass::Newline:
            return QColor(0xF5, 0xF5, 0xDC);
        case TextByteClass::CarriageReturn:
            return QColor(0xFA, 0xEB, 0xD7);
        case TextByteClass::NonBreakingSpace:
            return QColor(0x00, 0xFF, 0xFF);
        case TextByteClass::Space:
            return QColor(0x7F, 0xFF, 0xD4);
        case TextByteClass::Tab:
            return QColor(0x5F, 0x9E, 0xA0);
        case TextByteClass::SoftHyphen:
            return QColor(0xFF, 0x8C, 0x00);
        case TextByteClass::OtherWhitespace:
            return QColor(0x00, 0xBF, 0xFF);
        case TextByteClass::Invalid:
        default:
            return QColor(130, 130, 130);
    }
}

void TextViewWidget::layoutChildren() {
    const int scrollW = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int scrollH = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    const int gutterW = m_gutterVisible ? 110 : 0;
    const int contentW = qMax(0, width() - gutterW - scrollW);
    const int contentH = qMax(0, height() - scrollH);

    m_gutterWidget->setGeometry(0, 0, gutterW, contentH);
    m_contentWidget->setGeometry(gutterW, 0, contentW, contentH);
    m_vScrollBar->setGeometry(gutterW + contentW, 0, scrollW, contentH);
    m_hScrollBar->setGeometry(gutterW, contentH, contentW, scrollH);
    m_gutterWidget->setVisible(m_gutterVisible);
}

void TextViewWidget::paintContent() {
    QPainter painter(m_contentWidget);
    painter.fillRect(m_contentWidget->rect(), palette().base());

    if (m_lines.isEmpty()) {
        painter.setPen(palette().text().color());
        painter.drawText(m_contentWidget->rect(), Qt::AlignCenter, QStringLiteral("Text view"));
        return;
    }

    const QFontMetrics fm(font());
    const int lineH = lineHeight();
    const int firstLine = firstVisibleLine();
    const int visible = visibleLineCount();
    const int selectedLine = lineIndexForOffset(m_selectedOffset);
    const int xShift = m_hScrollBar->value();
    const bool hasSelection = hasSelectionRange();
    const auto selection = normalizedSelectionVisibleIndices();

    for (int i = 0; i < visible && firstLine + i < m_lines.size(); ++i) {
        const int lineIdx = firstLine + i;
        const int baselineY = fm.ascent() + 8 + i * lineH;
        const QRect rowRect(0, i * lineH, m_contentWidget->width(), lineH);
        if (lineIdx == selectedLine) {
            painter.fillRect(rowRect, palette().alternateBase());
        }

        int x = 8 - xShift;
        for (const Token& token : m_lines.at(lineIdx).tokens) {
            const quint64 tokenStartOffset = token.absoluteOffset;
            const quint64 tokenEndOffset =
                token.absoluteOffset + static_cast<quint64>(qMax(1, token.byteLen));

            if (hasSelection && token.visibleIndex >= selection.first &&
                token.visibleIndex < selection.second) {
                painter.fillRect(QRect(x, rowRect.top() + 1, token.pixelWidth, lineH - 2),
                                 palette().highlight().color().lighter(120));
            }

            const quint64 matchStart = m_matchStartOffset;
            const quint64 matchEnd = m_matchStartOffset + static_cast<quint64>(m_matchLength);
            const bool overlapsMatch =
                m_matchLength > 0 && tokenEndOffset > matchStart && tokenStartOffset < matchEnd;
            if (overlapsMatch) {
                painter.fillRect(QRect(x, rowRect.top() + 1, token.pixelWidth, lineH - 2),
                                 QColor(180, 255, 180));
            }

            if (token.kind == TokenKind::Text) {
                painter.setPen(colorForClass(token.cls));
                painter.drawText(x, baselineY, token.text);
                x += token.pixelWidth;
            } else {
                const QRect tokenRect(x, rowRect.top() + 2, token.pixelWidth, lineH - 4);
                QColor fg = colorForClass(token.cls);
                QColor bg = QColor(0, 0, 0, 0);
                if (token.specialNullBox) {
                    fg = QColor(0, 180, 0);
                    bg = QColor(0xFE, 0xFE, 0xFE);
                }
                if (token.controlByteBox) {
                    fg = QColor(0x00, 0x00, 0x00);
                    bg = QColor(0xFE, 0xFE, 0xFE);
                }
                if (token.collapsedUnprintableRun) {
                    fg = QColor(0x00, 0x00, 0xFF);
                    bg = QColor(0, 0, 0, 0);
                }
                if (bg.alpha() > 0) {
                    painter.fillRect(tokenRect, bg);
                }
                painter.setPen(fg);
                painter.drawRect(tokenRect);
                painter.drawText(tokenRect, Qt::AlignCenter, token.text);
                x += token.pixelWidth + 2;
            }
        }
    }
}

void TextViewWidget::paintGutter() {
    QPainter painter(m_gutterWidget);
    painter.fillRect(m_gutterWidget->rect(), QColor(0xF8, 0xF8, 0xFF));
    if (m_lines.isEmpty()) {
        return;
    }

    const QFontMetrics fm(font());
    const int lineH = lineHeight();
    const int firstLine = firstVisibleLine();
    const int visible = visibleLineCount();
    const int selectedLine = lineIndexForOffset(m_selectedOffset);

    for (int i = 0; i < visible && firstLine + i < m_lines.size(); ++i) {
        const int lineIdx = firstLine + i;
        const int y = fm.ascent() + 8 + i * lineH;
        const QRect rowRect(0, i * lineH, m_gutterWidget->width(), lineH);
        if (lineIdx == selectedLine) {
            painter.fillRect(rowRect, palette().highlight());
        }
        painter.setPen(lineIdx == selectedLine ? palette().highlightedText().color()
                                               : palette().text().color());
        painter.drawText(6, y,
                         QStringLiteral("%1")
                             .arg(m_lines.at(lineIdx).absoluteOffset, 10, 16, QChar('0'))
                             .toUpper());
    }
}

void TextViewWidget::emitCenterAnchorOffset() {
    const quint64 anchor = currentCenterAnchorOffset();
    if (anchor != m_lastEmittedCenterAnchor) {
        m_lastEmittedCenterAnchor = anchor;
        emit centerAnchorOffsetChanged(anchor);
    }
}

quint64 TextViewWidget::currentCenterAnchorOffset() const {
    if (m_hasSelectedOffset) {
        const quint64 start = m_baseOffset;
        const quint64 size = static_cast<quint64>(qMax(0, m_bytes.size()));
        if (size > 0 && m_selectedOffset >= start && m_selectedOffset < start + size) {
            if (m_displayMode == TextDisplayMode::ByteMode) {
                return m_selectedOffset;
            }
            const qint64 relSigned =
                static_cast<qint64>(m_selectedOffset) - static_cast<qint64>(m_baseOffset);
            if (relSigned >= 0 && relSigned < m_stringVisibilityMask.size() &&
                m_stringVisibilityMask.at(static_cast<int>(relSigned))) {
                return m_selectedOffset;
            }
        }
    }
    if (m_lines.isEmpty() || m_visibleOffsets.isEmpty()) {
        return m_baseOffset;
    }
    const int centerLine =
        qMin(m_lines.size() - 1, firstVisibleLine() + (visibleLineCount() / 2));
    const DisplayLine& line = m_lines.at(centerLine);
    if (!line.tokens.isEmpty()) {
        return line.tokens.first().absoluteOffset;
    }
    return m_visibleOffsets.first();
}

}  // namespace breco
