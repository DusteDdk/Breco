#include "settings/AppSettings.h"

#include <QDir>
#include <QSettings>

namespace breco {

namespace {
constexpr const char* kOrg = "breco";
constexpr const char* kApp = "breco";
constexpr const char* kLastFilePathKey = "ui/lastFileDialogPath";
constexpr const char* kLastDirPathKey = "ui/lastDirectoryDialogPath";
constexpr const char* kTextByteModeKey = "ui/textByteModeEnabled";
constexpr const char* kTextWrapModeKey = "ui/textWrapModeEnabled";
constexpr const char* kTextMonospaceKey = "ui/textMonospaceEnabled";
constexpr const char* kTextNewlineModeIndexKey = "ui/textNewlineModeIndex";
constexpr const char* kTextByteLineModeIndexKey = "ui/textByteLineModeIndex";
constexpr const char* kPrefillOnMergeEnabledKey = "ui/prefillOnMergeEnabled";
}  // namespace

QString AppSettings::lastFileDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastFilePathKey, QDir::homePath()).toString();
}

QString AppSettings::lastDirectoryDialogPath() {
    QSettings settings(kOrg, kApp);
    return settings.value(kLastDirPathKey, QDir::homePath()).toString();
}

void AppSettings::setLastFileDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastFilePathKey, path);
}

void AppSettings::setLastDirectoryDialogPath(const QString& path) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kLastDirPathKey, path);
}

bool AppSettings::textByteModeEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextByteModeKey, false).toBool();
}

bool AppSettings::textWrapModeEnabled() {
    QSettings settings(kOrg, kApp);
    return settings.value(kTextWrapModeKey, true).toBool();
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

void AppSettings::setTextByteModeEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextByteModeKey, enabled);
}

void AppSettings::setTextWrapModeEnabled(bool enabled) {
    QSettings settings(kOrg, kApp);
    settings.setValue(kTextWrapModeKey, enabled);
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

}  // namespace breco
