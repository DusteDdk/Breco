#pragma once

#include <QList>
#include <QString>

namespace breco {

class AppSettings {
public:
    static QString lastFileDialogPath();
    static QString lastDirectoryDialogPath();
    static QString lastBrowseDialogDirectory();
    static QString lastStructDefinitionDialogDirectory();
    static QString rememberedSingleFilePath();
    static quint64 rememberedSingleFileOffset();
    static void setLastFileDialogPath(const QString& path);
    static void setLastDirectoryDialogPath(const QString& path);
    static void setLastBrowseDialogDirectory(const QString& path);
    static void setRememberedSingleFilePath(const QString& path);
    static void setRememberedSingleFileOffset(quint64 offset);
    static void clearRememberedSingleFilePath();
    static void clearRememberedSingleFileOffset();
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
    static int hexShowAsIndex();
    static bool hexBigEndianEnabled();
    static bool hexStringsOnlyEnabled();
    static bool hexHighlightResultEnabled();
    static int dataViewModeIndex();
    static bool dataViewBigEndianEnabled();
    static int dataViewTextModeIndex();
    static int dataViewBitmapModeIndex();
    static int dataViewBitmapZoom();
    static int dataViewImageFormatMask(int defaultMask);
    static int dataViewImageScopeIndex();
    static int dataViewImageMaxPixelsK();
    static int dataViewImageMaxResults();
    static int dataViewImageJobs(int defaultValue);
    static QList<int> dataViewByteAndBitmapSplitterSizes();
    static QList<int> dataViewStructuredSplitterSizes();
    static bool viewScanLogVisible();
    static bool viewEditsVisible();
    static QString lastStructDefinitionFilePath();
    static QString structDeclarationText();
    static QString structEntryName();
    static int structEntryCount();
    static bool structPreviewEnabled();
    static bool structViewsVisible();
    static bool structLanguageVisible();
    static void setLastStructDefinitionFilePath(const QString& path);
    static void setStructDeclarationText(const QString& text);
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
    static void setHexShowAsIndex(int index);
    static void setHexBigEndianEnabled(bool enabled);
    static void setHexStringsOnlyEnabled(bool enabled);
    static void setHexHighlightResultEnabled(bool enabled);
    static void setDataViewModeIndex(int index);
    static void setDataViewBigEndianEnabled(bool enabled);
    static void setDataViewTextModeIndex(int index);
    static void setDataViewBitmapModeIndex(int index);
    static void setDataViewBitmapZoom(int zoom);
    static void setDataViewImageFormatMask(int mask);
    static void setDataViewImageScopeIndex(int index);
    static void setDataViewImageMaxPixelsK(int value);
    static void setDataViewImageMaxResults(int value);
    static void setDataViewImageJobs(int value);
    static void setDataViewByteAndBitmapSplitterSizes(const QList<int>& sizes);
    static void setDataViewStructuredSplitterSizes(const QList<int>& sizes);
    static void setViewScanLogVisible(bool visible);
    static void setViewEditsVisible(bool visible);
    static void setStructEntryName(const QString& name);
    static void setStructEntryCount(int count);
    static void setStructPreviewEnabled(bool enabled);
    static void setStructViewsVisible(bool visible);
    static void setStructLanguageVisible(bool visible);
};

}  // namespace breco
