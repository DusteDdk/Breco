#pragma once

#include <QByteArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPair>
#include <QScrollBar>
#include <QVector>
#include <QWheelEvent>
#include <QWidget>
#include <optional>

#include "model/ResultTypes.h"
#include "text/TextSequenceAnalyzer.h"

namespace breco {

enum class TextDisplayMode {
    StringMode = 0,
    ByteMode,
};

enum class TextNewlineMode {
    None = 0,
    Nl,
    Crlf,
    Null,
    NlCrNull,
};

enum class ByteLineMode {
    B8 = 0,
    B16,
    B32,
    B64,
    Auto,
};

class TextViewWidget : public QWidget {
    Q_OBJECT

public:
    explicit TextViewWidget(QWidget* parent = nullptr);

    void setMode(TextInterpretationMode mode);
    void setDisplayMode(TextDisplayMode mode);
    void setData(const QByteArray& bytes, quint64 baseOffset,
                 std::optional<unsigned char> previousByteBeforeBase = std::nullopt);
    void setSelectedOffset(quint64 absoluteOffset, bool centerInView = true);
    void setMatchRange(quint64 startOffset, quint32 length);
    void setGutterVisible(bool visible);
    void setNewlineMode(TextNewlineMode mode);
    void setWrapMode(bool enabled);
    void setByteLineMode(ByteLineMode mode);
    void setMonospaceEnabled(bool enabled);
    int visibleByteCount() const;
    int scrollBytesPerWheelStepHint() const;
    int recommendedViewportByteCount() const;

signals:
    void centerAnchorOffsetChanged(quint64 offset);
    void hoverAbsoluteOffsetChanged(quint64 offset);
    void hoverLeft();
    void selectionRangeChanged(bool hasRange, quint64 start, quint64 end);
    void backingScrollRequested(int wheelSteps, int bytesPerStepHint, int visibleBytesHint);

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class TokenKind { Text, ByteBox };
    enum class CopyFormat { TextOnly, OffsetHex, Hex, CHeader, Binary };

    struct Token {
        TokenKind kind = TokenKind::Text;
        QString text;
        quint64 absoluteOffset = 0;
        int visibleIndex = -1;
        int byteLen = 0;
        int pixelWidth = 0;
        TextByteClass cls = TextByteClass::Invalid;
        unsigned char byteValue = 0;
        bool specialNullBox = false;
        bool controlByteBox = false;
        bool collapsedUnprintableRun = false;
    };

    struct DisplayLine {
        quint64 absoluteOffset = 0;
        int firstVisibleIndex = -1;
        int byteLength = 0;
        int pixelWidth = 0;
        QVector<Token> tokens;
    };

    QVector<Token> decodeTokens(const QByteArray& rawLine, quint64 absoluteOffset) const;
    void rebuildLines();
    int viewportByteCapacity() const;
    bool setViewportWindow(quint64 desiredStartOffset, bool forceRebuild = false);
    bool ensureOffsetInViewport(quint64 absoluteOffset, bool centerInView);
    bool shiftViewportByBytes(qint64 signedBytes);
    void updateScrollRange();
    int lineHeight() const;
    int visibleLineCount() const;
    int firstVisibleLine() const;
    int lineIndexForOffset(quint64 absoluteOffset) const;
    int xOffsetForAbsoluteOffset(const DisplayLine& line, quint64 absoluteOffset) const;
    quint64 absoluteOffsetForPoint(const QPoint& point) const;
    std::optional<int> visibleIndexForPoint(const QPoint& point) const;
    void updateHoverFromPoint(const QPoint& point);
    bool hasSelectionRange() const;
    QPair<quint64, quint64> normalizedSelection() const;
    QPair<int, int> normalizedSelectionVisibleIndices() const;
    QVector<const Token*> selectedTokens() const;
    QVector<quint64> selectedVisibleOffsets() const;
    QByteArray selectedBytes() const;
    QString selectedText(bool replaceNullMarkers) const;
    QString selectedOffsetHexText() const;
    QString selectedHexText() const;
    QString selectedCHeaderText() const;
    void copySelectionToClipboard(CopyFormat format) const;
    int fixedBytesPerLine() const;
    bool shouldBreakAfterByte(int index, const QByteArray& data, bool byteIsVisible) const;
    void emitSelectionRangeChanged();
    void showSelectionContextMenu(const QPoint& localPos);
    int tokenVisualWidth(const Token& token) const;
    QColor colorForClass(TextByteClass cls) const;
    void layoutChildren();
    void paintContent();
    void paintGutter();
    void emitCenterAnchorOffset();
    quint64 currentCenterAnchorOffset() const;
    bool handleWheelEvent(QWheelEvent* event);
    unsigned char byteAtAbsoluteOffset(quint64 absoluteOffset) const;

    QByteArray m_bytes;
    QByteArray m_backingBytes;
    QVector<TextByteClass> m_byteClasses;
    QVector<bool> m_stringVisibilityMask;
    QVector<quint64> m_visibleOffsets;
    std::optional<unsigned char> m_previousByteBeforeBase;
    std::optional<unsigned char> m_previousByteBeforeBackingBase;
    quint64 m_baseOffset = 0;
    quint64 m_backingBaseOffset = 0;
    quint64 m_selectedOffset = 0;
    quint64 m_matchStartOffset = 0;
    quint32 m_matchLength = 0;
    TextInterpretationMode m_mode = TextInterpretationMode::Ascii;
    TextDisplayMode m_displayMode = TextDisplayMode::StringMode;
    TextNewlineMode m_newlineMode = TextNewlineMode::Nl;
    ByteLineMode m_byteLineMode = ByteLineMode::Auto;
    bool m_utf16LittleEndian = true;
    bool m_gutterVisible = true;
    bool m_wrapMode = true;
    bool m_monospaceEnabled = false;
    bool m_hasSelectedOffset = false;
    quint64 m_lastEmittedCenterAnchor = 0;
    qint64 m_lastHoveredAbsoluteOffset = -1;
    bool m_selecting = false;
    bool m_hasSelection = false;
    int m_selectionStartVisibleIndex = -1;
    int m_selectionEndVisibleIndex = -1;

    QVector<DisplayLine> m_lines;
    QWidget* m_gutterWidget = nullptr;
    QWidget* m_contentWidget = nullptr;
    QScrollBar* m_vScrollBar = nullptr;
    QScrollBar* m_hScrollBar = nullptr;
};

}  // namespace breco
