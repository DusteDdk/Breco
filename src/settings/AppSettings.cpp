#include "settings/AppSettings.h"

#include <QDir>
#include <QSettings>

namespace breco {

namespace {
constexpr const char* kOrg = "breco";
constexpr const char* kApp = "breco";
constexpr const char* kLastFilePathKey = "ui/lastFileDialogPath";
constexpr const char* kLastDirPathKey = "ui/lastDirectoryDialogPath";
constexpr const char* kRememberedSingleFilePathKey = "ui/rememberedSingleFilePath";
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
constexpr const char* kViewScanLogVisibleKey = "ui/viewScanLogVisible";
constexpr const char* kViewEditsVisibleKey = "ui/viewEditsVisible";
constexpr const char* kViewControlsVisibleKey = "ui/viewControlsVisible";
}  // namespace

QString AppSettings::lastFileDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastFilePathKey, QDir::homePath()).toString();
}

QString AppSettings::lastDirectoryDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastDirPathKey, QDir::homePath()).toString();
}

QString AppSettings::rememberedSingleFilePath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kRememberedSingleFilePathKey, QString()).toString();
}

void AppSettings::setLastFileDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastFilePathKey, path);
}

void AppSettings::setLastDirectoryDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastDirPathKey, path);
}

void AppSettings::setRememberedSingleFilePath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kRememberedSingleFilePathKey, path);
}

void AppSettings::clearRememberedSingleFilePath() {
    QSettings settings(kOrg, kApp);
    settings.remove(kRememberedSingleFilePathKey);
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
    return settings.value(kTextByteLineModeIndexKey, 4).toInt();
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
    QSettings settings(kOrg, kApp);
    const QVariantList raw = settings.value(kContentSplitterSizesKey).toList();
    QList<int> sizes;
    sizes.reserve(raw.size());
    for (const QVariant& value : raw) {
        sizes.push_back(value.toInt());
    }
    return sizes;
}

QList<int> AppSettings::mainSplitterSizes() {
    QSettings settings(kOrg, kApp);
    const QVariantList raw = settings.value(kMainSplitterSizesKey).toList();
    QList<int> sizes;
    sizes.reserve(raw.size());
    for (const QVariant& value : raw) {
        sizes.push_back(value.toInt());
    }
    return sizes;
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

bool AppSettings::viewScanLogVisible() {
    QSettings settings(kOrg, kApp);
    return settings.value(kViewScanLogVisibleKey, false).toBool();
}

bool AppSettings::viewEditsVisible() {
    QSettings settings(kOrg, kApp);
    return settings.value(kViewEditsVisibleKey, false).toBool();
}

bool AppSettings::viewControlsVisible() {
    QSettings settings(kOrg, kApp);
    return settings.value(kViewControlsVisibleKey, false).toBool();
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
    QSettings settings(kOrg, kApp);
    QVariantList raw;
    raw.reserve(sizes.size());
    for (const int size : sizes) {
        raw.push_back(size);
    }
    settings.setValue(kContentSplitterSizesKey, raw);
}

void AppSettings::setMainSplitterSizes(const QList<int>& sizes) {
    QSettings settings(kOrg, kApp);
    QVariantList raw;
    raw.reserve(sizes.size());
    for (const int size : sizes) {
        raw.push_back(size);
    }
    settings.setValue(kMainSplitterSizesKey, raw);
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

void AppSettings::setViewScanLogVisible(bool visible) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kViewScanLogVisibleKey, visible);
}

void AppSettings::setViewEditsVisible(bool visible) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kViewEditsVisibleKey, visible);
}

void AppSettings::setViewControlsVisible(bool visible) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kViewControlsVisibleKey, visible);
}

}  // namespace breco
