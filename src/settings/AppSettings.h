#pragma once

#include <QList>
#include <QString>

namespace breco {

class AppSettings {
public:
    static QString lastFileDialogPath();
    static QString lastDirectoryDialogPath();
    static QString rememberedSingleFilePath();
    static void setLastFileDialogPath(const QString& path);
    static void setLastDirectoryDialogPath(const QString& path);
    static void setRememberedSingleFilePath(const QString& path);
    static void clearRememberedSingleFilePath();
    static bool textByteModeEnabled();
    static bool textWrapModeEnabled();
    static bool textCollapseEnabled();
    static bool textBreatheEnabled();
    static bool textMonospaceEnabled();
    static int textNewlineModeIndex();
    static int textByteLineModeIndex();
    static bool prefillOnMergeEnabled();
    static int scanBlockSizeValue(int defaultValue);
    static int scanBlockSizeUnitIndex();
    static QList<int> contentSplitterSizes();
    static QList<int> mainSplitterSizes();
    static int textGutterFormatIndex();
    static int textGutterWidth();
    static int currentByteInfoNumberSystemIndex();
    static bool currentByteInfoBigEndianEnabled();
    static bool viewScanLogVisible();
    static bool viewEditsVisible();
    static bool viewControlsVisible();
    static void setTextByteModeEnabled(bool enabled);
    static void setTextWrapModeEnabled(bool enabled);
    static void setTextCollapseEnabled(bool enabled);
    static void setTextBreatheEnabled(bool enabled);
    static void setTextMonospaceEnabled(bool enabled);
    static void setTextNewlineModeIndex(int index);
    static void setTextByteLineModeIndex(int index);
    static void setPrefillOnMergeEnabled(bool enabled);
    static void setScanBlockSizeValue(int value);
    static void setScanBlockSizeUnitIndex(int index);
    static void setContentSplitterSizes(const QList<int>& sizes);
    static void setMainSplitterSizes(const QList<int>& sizes);
    static void setTextGutterFormatIndex(int index);
    static void setTextGutterWidth(int width);
    static void setCurrentByteInfoNumberSystemIndex(int index);
    static void setCurrentByteInfoBigEndianEnabled(bool enabled);
    static void setViewScanLogVisible(bool visible);
    static void setViewEditsVisible(bool visible);
    static void setViewControlsVisible(bool visible);
};

}  // namespace breco
