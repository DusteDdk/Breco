#include "settings/AppSettings.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QVariant>

namespace breco {

namespace {
constexpr const char* kOrg = "breco";
constexpr const char* kApp = "breco";
constexpr const char* kLastFilePathKey = "ui/lastFileDialogPath";
constexpr const char* kLastDirPathKey = "ui/lastDirectoryDialogPath";
constexpr const char* kLastBrowseDialogDirectoryKey = "ui/lastBrowseDialogDirectory";
constexpr const char* kRememberedSingleFilePathKey = "ui/rememberedSingleFilePath";
constexpr const char* kRememberedSingleFileOffsetKey =
    "ui/rememberedSingleFileOffset";
constexpr const char* kTextByteModeKey = "ui/textByteModeEnabled";
constexpr const char* kTextWrapModeKey = "ui/textWrapModeEnabled";
constexpr const char* kTextCollapseKey = "ui/textCollapseEnabled";
constexpr const char* kTextBreatheKey = "ui/textBreatheEnabled";
constexpr const char* kTextMonospaceKey = "ui/textMonospaceEnabled";
constexpr const char* kTextNewlineModeIndexKey = "ui/textNewlineModeIndex";
constexpr const char* kTextByteLineModeIndexKey = "ui/textByteLineModeIndex";
constexpr const char* kPrefillOnMergeEnabledKey = "ui/prefillOnMergeEnabled";
constexpr const char* kScanBlockSizeValueKey = "ui/scanBlockSizeValue";
constexpr const char* kScanBlockSizeUnitIndexKey = "ui/scanBlockSizeUnitIndex";
constexpr const char* kContentSplitterSizesKey = "ui/contentSplitterSizes";
constexpr const char* kMainSplitterSizesKey = "ui/mainSplitterSizes";
constexpr const char* kTextGutterFormatIndexKey = "ui/textGutterFormatIndex";
constexpr const char* kTextGutterWidthKey = "ui/textGutterWidth";
constexpr const char* kCurrentByteInfoNumberSystemIndexKey = "ui/currentByteInfoNumberSystemIndex";
constexpr const char* kCurrentByteInfoBigEndianEnabledKey = "ui/currentByteInfoBigEndianEnabled";
constexpr const char* kHexShowAsIndexKey = "ui/hexShowAsIndex";
constexpr const char* kHexBigEndianEnabledKey = "ui/hexBigEndianEnabled";
constexpr const char* kHexStringsOnlyEnabledKey = "ui/hexStringsOnlyEnabled";
constexpr const char* kHexHighlightResultEnabledKey = "ui/hexHighlightResultEnabled";
constexpr const char* kDataViewBigEndianEnabledKey = "ui/dataViewBigEndianEnabled";
constexpr const char* kDataViewTextModeIndexKey = "ui/dataViewTextModeIndex";
constexpr const char* kDataViewBitmapModeIndexKey = "ui/dataViewBitmapModeIndex";
constexpr const char* kDataViewBitmapZoomKey = "ui/dataViewBitmapZoom";
constexpr const char* kDataViewImageFormatMaskKey = "ui/dataViewImageFormatMask";
constexpr const char* kDataViewImageScopeIndexKey = "ui/dataViewImageScopeIndex";
constexpr const char* kDataViewImageMaxPixelsKKey = "ui/dataViewImageMaxPixelsK";
constexpr const char* kDataViewImageMaxResultsKey = "ui/dataViewImageMaxResults";
constexpr const char* kDataViewImageJobsKey = "ui/dataViewImageJobs";
constexpr const char* kDataViewByteAndBitmapSplitterSizesKey =
    "ui/dataViewByteAndBitmapSplitterSizes";
constexpr const char* kViewScanLogVisibleKey = "ui/viewScanLogVisible";
constexpr const char* kViewEditsVisibleKey = "ui/viewEditsVisible";
constexpr const char* kLastBrecoLangSchemaPathKey =
    "ui/lastBrecoLangSchemaPath";
constexpr const char* kBrecoLangLibraryDirectoryKey =
    "ui/brecoLangLibraryDirectory";

QList<int> readIntList(const char* key) {
    QSettings settings(kOrg, kApp);
    const QVariantList raw = settings.value(key).toList();
    QList<int> sizes;
    sizes.reserve(raw.size());
    for (const QVariant& value : raw) {
        sizes.push_back(value.toInt());
    }
    return sizes;
}

void writeIntList(const char* key, const QList<int>& values) {
    QSettings settings(kOrg, kApp);
    QVariantList raw;
    raw.reserve(values.size());
    for (const int value : values) {
        raw.push_back(value);
    }
    settings.setValue(key, raw);
}

QString directoryFromPath(const QString& path) {
    if (path.isEmpty()) {
        return QString();
    }
    const QFileInfo info(path);
    if (info.isDir()) {
        return info.absoluteFilePath();
    }
    return info.absolutePath();
}
}  // namespace

QString AppSettings::lastFileDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastFilePathKey, QDir::homePath()).toString();
}

QString AppSettings::lastDirectoryDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastDirPathKey, QDir::homePath()).toString();
}

QString AppSettings::lastBrowseDialogDirectory() {
    QSettings settings(kOrg, kApp);
    if (settings.contains(kLastBrowseDialogDirectoryKey)) {
        return settings.value(kLastBrowseDialogDirectoryKey, QDir::homePath()).toString();
    }
    const QString fromFilePath = directoryFromPath(
        settings.value(kLastFilePathKey).toString());
    if (!fromFilePath.isEmpty()) {
        return fromFilePath;
    }
    const QString fromDirPath = settings.value(kLastDirPathKey).toString();
    if (!fromDirPath.isEmpty()) {
        return fromDirPath;
    }
    return QDir::homePath();
}

QString AppSettings::rememberedSingleFilePath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kRememberedSingleFilePathKey, QString()).toString();
}

quint64 AppSettings::rememberedSingleFileOffset() {
    QSettings settings(kOrg, kApp);
    return settings.value(kRememberedSingleFileOffsetKey, 0).toULongLong();
}

void AppSettings::setLastFileDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastFilePathKey, path);
}

void AppSettings::setLastDirectoryDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastDirPathKey, path);
}

void AppSettings::setLastBrowseDialogDirectory(const QString& path) {
    const QString directory = directoryFromPath(path);
    if (directory.isEmpty()) {
        return;
    }
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastBrowseDialogDirectoryKey, directory);
}

void AppSettings::setRememberedSingleFilePath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kRememberedSingleFilePathKey, path);
}

void AppSettings::setRememberedSingleFileOffset(quint64 offset) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kRememberedSingleFileOffsetKey,
                      QVariant::fromValue<qulonglong>(offset));
}

void AppSettings::clearRememberedSingleFilePath() {
    QSettings settings(kOrg, kApp);
    settings.remove(kRememberedSingleFilePathKey);
}

void AppSettings::clearRememberedSingleFileOffset() {
    QSettings settings(kOrg, kApp);
    settings.remove(kRememberedSingleFileOffsetKey);
}

bool AppSettings::textByteModeEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextByteModeKey, false).toBool();
}

bool AppSettings::textWrapModeEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextWrapModeKey, true).toBool();
}

bool AppSettings::textCollapseEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextCollapseKey, true).toBool();
}

bool AppSettings::textBreatheEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextBreatheKey, false).toBool();
}

bool AppSettings::textMonospaceEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextMonospaceKey, false).toBool();
}

int AppSettings::textNewlineModeIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextNewlineModeIndexKey, 1).toInt();
}

int AppSettings::textByteLineModeIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextByteLineModeIndexKey, 1).toInt();
}

bool AppSettings::prefillOnMergeEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kPrefillOnMergeEnabledKey, true).toBool();
}

int AppSettings::scanBlockSizeValue(int defaultValue) {
    QSettings settings(kOrg, kApp);
    return settings.value(kScanBlockSizeValueKey, defaultValue).toInt();
}

int AppSettings::scanBlockSizeUnitIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kScanBlockSizeUnitIndexKey, 2).toInt();
}

QList<int> AppSettings::contentSplitterSizes() {
    return readIntList(kContentSplitterSizesKey);
}

QList<int> AppSettings::mainSplitterSizes() {
    return readIntList(kMainSplitterSizesKey);
}

int AppSettings::textGutterFormatIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextGutterFormatIndexKey, 1).toInt();
}

int AppSettings::textGutterWidth() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextGutterWidthKey, 110).toInt();
}

int AppSettings::currentByteInfoNumberSystemIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kCurrentByteInfoNumberSystemIndexKey, 0).toInt();
}

bool AppSettings::currentByteInfoBigEndianEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kCurrentByteInfoBigEndianEnabledKey, true).toBool();
}

int AppSettings::hexShowAsIndex() {
    QSettings settings(kOrg, kApp);
    if (!settings.contains(kHexShowAsIndexKey)) {
        if (settings.contains(kTextByteModeKey)) {
            return settings.value(kTextByteModeKey).toBool() ? 0 : 1;
        }
        return 4;
    }
    return settings.value(kHexShowAsIndexKey, 0).toInt();
}

bool AppSettings::hexBigEndianEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kHexBigEndianEnabledKey, false).toBool();
}

bool AppSettings::hexStringsOnlyEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kHexStringsOnlyEnabledKey, false).toBool();
}

bool AppSettings::hexHighlightResultEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kHexHighlightResultEnabledKey, true).toBool();
}

bool AppSettings::dataViewBigEndianEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewBigEndianEnabledKey, false).toBool();
}

int AppSettings::dataViewTextModeIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewTextModeIndexKey, 0).toInt();
}

int AppSettings::dataViewBitmapModeIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewBitmapModeIndexKey, 0).toInt();
}

int AppSettings::dataViewBitmapZoom() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewBitmapZoomKey, 1).toInt();
}

int AppSettings::dataViewImageFormatMask(int defaultMask) {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewImageFormatMaskKey, defaultMask).toInt();
}

int AppSettings::dataViewImageScopeIndex() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewImageScopeIndexKey, 0).toInt();
}

int AppSettings::dataViewImageMaxPixelsK() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewImageMaxPixelsKKey, 4096).toInt();
}

int AppSettings::dataViewImageMaxResults() {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewImageMaxResultsKey, 5).toInt();
}

int AppSettings::dataViewImageJobs(int defaultValue) {
    QSettings settings(kOrg, kApp);
    return settings.value(kDataViewImageJobsKey, defaultValue).toInt();
}

QList<int> AppSettings::dataViewByteAndBitmapSplitterSizes() {
    return readIntList(kDataViewByteAndBitmapSplitterSizesKey);
}

bool AppSettings::viewScanLogVisible() {
    QSettings settings(kOrg, kApp);
    return settings.value(kViewScanLogVisibleKey, false).toBool();
}

bool AppSettings::viewEditsVisible() {
    QSettings settings(kOrg, kApp);
    return settings.value(kViewEditsVisibleKey, false).toBool();
}

QString AppSettings::lastBrecoLangSchemaPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastBrecoLangSchemaPathKey, QString()).toString();
}

QString AppSettings::brecoLangLibraryDirectory() {
    QSettings settings(kOrg, kApp);
    return settings.value(kBrecoLangLibraryDirectoryKey, QString()).toString();
}

void AppSettings::setTextByteModeEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextByteModeKey, enabled);
}

void AppSettings::setTextWrapModeEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextWrapModeKey, enabled);
}

void AppSettings::setTextCollapseEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextCollapseKey, enabled);
}

void AppSettings::setTextBreatheEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextBreatheKey, enabled);
}

void AppSettings::setTextMonospaceEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextMonospaceKey, enabled);
}

void AppSettings::setTextNewlineModeIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextNewlineModeIndexKey, index);
}

void AppSettings::setTextByteLineModeIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextByteLineModeIndexKey, index);
}

void AppSettings::setPrefillOnMergeEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kPrefillOnMergeEnabledKey, enabled);
}

void AppSettings::setScanBlockSizeValue(int value) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kScanBlockSizeValueKey, value);
}

void AppSettings::setScanBlockSizeUnitIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kScanBlockSizeUnitIndexKey, index);
}

void AppSettings::setContentSplitterSizes(const QList<int>& sizes) {
    writeIntList(kContentSplitterSizesKey, sizes);
}

void AppSettings::setMainSplitterSizes(const QList<int>& sizes) {
    writeIntList(kMainSplitterSizesKey, sizes);
}

void AppSettings::setTextGutterFormatIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextGutterFormatIndexKey, index);
}

void AppSettings::setTextGutterWidth(int width) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextGutterWidthKey, width);
}

void AppSettings::setCurrentByteInfoNumberSystemIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kCurrentByteInfoNumberSystemIndexKey, index);
}

void AppSettings::setCurrentByteInfoBigEndianEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kCurrentByteInfoBigEndianEnabledKey, enabled);
}

void AppSettings::setHexShowAsIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kHexShowAsIndexKey, index);
}

void AppSettings::setHexBigEndianEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kHexBigEndianEnabledKey, enabled);
}

void AppSettings::setHexStringsOnlyEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kHexStringsOnlyEnabledKey, enabled);
}

void AppSettings::setHexHighlightResultEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kHexHighlightResultEnabledKey, enabled);
}

void AppSettings::setDataViewBigEndianEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewBigEndianEnabledKey, enabled);
}

void AppSettings::setDataViewTextModeIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewTextModeIndexKey, index);
}

void AppSettings::setDataViewBitmapModeIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewBitmapModeIndexKey, index);
}

void AppSettings::setDataViewBitmapZoom(int zoom) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewBitmapZoomKey, zoom);
}

void AppSettings::setDataViewImageFormatMask(int mask) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewImageFormatMaskKey, mask);
}

void AppSettings::setDataViewImageScopeIndex(int index) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewImageScopeIndexKey, index);
}

void AppSettings::setDataViewImageMaxPixelsK(int value) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewImageMaxPixelsKKey, value);
}

void AppSettings::setDataViewImageMaxResults(int value) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewImageMaxResultsKey, value);
}

void AppSettings::setDataViewImageJobs(int value) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kDataViewImageJobsKey, value);
}

void AppSettings::setDataViewByteAndBitmapSplitterSizes(const QList<int>& sizes) {
    writeIntList(kDataViewByteAndBitmapSplitterSizesKey, sizes);
}

void AppSettings::setViewScanLogVisible(bool visible) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kViewScanLogVisibleKey, visible);
}

void AppSettings::setViewEditsVisible(bool visible) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kViewEditsVisibleKey, visible);
}

void AppSettings::setLastBrecoLangSchemaPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastBrecoLangSchemaPathKey, path);
}

void AppSettings::setBrecoLangLibraryDirectory(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kBrecoLangLibraryDirectoryKey, path);
}

}  // namespace breco
