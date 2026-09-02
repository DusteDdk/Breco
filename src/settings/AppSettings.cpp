#include "settings/AppSettings.h"

#include <QSettings>
#include <QVariant>

#include <memory>

namespace breco {

namespace {
constexpr const char* kOrg = "breco";
constexpr const char* kApp = "breco";
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
constexpr const char* kVisualizeModeIndexKey = "ui/visualizeModeIndex";
constexpr const char* kViewScanLogVisibleKey = "ui/viewScanLogVisible";
constexpr const char* kViewEditsVisibleKey = "ui/viewEditsVisible";
constexpr const char* kLastBrecoLangSchemaPathKey =
    "ui/lastBrecoLangSchemaPath";
constexpr const char* kBrecoLangLibraryDirectoryKey =
    "ui/brecoLangLibraryDirectory";

QList<int> readIntList(const char* key) {
    const auto settings = AppSettings::open();
    const QVariantList raw = settings->value(key).toList();
    QList<int> sizes;
    sizes.reserve(raw.size());
    for (const QVariant& value : raw) {
        sizes.push_back(value.toInt());
    }
    return sizes;
}

void writeIntList(const char* key, const QList<int>& values) {
    const auto settings = AppSettings::open();
    QVariantList raw;
    raw.reserve(values.size());
    for (const int value : values) {
        raw.push_back(value);
    }
    settings->setValue(key, raw);
}

QString lastSelectedPathKey(const QString& activityKey) {
    return QStringLiteral("ui/lastSelectedPath/%1").arg(activityKey);
}

QString& isolatedIniPath() {
    static QString path;
    return path;
}
}  // namespace

void AppSettings::useIsolatedIni(const QString& filePath) {
    isolatedIniPath() = filePath;
}

std::unique_ptr<QSettings> AppSettings::open() {
    if (!isolatedIniPath().isEmpty()) {
        return std::make_unique<QSettings>(isolatedIniPath(), QSettings::IniFormat);
    }
    return std::make_unique<QSettings>(kOrg, kApp);
}

QString AppSettings::lastSelectedPath(const QString& activityKey) {
    const auto settings = AppSettings::open();
    return settings->value(lastSelectedPathKey(activityKey), QString()).toString();
}

QString AppSettings::rememberedSingleFilePath() {
    const auto settings = AppSettings::open();
    return settings->value(kRememberedSingleFilePathKey, QString()).toString();
}

quint64 AppSettings::rememberedSingleFileOffset() {
    const auto settings = AppSettings::open();
    return settings->value(kRememberedSingleFileOffsetKey, 0).toULongLong();
}

void AppSettings::setLastSelectedPath(const QString& activityKey,
                                      const QString& path) {
    const auto settings = AppSettings::open();
    settings->setValue(lastSelectedPathKey(activityKey), path);
}

void AppSettings::setRememberedSingleFilePath(const QString& path) {
    const auto settings = AppSettings::open();
    settings->setValue(kRememberedSingleFilePathKey, path);
}

void AppSettings::setRememberedSingleFileOffset(quint64 offset) {
    const auto settings = AppSettings::open();
    settings->setValue(kRememberedSingleFileOffsetKey,
                      QVariant::fromValue<qulonglong>(offset));
}

void AppSettings::clearRememberedSingleFilePath() {
    const auto settings = AppSettings::open();
    settings->remove(kRememberedSingleFilePathKey);
}

void AppSettings::clearRememberedSingleFileOffset() {
    const auto settings = AppSettings::open();
    settings->remove(kRememberedSingleFileOffsetKey);
}

bool AppSettings::textByteModeEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kTextByteModeKey, false).toBool();
}

bool AppSettings::textWrapModeEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kTextWrapModeKey, true).toBool();
}

bool AppSettings::textCollapseEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kTextCollapseKey, true).toBool();
}

bool AppSettings::textBreatheEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kTextBreatheKey, false).toBool();
}

bool AppSettings::textMonospaceEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kTextMonospaceKey, false).toBool();
}

int AppSettings::textNewlineModeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kTextNewlineModeIndexKey, 1).toInt();
}

int AppSettings::textByteLineModeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kTextByteLineModeIndexKey, 1).toInt();
}

bool AppSettings::prefillOnMergeEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kPrefillOnMergeEnabledKey, true).toBool();
}

int AppSettings::scanBlockSizeValue(int defaultValue) {
    const auto settings = AppSettings::open();
    return settings->value(kScanBlockSizeValueKey, defaultValue).toInt();
}

int AppSettings::scanBlockSizeUnitIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kScanBlockSizeUnitIndexKey, 2).toInt();
}

QList<int> AppSettings::contentSplitterSizes() {
    return readIntList(kContentSplitterSizesKey);
}

QList<int> AppSettings::mainSplitterSizes() {
    return readIntList(kMainSplitterSizesKey);
}

int AppSettings::textGutterFormatIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kTextGutterFormatIndexKey, 1).toInt();
}

int AppSettings::textGutterWidth() {
    const auto settings = AppSettings::open();
    return settings->value(kTextGutterWidthKey, 110).toInt();
}

int AppSettings::currentByteInfoNumberSystemIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kCurrentByteInfoNumberSystemIndexKey, 0).toInt();
}

bool AppSettings::currentByteInfoBigEndianEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kCurrentByteInfoBigEndianEnabledKey, true).toBool();
}

int AppSettings::hexShowAsIndex() {
    const auto settings = AppSettings::open();
    if (!settings->contains(kHexShowAsIndexKey)) {
        if (settings->contains(kTextByteModeKey)) {
            return settings->value(kTextByteModeKey).toBool() ? 0 : 1;
        }
        return 4;
    }
    return settings->value(kHexShowAsIndexKey, 0).toInt();
}

bool AppSettings::hexBigEndianEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kHexBigEndianEnabledKey, false).toBool();
}

bool AppSettings::hexStringsOnlyEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kHexStringsOnlyEnabledKey, false).toBool();
}

bool AppSettings::hexHighlightResultEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kHexHighlightResultEnabledKey, true).toBool();
}

bool AppSettings::dataViewBigEndianEnabled() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewBigEndianEnabledKey, false).toBool();
}

int AppSettings::dataViewTextModeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewTextModeIndexKey, 0).toInt();
}

int AppSettings::dataViewBitmapModeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewBitmapModeIndexKey, 0).toInt();
}

int AppSettings::dataViewBitmapZoom() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewBitmapZoomKey, 1).toInt();
}

int AppSettings::dataViewImageFormatMask(int defaultMask) {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewImageFormatMaskKey, defaultMask).toInt();
}

int AppSettings::dataViewImageScopeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewImageScopeIndexKey, 0).toInt();
}

int AppSettings::dataViewImageMaxPixelsK() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewImageMaxPixelsKKey, 4096).toInt();
}

int AppSettings::dataViewImageMaxResults() {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewImageMaxResultsKey, 5).toInt();
}

int AppSettings::dataViewImageJobs(int defaultValue) {
    const auto settings = AppSettings::open();
    return settings->value(kDataViewImageJobsKey, defaultValue).toInt();
}

QList<int> AppSettings::dataViewByteAndBitmapSplitterSizes() {
    return readIntList(kDataViewByteAndBitmapSplitterSizesKey);
}

int AppSettings::visualizeModeIndex() {
    const auto settings = AppSettings::open();
    return settings->value(kVisualizeModeIndexKey, 0).toInt();
}

bool AppSettings::viewScanLogVisible() {
    const auto settings = AppSettings::open();
    return settings->value(kViewScanLogVisibleKey, false).toBool();
}

bool AppSettings::viewEditsVisible() {
    const auto settings = AppSettings::open();
    return settings->value(kViewEditsVisibleKey, false).toBool();
}

QString AppSettings::lastBrecoLangSchemaPath() {
    const auto settings = AppSettings::open();
    return settings->value(kLastBrecoLangSchemaPathKey, QString()).toString();
}

QString AppSettings::brecoLangLibraryDirectory() {
    const auto settings = AppSettings::open();
    return settings->value(kBrecoLangLibraryDirectoryKey, QString()).toString();
}

void AppSettings::setTextByteModeEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextByteModeKey, enabled);
}

void AppSettings::setTextWrapModeEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextWrapModeKey, enabled);
}

void AppSettings::setTextCollapseEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextCollapseKey, enabled);
}

void AppSettings::setTextBreatheEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextBreatheKey, enabled);
}

void AppSettings::setTextMonospaceEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextMonospaceKey, enabled);
}

void AppSettings::setTextNewlineModeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextNewlineModeIndexKey, index);
}

void AppSettings::setTextByteLineModeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextByteLineModeIndexKey, index);
}

void AppSettings::setPrefillOnMergeEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kPrefillOnMergeEnabledKey, enabled);
}

void AppSettings::setScanBlockSizeValue(int value) {
    const auto settings = AppSettings::open();
    settings->setValue(kScanBlockSizeValueKey, value);
}

void AppSettings::setScanBlockSizeUnitIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kScanBlockSizeUnitIndexKey, index);
}

void AppSettings::setContentSplitterSizes(const QList<int>& sizes) {
    writeIntList(kContentSplitterSizesKey, sizes);
}

void AppSettings::setMainSplitterSizes(const QList<int>& sizes) {
    writeIntList(kMainSplitterSizesKey, sizes);
}

void AppSettings::setTextGutterFormatIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextGutterFormatIndexKey, index);
}

void AppSettings::setTextGutterWidth(int width) {
    const auto settings = AppSettings::open();
    settings->setValue(kTextGutterWidthKey, width);
}

void AppSettings::setCurrentByteInfoNumberSystemIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kCurrentByteInfoNumberSystemIndexKey, index);
}

void AppSettings::setCurrentByteInfoBigEndianEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kCurrentByteInfoBigEndianEnabledKey, enabled);
}

void AppSettings::setHexShowAsIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kHexShowAsIndexKey, index);
}

void AppSettings::setHexBigEndianEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kHexBigEndianEnabledKey, enabled);
}

void AppSettings::setHexStringsOnlyEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kHexStringsOnlyEnabledKey, enabled);
}

void AppSettings::setHexHighlightResultEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kHexHighlightResultEnabledKey, enabled);
}

void AppSettings::setDataViewBigEndianEnabled(bool enabled) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewBigEndianEnabledKey, enabled);
}

void AppSettings::setDataViewTextModeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewTextModeIndexKey, index);
}

void AppSettings::setDataViewBitmapModeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewBitmapModeIndexKey, index);
}

void AppSettings::setDataViewBitmapZoom(int zoom) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewBitmapZoomKey, zoom);
}

void AppSettings::setDataViewImageFormatMask(int mask) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewImageFormatMaskKey, mask);
}

void AppSettings::setDataViewImageScopeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewImageScopeIndexKey, index);
}

void AppSettings::setDataViewImageMaxPixelsK(int value) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewImageMaxPixelsKKey, value);
}

void AppSettings::setDataViewImageMaxResults(int value) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewImageMaxResultsKey, value);
}

void AppSettings::setDataViewImageJobs(int value) {
    const auto settings = AppSettings::open();
    settings->setValue(kDataViewImageJobsKey, value);
}

void AppSettings::setDataViewByteAndBitmapSplitterSizes(const QList<int>& sizes) {
    writeIntList(kDataViewByteAndBitmapSplitterSizesKey, sizes);
}

void AppSettings::setVisualizeModeIndex(int index) {
    const auto settings = AppSettings::open();
    settings->setValue(kVisualizeModeIndexKey, index);
}

void AppSettings::setViewScanLogVisible(bool visible) {
    const auto settings = AppSettings::open();
    settings->setValue(kViewScanLogVisibleKey, visible);
}

void AppSettings::setViewEditsVisible(bool visible) {
    const auto settings = AppSettings::open();
    settings->setValue(kViewEditsVisibleKey, visible);
}

void AppSettings::setLastBrecoLangSchemaPath(const QString& path) {
    const auto settings = AppSettings::open();
    settings->setValue(kLastBrecoLangSchemaPathKey, path);
}

void AppSettings::setBrecoLangLibraryDirectory(const QString& path) {
    const auto settings = AppSettings::open();
    settings->setValue(kBrecoLangLibraryDirectoryKey, path);
}

}  // namespace breco
