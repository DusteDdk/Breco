#pragma once

#include <QString>

namespace breco {

class AppSettings {
public:
    static QString lastFileDialogPath();
    static QString lastDirectoryDialogPath();
    static void setLastFileDialogPath(const QString& path);
    static void setLastDirectoryDialogPath(const QString& path);
    static bool textByteModeEnabled();
    static bool textWrapModeEnabled();
    static bool textMonospaceEnabled();
    static int textNewlineModeIndex();
    static int textByteLineModeIndex();
    static bool prefillOnMergeEnabled();
    static void setTextByteModeEnabled(bool enabled);
    static void setTextWrapModeEnabled(bool enabled);
    static void setTextMonospaceEnabled(bool enabled);
    static void setTextNewlineModeIndex(int index);
    static void setTextByteLineModeIndex(int index);
    static void setPrefillOnMergeEnabled(bool enabled);
};

}  // namespace breco
